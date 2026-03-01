// SPDX-License-Identifier: MIT
/*
 * bionilux_preload.c — LD_PRELOAD library for bionilux
 *
 * Intercepts exec*() calls so that child processes spawned by a glibc
 * binary are transparently routed through the Termux glibc loader.
 * Also fixes /proc/self/exe readlink so programs can locate their own
 * resources.
 *
 * Loaded into arm64 glibc processes only (never into box64 x86_64).
 *
 * Build (against glibc sysroot):
 *   clang --sysroot=$PREFIX/glibc -shared -fPIC -O2 -Wall -Wextra \
 *         -o libbionilux_preload.so bionilux_preload.c -lc -ldl
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "bionilux_elf.h"

/* ── environment variable names ──────────────────────────────────── */

#define GLIBC_LIB_ENV		"BIONILUX_GLIBC_LIB"
#define GLIBC_LOADER_ENV	"BIONILUX_GLIBC_LOADER"
#define BIONILUX_DEBUG_ENV	"BIONILUX_DEBUG"
#define BIONILUX_ORIG_EXE_ENV	"BIONILUX_ORIG_EXE"

/* compile-time prefix match for environment variables */
#define ENVPREFIX(var, lit)	(strncmp((var), (lit), sizeof(lit) - 1) == 0)

/* ── debug logging ───────────────────────────────────────────────── */

static int debug_enabled;

__attribute__((format(printf, 1, 2)))
static void debug_print(const char *fmt, ...)
{
	if (!debug_enabled)
		return;

	va_list ap;

	va_start(ap, fmt);
	fprintf(stderr, "[bionilux] ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ── real function pointers (set in constructor) ─────────────────── */

static int     (*real_execve)(const char *, char *const[], char *const[]);
static ssize_t (*real_readlink)(const char *, char *, size_t);
static ssize_t (*real_readlinkat)(int, const char *, char *, size_t);

/*
 * Fallback execve via raw syscall — used when dlsym(RTLD_NEXT) fails.
 * This is the ultimate safety net: even if the dynamic linker is
 * misconfigured, we can still exec.
 */
static int fallback_execve(const char *path, char *const argv[],
			   char *const envp[])
{
	return (int)syscall(SYS_execve, path, argv, envp);
}

/*
 * Safe wrapper: calls real_execve if resolved, otherwise falls back
 * to the raw syscall.  Never crashes on NULL function pointer.
 */
static inline int safe_execve(const char *path, char *const argv[],
			      char *const envp[])
{
	if (real_execve)
		return real_execve(path, argv, envp);
	return fallback_execve(path, argv, envp);
}

/* ── path resolution ─────────────────────────────────────────────── */

/*
 * Resolve @path to an absolute path.
 *
 *   Absolute        → copy as-is
 *   Bare name       → search $PATH
 *   Relative with / → prepend CWD
 *
 * Always returns @resolved (caller-owned buffer of PATH_MAX bytes).
 */
static char *resolve_path(const char *path, char *resolved)
{
	if (path[0] == '/') {
		snprintf(resolved, PATH_MAX, "%s", path);
		return resolved;
	}

	if (!strchr(path, '/')) {
		const char *path_env = getenv("PATH");

		if (path_env) {
			char *dup = strdup(path_env);

			if (dup) {
				char *saveptr;
				char *dir;

				for (dir = strtok_r(dup, ":", &saveptr);
				     dir;
				     dir = strtok_r(NULL, ":", &saveptr)) {
					snprintf(resolved, PATH_MAX,
						 "%s/%s", dir, path);
					if (access(resolved, X_OK) == 0) {
						free(dup);
						return resolved;
					}
				}
				free(dup);
			}
		}
	}

	/* relative path with '/' → prepend CWD */
	{
		char cwd[PATH_MAX];

		if (getcwd(cwd, sizeof(cwd))) {
			snprintf(resolved, PATH_MAX, "%s/%s", cwd, path);
			return resolved;
		}
	}

	snprintf(resolved, PATH_MAX, "%s", path);
	return resolved;
}

/* ── argv / envp builders ────────────────────────────────────────── */

static void free_strarray(char **arr)
{
	if (!arr)
		return;
	for (int i = 0; arr[i]; i++)
		free(arr[i]);
	free(arr);
}

/*
 * Build argv for the glibc loader invocation:
 *   loader --library-path lib --argv0 argv[0] binary [argv[1]...]
 */
static char **build_loader_argv(const char *loader, const char *lib_path,
				const char *binary, char *const argv[])
{
	size_t argc = 0;
	size_t new_argc, k;
	char **av;

	while (argv[argc])
		argc++;

	new_argc = 6 + (argc > 0 ? argc - 1 : 0);
	av = calloc(new_argc + 1, sizeof(char *));
	if (!av)
		return NULL;

	k = 0;
	av[k++] = strdup(loader);
	av[k++] = strdup("--library-path");
	av[k++] = strdup(lib_path);
	av[k++] = strdup("--argv0");
	av[k++] = strdup(argv[0] ? argv[0] : binary);
	av[k++] = strdup(binary);

	for (size_t i = 1; i < argc; i++)
		av[k++] = strdup(argv[i]);
	av[k] = NULL;

	/* check for OOM */
	for (size_t i = 0; i < k; i++) {
		if (!av[i]) {
			free_strarray(av);
			return NULL;
		}
	}

	return av;
}

/*
 * Build envp for glibc child: keep everything, update BIONILUX_ORIG_EXE.
 * Strips libtermux-exec from LD_PRELOAD (bionic-only, breaks glibc).
 */
static char **build_new_envp(char *const envp[], const char *orig_exe)
{
	size_t envc = 0;
	size_t j = 0;
	size_t len;
	char **ev;

	while (envp[envc])
		envc++;

	ev = calloc(envc + 2, sizeof(char *));
	if (!ev)
		return NULL;

	for (size_t i = 0; i < envc; i++) {
		/* skip bionic-only LD_PRELOAD (libtermux-exec) */
		if (ENVPREFIX(envp[i], "LD_PRELOAD=") &&
		    strstr(envp[i], "libtermux-exec"))
			continue;

		/* skip existing BIONILUX_ORIG_EXE */
		if (ENVPREFIX(envp[i], BIONILUX_ORIG_EXE_ENV "="))
			continue;

		ev[j] = strdup(envp[i]);
		if (!ev[j]) {
			free_strarray(ev);
			return NULL;
		}
		j++;
	}

	/* add BIONILUX_ORIG_EXE */
	len = sizeof(BIONILUX_ORIG_EXE_ENV) + strlen(orig_exe) + 1;
	ev[j] = malloc(len);
	if (!ev[j]) {
		free_strarray(ev);
		return NULL;
	}
	snprintf(ev[j], len, "%s=%s", BIONILUX_ORIG_EXE_ENV, orig_exe);
	j++;

	ev[j] = NULL;
	return ev;
}

/*
 * Build a cleaned envp for non-glibc (bionic) child processes.
 * Removes glibc paths from LD_LIBRARY_PATH and the bionilux LD_PRELOAD.
 * Also strips LD_AUDIT and LD_DEBUG inherited from the glibc environment
 * — they are glibc-specific and meaningless (or harmful) under bionic.
 */
static char **build_clean_envp(char *const envp[])
{
	const char *glibc_lib = getenv(GLIBC_LIB_ENV);
	size_t envc = 0;
	size_t j = 0;
	char **ev;

	while (envp[envc])
		envc++;

	ev = calloc(envc + 1, sizeof(char *));
	if (!ev)
		return NULL;

	for (size_t i = 0; i < envc; i++) {
		/* filter glibc paths from LD_LIBRARY_PATH */
		if (ENVPREFIX(envp[i], "LD_LIBRARY_PATH=") &&
		    glibc_lib && strstr(envp[i], glibc_lib)) {
			const char *value = envp[i] + 16;
			char *dup = strdup(value);

			if (!dup) {
				/* OOM fallback: copy as-is */
				ev[j] = strdup(envp[i]);
				if (!ev[j]) {
					free_strarray(ev);
					return NULL;
				}
				j++;
				continue;
			}

			char result[PATH_MAX * 4];
			size_t off = 0;
			char *saveptr;

			for (char *tok = strtok_r(dup, ":", &saveptr);
			     tok;
			     tok = strtok_r(NULL, ":", &saveptr)) {
				if (strstr(tok, glibc_lib))
					continue;
				int n = snprintf(result + off,
						 sizeof(result) - off,
						 "%s%s",
						 off ? ":" : "", tok);
				if (n > 0 &&
				    (size_t)n < sizeof(result) - off)
					off += (size_t)n;
			}
			free(dup);

			if (off > 0) {
				size_t sz = 17 + off + 1;

				ev[j] = malloc(sz);
				if (!ev[j]) {
					free_strarray(ev);
					return NULL;
				}
				snprintf(ev[j], sz,
					 "LD_LIBRARY_PATH=%s", result);
				j++;
			}
			continue;
		}

		/* remove glibc preload */
		if (ENVPREFIX(envp[i], "LD_PRELOAD=") &&
		    strstr(envp[i], "libbionilux_preload")) {
			debug_print("stripping glibc LD_PRELOAD for "
				    "bionic child");
			continue;
		}

		/*
		 * Strip LD_AUDIT and LD_DEBUG — glibc-specific,
		 * meaningless and potentially harmful under bionic.
		 */
		if (ENVPREFIX(envp[i], "LD_AUDIT=") ||
		    ENVPREFIX(envp[i], "LD_DEBUG="))
			continue;

		ev[j] = strdup(envp[i]);
		if (!ev[j]) {
			free_strarray(ev);
			return NULL;
		}
		j++;
	}

	ev[j] = NULL;
	return ev;
}

/* ── hooked exec functions ───────────────────────────────────────── */

/*
 * Central execve hook — all other exec wrappers funnel through here.
 *
 * Logic:
 *   1. If BIONILUX env vars are not set → pass through.
 *   2. Resolve the binary path.
 *   3. If it is a glibc ELF → rewrite argv to go through the loader.
 *   4. Otherwise → clean the environment and exec normally.
 */
int execve(const char *pathname, char *const argv[], char *const envp[])
{
	const char *glibc_lib    = getenv(GLIBC_LIB_ENV);
	const char *glibc_loader = getenv(GLIBC_LOADER_ENV);
	char resolved[PATH_MAX];
	int glibc_bin;

	if (!glibc_lib || !glibc_loader) {
		debug_print("BIONILUX env vars not set, pass-through");
		return safe_execve(pathname, argv, envp);
	}

	resolve_path(pathname, resolved);
	debug_print("execve: %s -> %s", pathname, resolved);

	glibc_bin = is_glibc_elf(resolved, glibc_lib);
	if (glibc_bin != 1) {
		debug_print("not glibc (result=%d), cleaning env", glibc_bin);

		char **clean = build_clean_envp(envp);

		if (clean) {
			int ret = safe_execve(pathname, argv, clean);
			int e = errno;

			free_strarray(clean);
			errno = e;
			return ret;
		}
		return safe_execve(pathname, argv, envp);
	}

	debug_print("glibc binary detected, redirecting through loader");

	char **new_argv = build_loader_argv(glibc_loader, glibc_lib,
					    resolved, argv);
	if (!new_argv)
		return safe_execve(pathname, argv, envp);

	char **new_envp = build_new_envp(envp, resolved);

	if (!new_envp) {
		free_strarray(new_argv);
		return safe_execve(pathname, argv, envp);
	}

	debug_print("exec: %s %s %s %s %s %s",
		    new_argv[0], new_argv[1], new_argv[2],
		    new_argv[3], new_argv[4], new_argv[5]);

	int ret = safe_execve(glibc_loader, new_argv, new_envp);
	int e = errno;

	free_strarray(new_argv);
	free_strarray(new_envp);
	errno = e;
	return ret;
}

int execv(const char *pathname, char *const argv[])
{
	return execve(pathname, argv, environ);
}

int execvp(const char *file, char *const argv[])
{
	char resolved[PATH_MAX];

	resolve_path(file, resolved);
	return execve(resolved, argv, environ);
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
	char resolved[PATH_MAX];

	resolve_path(file, resolved);
	return execve(resolved, argv, envp);
}

/*
 * Variadic exec wrappers — convert va_list to argv[], then call
 * through our hooked execve / execvp.
 *
 * These are necessary because glibc's internal implementation of
 * execl() may bypass our execve() hook by calling __execve directly.
 */
int execl(const char *pathname, const char *arg, ...)
{
	va_list ap;
	size_t argc = 1;
	char **argv;
	int ret;

	/* count arguments (including the first, excluding trailing NULL) */
	va_start(ap, arg);
	while (va_arg(ap, const char *))
		argc++;
	va_end(ap);

	argv = calloc(argc + 1, sizeof(char *));
	if (!argv) {
		errno = ENOMEM;
		return -1;
	}

	argv[0] = (char *)arg;
	va_start(ap, arg);
	for (size_t i = 1; i < argc; i++)
		argv[i] = va_arg(ap, char *);
	va_end(ap);
	argv[argc] = NULL;

	ret = execv(pathname, argv);
	free(argv);
	return ret;
}

int execlp(const char *file, const char *arg, ...)
{
	va_list ap;
	size_t argc = 1;
	char **argv;
	int ret;

	va_start(ap, arg);
	while (va_arg(ap, const char *))
		argc++;
	va_end(ap);

	argv = calloc(argc + 1, sizeof(char *));
	if (!argv) {
		errno = ENOMEM;
		return -1;
	}

	argv[0] = (char *)arg;
	va_start(ap, arg);
	for (size_t i = 1; i < argc; i++)
		argv[i] = va_arg(ap, char *);
	va_end(ap);
	argv[argc] = NULL;

	ret = execvp(file, argv);
	free(argv);
	return ret;
}

int execle(const char *pathname, const char *arg, ... /*, char *const envp[] */)
{
	va_list ap;
	size_t argc = 1;
	char **argv;
	char *const *envp;
	int ret;

	/* count args (the envp pointer follows the trailing NULL) */
	va_start(ap, arg);
	while (va_arg(ap, const char *))
		argc++;
	va_end(ap);

	argv = calloc(argc + 1, sizeof(char *));
	if (!argv) {
		errno = ENOMEM;
		return -1;
	}

	argv[0] = (char *)arg;
	va_start(ap, arg);
	for (size_t i = 1; i < argc; i++)
		argv[i] = va_arg(ap, char *);
	envp = va_arg(ap, char *const *);
	va_end(ap);
	argv[argc] = NULL;

	ret = execve(pathname, argv, envp);
	free(argv);
	return ret;
}

/* ── hooked readlink / readlinkat ────────────────────────────────── */

/*
 * Check whether the combination of @dirfd + @pathname refers to
 * /proc/self/exe.  Handles:
 *   - readlink("/proc/self/exe", ...)
 *   - readlinkat(AT_FDCWD, "/proc/self/exe", ...)
 *   - readlinkat(fd_for_proc_self, "exe", ...)
 */
static int is_proc_self_exe(int dirfd, const char *pathname)
{
	if (strcmp(pathname, "/proc/self/exe") == 0)
		return 1;

	/*
	 * Handle readlinkat(fd, "exe", ...) where fd points to
	 * /proc/self/.  Some programs open the directory and use
	 * relative paths.
	 */
	if (dirfd != AT_FDCWD && strcmp(pathname, "exe") == 0) {
		char fd_path[64];
		char link_target[PATH_MAX];
		ssize_t len;

		snprintf(fd_path, sizeof(fd_path),
			 "/proc/self/fd/%d", dirfd);
		/* use real_readlink to avoid recursion through our hook */
		len = real_readlink
		      ? real_readlink(fd_path, link_target,
				      sizeof(link_target) - 1)
		      : -1;
		if (len > 0) {
			link_target[len] = '\0';
			if (strcmp(link_target, "/proc/self") == 0 ||
			    strcmp(link_target, "/proc/self/") == 0)
				return 1;
		}
	}

	return 0;
}

ssize_t readlink(const char *pathname, char *buf, size_t bufsiz)
{
	ssize_t ret;

	if (!real_readlink) {
		errno = ENOSYS;
		return -1;
	}

	ret = real_readlink(pathname, buf, bufsiz);

	if (ret > 0 && strcmp(pathname, "/proc/self/exe") == 0) {
		const char *orig = getenv(BIONILUX_ORIG_EXE_ENV);

		if (orig) {
			size_t len = strlen(orig);

			if (len < bufsiz) {
				memcpy(buf, orig, len);
				debug_print("readlink(/proc/self/exe)"
					    " -> %s", orig);
				return (ssize_t)len;
			}
		}
	}

	return ret;
}

ssize_t readlinkat(int dirfd, const char *pathname,
		   char *buf, size_t bufsiz)
{
	ssize_t ret;

	if (!real_readlinkat) {
		errno = ENOSYS;
		return -1;
	}

	ret = real_readlinkat(dirfd, pathname, buf, bufsiz);

	if (ret > 0 && is_proc_self_exe(dirfd, pathname)) {
		const char *orig = getenv(BIONILUX_ORIG_EXE_ENV);

		if (orig) {
			size_t len = strlen(orig);

			if (len < bufsiz) {
				memcpy(buf, orig, len);
				debug_print("readlinkat(%d, %s) -> %s",
					    dirfd, pathname, orig);
				return (ssize_t)len;
			}
		}
	}

	return ret;
}

/* ── constructor ─────────────────────────────────────────────────── */

__attribute__((constructor))
static void init(void)
{
	/*
	 * Use *(void **)& to assign dlsym results to function pointers
	 * without triggering -Wpedantic warnings about void* → fptr
	 * conversion (which is technically undefined in ISO C but
	 * required by POSIX dlsym).
	 */
	*(void **)&real_execve = dlsym(RTLD_NEXT, "execve");
	if (!real_execve)
		fprintf(stderr, "[bionilux] WARNING: dlsym(execve) "
			"failed: %s — using syscall fallback\n",
			dlerror() ? dlerror() : "unknown");

	*(void **)&real_readlink = dlsym(RTLD_NEXT, "readlink");
	if (!real_readlink)
		fprintf(stderr, "[bionilux] WARNING: dlsym(readlink) "
			"failed: %s\n",
			dlerror() ? dlerror() : "unknown");

	*(void **)&real_readlinkat = dlsym(RTLD_NEXT, "readlinkat");
	if (!real_readlinkat)
		fprintf(stderr, "[bionilux] WARNING: dlsym(readlinkat) "
			"failed: %s\n",
			dlerror() ? dlerror() : "unknown");

	debug_enabled = (getenv(BIONILUX_DEBUG_ENV) != NULL);

	debug_print("bionilux_preload loaded (pid=%d)", (int)getpid());
}

// SPDX-License-Identifier: MIT
/*
 * ovgl_preload.c — LD_PRELOAD library for ovgl
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
 *         -o libovgl_preload.so ovgl_preload.c -lc -ldl
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── environment variable names ──────────────────────────────────── */

#define GLIBC_LIB_ENV     "OVGL_GLIBC_LIB"
#define GLIBC_LOADER_ENV  "OVGL_GLIBC_LOADER"
#define OVGL_DEBUG_ENV    "OVGL_DEBUG"
#define OVGL_ORIG_EXE_ENV "OVGL_ORIG_EXE"

/* ── debug logging ───────────────────────────────────────────────── */

static int debug_enabled;

static void debug_print(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

static void debug_print(const char *fmt, ...)
{
	if (!debug_enabled)
		return;

	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "[ovgl] ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

/* ── cached real function pointers (set in constructor) ──────────── */

static int     (*real_execve)(const char *, char *const[], char *const[]);
static ssize_t (*real_readlink)(const char *, char *, size_t);
static ssize_t (*real_readlinkat)(int, const char *, char *, size_t);

/* ── ELF inspection ──────────────────────────────────────────────── */

/*
 * Check whether @path is a glibc ELF that needs to be routed through
 * the Termux glibc loader.
 *
 * Returns:  1  → glibc binary (redirect through loader)
 *           0  → not glibc / not ELF / static / already set up
 *          -1  → I/O error
 *
 * Uses pread() to avoid the 4096-byte-buffer limitation.
 */
static int is_glibc_elf(const char *path)
{
	Elf64_Ehdr ehdr;
	int fd, ret = 0;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	if (pread(fd, &ehdr, sizeof(ehdr), 0) != sizeof(ehdr))
		goto out;

	if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0)
		goto out;
	if (ehdr.e_ident[EI_CLASS] != ELFCLASS64)
		goto out;
	if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN)
		goto out;
	if (ehdr.e_phoff == 0 || ehdr.e_phnum == 0)
		goto out;

	for (unsigned i = 0; i < ehdr.e_phnum; i++) {
		Elf64_Phdr phdr;
		off_t off = (off_t)(ehdr.e_phoff + (Elf64_Off)i * ehdr.e_phentsize);

		if (pread(fd, &phdr, sizeof(phdr), off) != sizeof(phdr))
			break;

		if (phdr.p_type != PT_INTERP)
			continue;

		if (phdr.p_filesz == 0 || phdr.p_filesz > PATH_MAX)
			break;

		char interp[PATH_MAX];
		if (pread(fd, interp, phdr.p_filesz, (off_t)phdr.p_offset)
		    != (ssize_t)phdr.p_filesz)
			break;

		interp[phdr.p_filesz - 1] = '\0'; /* ensure NUL */

		debug_print("interpreter: %s", interp);

		/*
		 * musl ≠ glibc.  They are ABI-incompatible.
		 * Do NOT redirect musl binaries through the glibc loader.
		 */
		if (strstr(interp, "ld-musl"))
			break;

		if (strstr(interp, "ld-linux")) {
			/*
			 * If the interpreter already points into the Termux
			 * glibc prefix we're set up — no redirect needed.
			 */
			const char *glibc_lib = getenv(GLIBC_LIB_ENV);
			if (glibc_lib && strstr(interp, glibc_lib)) {
				debug_print("already using Termux glibc loader");
				break;
			}
			ret = 1;
		}
		break; /* PT_INTERP found, stop */
	}

out:
	close(fd);
	return ret;
}

/* ── path resolution ─────────────────────────────────────────────── */

/*
 * Resolve @path to an absolute path.
 *
 * - Absolute: copy as-is.
 * - Bare name (no '/'): search PATH.
 * - Relative with '/': prepend CWD.
 *
 * Returns @resolved on success, NULL on failure.
 */
static char *resolve_path(const char *path, char *resolved)
{
	if (path[0] == '/') {
		snprintf(resolved, PATH_MAX, "%s", path);
		return resolved;
	}

	if (!strchr(path, '/')) {
		/* bare name → PATH lookup */
		const char *path_env = getenv("PATH");
		if (path_env) {
			char *dup = strdup(path_env);
			if (dup) {
				char *saveptr;
				for (char *dir = strtok_r(dup, ":", &saveptr);
				     dir;
				     dir = strtok_r(NULL, ":", &saveptr)) {
					snprintf(resolved, PATH_MAX, "%s/%s",
						 dir, path);
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
	char cwd[PATH_MAX];
	if (getcwd(cwd, sizeof(cwd))) {
		snprintf(resolved, PATH_MAX, "%s/%s", cwd, path);
		return resolved;
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
	while (argv[argc])
		argc++;

	size_t new_argc = 6 + (argc > 0 ? argc - 1 : 0);
	char **av = calloc(new_argc + 1, sizeof(char *));
	if (!av)
		return NULL;

	size_t k = 0;
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
 * Build envp for glibc child: keep everything, update OVGL_ORIG_EXE.
 */
static char **build_new_envp(char *const envp[], const char *orig_exe)
{
	size_t envc = 0;
	while (envp[envc])
		envc++;

	char **ev = calloc(envc + 2, sizeof(char *));
	if (!ev)
		return NULL;

	size_t j = 0;
	for (size_t i = 0; i < envc; i++) {
		/* skip bionic-only LD_PRELOAD (libtermux-exec) */
		if (strncmp(envp[i], "LD_PRELOAD=", 11) == 0 &&
		    strstr(envp[i], "libtermux-exec"))
			continue;

		/* skip existing OVGL_ORIG_EXE */
		if (strncmp(envp[i], OVGL_ORIG_EXE_ENV "=",
			    strlen(OVGL_ORIG_EXE_ENV "=")) == 0)
			continue;

		ev[j] = strdup(envp[i]);
		if (!ev[j]) { free_strarray(ev); return NULL; }
		j++;
	}

	/* add OVGL_ORIG_EXE */
	size_t len = strlen(OVGL_ORIG_EXE_ENV) + 1 + strlen(orig_exe) + 1;
	ev[j] = malloc(len);
	if (!ev[j]) { free_strarray(ev); return NULL; }
	snprintf(ev[j], len, "%s=%s", OVGL_ORIG_EXE_ENV, orig_exe);
	j++;

	ev[j] = NULL;
	return ev;
}

/*
 * Build a cleaned envp for non-glibc (bionic) child processes.
 * Removes glibc paths from LD_LIBRARY_PATH and the glibc LD_PRELOAD.
 */
static char **build_clean_envp(char *const envp[])
{
	const char *glibc_lib = getenv(GLIBC_LIB_ENV);
	size_t envc = 0;
	while (envp[envc])
		envc++;

	char **ev = calloc(envc + 1, sizeof(char *));
	if (!ev)
		return NULL;

	size_t j = 0;
	for (size_t i = 0; i < envc; i++) {

		/* filter glibc paths from LD_LIBRARY_PATH */
		if (strncmp(envp[i], "LD_LIBRARY_PATH=", 16) == 0 &&
		    glibc_lib && strstr(envp[i], glibc_lib)) {
			const char *value = envp[i] + 16;
			char *dup = strdup(value);
			if (!dup) {
				/* OOM fallback: copy as-is */
				ev[j] = strdup(envp[i]);
				if (!ev[j]) { free_strarray(ev); return NULL; }
				j++;
				continue;
			}

			/* rebuild without glibc components */
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
				if (n > 0 && (size_t)n < sizeof(result) - off)
					off += (size_t)n;
			}
			free(dup);

			if (off > 0) {
				size_t sz = 17 + off + 1;
				ev[j] = malloc(sz);
				if (!ev[j]) { free_strarray(ev); return NULL; }
				snprintf(ev[j], sz, "LD_LIBRARY_PATH=%s", result);
				j++;
			}
			continue;
		}

		/* remove glibc preload */
		if (strncmp(envp[i], "LD_PRELOAD=", 11) == 0 &&
		    strstr(envp[i], "libovgl_preload")) {
			debug_print("removing glibc LD_PRELOAD for bionic child");
			continue;
		}

		ev[j] = strdup(envp[i]);
		if (!ev[j]) { free_strarray(ev); return NULL; }
		j++;
	}

	ev[j] = NULL;
	return ev;
}

/* ── hooked exec functions ───────────────────────────────────────── */

int execve(const char *pathname, char *const argv[], char *const envp[])
{
	const char *glibc_lib    = getenv(GLIBC_LIB_ENV);
	const char *glibc_loader = getenv(GLIBC_LOADER_ENV);

	if (!glibc_lib || !glibc_loader) {
		debug_print("OVGL env vars not set, pass-through");
		return real_execve(pathname, argv, envp);
	}

	char resolved[PATH_MAX];
	resolve_path(pathname, resolved);

	debug_print("execve: %s → %s", pathname, resolved);

	int glibc = is_glibc_elf(resolved);
	if (glibc != 1) {
		debug_print("not glibc (result=%d), cleaning env", glibc);

		char **clean = build_clean_envp(envp);
		if (clean) {
			int ret = real_execve(pathname, argv, clean);
			int e = errno;
			free_strarray(clean);
			errno = e;
			return ret;
		}
		return real_execve(pathname, argv, envp);
	}

	debug_print("glibc binary detected, redirecting through loader");

	char **new_argv = build_loader_argv(glibc_loader, glibc_lib,
					    resolved, argv);
	if (!new_argv)
		return real_execve(pathname, argv, envp);

	char **new_envp = build_new_envp(envp, resolved);
	if (!new_envp) {
		free_strarray(new_argv);
		return real_execve(pathname, argv, envp);
	}

	debug_print("exec: %s %s %s %s %s %s",
		    new_argv[0], new_argv[1], new_argv[2],
		    new_argv[3], new_argv[4], new_argv[5]);

	int ret = real_execve(glibc_loader, new_argv, new_envp);
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

/* ── hooked readlink / readlinkat ────────────────────────────────── */

ssize_t readlink(const char *pathname, char *buf, size_t bufsiz)
{
	ssize_t ret = real_readlink(pathname, buf, bufsiz);

	if (ret > 0 && strcmp(pathname, "/proc/self/exe") == 0) {
		const char *orig = getenv(OVGL_ORIG_EXE_ENV);
		if (orig) {
			size_t len = strlen(orig);
			if (len < bufsiz) {
				memcpy(buf, orig, len);
				debug_print("readlink(/proc/self/exe) → %s",
					    orig);
				return (ssize_t)len;
			}
		}
	}

	return ret;
}

ssize_t readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsiz)
{
	ssize_t ret = real_readlinkat(dirfd, pathname, buf, bufsiz);

	if (ret > 0 && strcmp(pathname, "/proc/self/exe") == 0) {
		const char *orig = getenv(OVGL_ORIG_EXE_ENV);
		if (orig) {
			size_t len = strlen(orig);
			if (len < bufsiz) {
				memcpy(buf, orig, len);
				debug_print("readlinkat(/proc/self/exe) → %s",
					    orig);
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
	/* cache real function pointers once, thread-safely */
	*(void **)&real_execve     = dlsym(RTLD_NEXT, "execve");
	*(void **)&real_readlink   = dlsym(RTLD_NEXT, "readlink");
	*(void **)&real_readlinkat = dlsym(RTLD_NEXT, "readlinkat");

	debug_enabled = (getenv(OVGL_DEBUG_ENV) != NULL);

	debug_print("ovgl_preload loaded");
}

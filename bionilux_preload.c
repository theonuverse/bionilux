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

#define DEFAULT_GLIBC_PREFIX "/data/data/com.termux/files/usr/glibc"

/* compile-time prefix match for environment variables */
#define ENVPREFIX(var, lit)	(strncmp((var), (lit), sizeof(lit) - 1) == 0)

/* ── debug logging ───────────────────────────────────────────────── */

static int debug_enabled;

static void constructor_warn(const char *symbol, const char *err)
{
	char buf[256];
	int n;

	n = snprintf(buf, sizeof(buf),
		     "[bionilux] WARNING: dlsym(%s) failed: %s\n",
		     symbol, err ? err : "unknown");
	if (n > 0) {
		size_t out = (size_t)n;

		if (out > sizeof(buf))
			out = sizeof(buf);
		write(STDERR_FILENO, buf, out);
	}
}

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
static int     (*real_execveat)(int, const char *, char *const[],
				char *const[], int);
static int     (*real_fexecve)(int, char *const[], char *const[]);
static ssize_t (*real_readlink)(const char *, char *, size_t);
static ssize_t (*real_readlinkat)(int, const char *, char *, size_t);
static int     (*real_open)(const char *, int, ...);
static int     (*real_open64)(const char *, int, ...);
static int     (*real_openat)(int, const char *, int, ...);
static int     (*real_openat64)(int, const char *, int, ...);

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

static int fallback_execveat(int dirfd, const char *path,
			     char *const argv[], char *const envp[],
			     int flags)
{
#ifdef SYS_execveat
	return (int)syscall(SYS_execveat, dirfd, path, argv, envp, flags);
#else
	(void)dirfd;
	(void)path;
	(void)argv;
	(void)envp;
	(void)flags;
	errno = ENOSYS;
	return -1;
#endif
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

static inline int safe_execveat(int dirfd, const char *path,
				char *const argv[], char *const envp[],
				int flags)
{
	if (real_execveat)
		return real_execveat(dirfd, path, argv, envp, flags);
	return fallback_execveat(dirfd, path, argv, envp, flags);
}


/* ── preload modules ────────────────────────────────────────────── */

#include "src/preload/path_env.c"
#include "src/preload/hooks_exec.c"
#include "src/preload/hooks_io.c"
#include "src/preload/ctor.c"

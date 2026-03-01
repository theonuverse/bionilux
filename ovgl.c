// SPDX-License-Identifier: MIT
/*
 * ovgl - Run unpatched glibc/x86_64 binaries on Android Termux
 *
 * Native bionic executable that detects binary architecture, invokes
 * the glibc dynamic linker for arm64 glibc binaries, or box64 for
 * x86_64 binaries.  Embeds a preload library for seamless child
 * process support.
 *
 * Build (bionic, on Termux):
 *   clang -O2 -Wall -Wextra -Wpedantic -o ovgl ovgl.c -DEMBED_PRELOAD
 */

#define _GNU_SOURCE
#include <errno.h>
#include <elf.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── version ─────────────────────────────────────────────────────── */

#define OVGL_VERSION "0.2.0"

/* ── paths ───────────────────────────────────────────────────────── */

#define GLIBC_PREFIX  "/data/data/com.termux/files/usr/glibc"
#define GLIBC_LIB     GLIBC_PREFIX "/lib"
#define GLIBC_LOADER  GLIBC_LIB "/ld-linux-aarch64.so.1"

/*
 * x86_64 libraries live under the standard Linux multiarch path so
 * that box64 picks them up automatically.
 */
#define GLIBC_LIB_X86 GLIBC_PREFIX "/lib/x86_64-linux-gnu"

/* ── colours (stderr only) ───────────────────────────────────────── */

#define C_RED    "\033[0;31m"
#define C_GREEN  "\033[0;32m"
#define C_YELLOW "\033[1;33m"
#define C_BLUE   "\033[0;34m"
#define C_RESET  "\033[0m"

/* ── logging helpers ─────────────────────────────────────────────── */

#define msg_info(...) \
	do { fprintf(stderr, C_BLUE   "ovgl: " C_RESET __VA_ARGS__); \
	     fputc('\n', stderr); } while (0)
#define msg_warn(...) \
	do { fprintf(stderr, C_YELLOW "ovgl: " C_RESET __VA_ARGS__); \
	     fputc('\n', stderr); } while (0)
#define msg_err(...)  \
	do { fprintf(stderr, C_RED    "ovgl: " C_RESET __VA_ARGS__); \
	     fputc('\n', stderr); } while (0)
#define msg_ok(...)   \
	do { fprintf(stderr, C_GREEN  "ovgl: " C_RESET __VA_ARGS__); \
	     fputc('\n', stderr); } while (0)

/* ── embedded preload library ────────────────────────────────────── */

#ifdef EMBED_PRELOAD
#include "preload_data.h"
#else
static const unsigned char preload_so_data[] = {0};
static const unsigned int  preload_so_size   = 0;
#endif

/* ── architecture / interpreter enums ────────────────────────────── */

typedef enum {
	ARCH_UNKNOWN = 0,
	ARCH_AARCH64,
	ARCH_X86_64,
	ARCH_NOT_ELF,
	ARCH_ERROR,
} elf_arch_t;

typedef enum {
	INTERP_NONE = 0,
	INTERP_GLIBC,
	INTERP_BIONIC,
	INTERP_MUSL,
	INTERP_OTHER,
} interp_type_t;

typedef struct {
	elf_arch_t    arch;
	interp_type_t interp;
	char          interp_path[256];
} binary_info_t;

/* ── ELF analysis ────────────────────────────────────────────────── */

/*
 * Read the ELF header and PT_INTERP from @path using pread() so that
 * we never miss program headers that sit beyond a small initial read.
 */
static binary_info_t analyze_binary(const char *path)
{
	binary_info_t info = { .arch = ARCH_ERROR };
	Elf64_Ehdr ehdr;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return info;

	/* ── read ELF header ──────────────────────────────────────── */
	if (pread(fd, &ehdr, sizeof(ehdr), 0) != sizeof(ehdr)) {
		info.arch = ARCH_NOT_ELF;
		goto out;
	}

	if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
		info.arch = ARCH_NOT_ELF;
		goto out;
	}

	if (ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
		info.arch = ARCH_UNKNOWN;
		goto out;
	}

	switch (ehdr.e_machine) {
	case EM_AARCH64: info.arch = ARCH_AARCH64; break;
	case EM_X86_64:  info.arch = ARCH_X86_64;  break;
	default:         info.arch = ARCH_UNKNOWN;  goto out;
	}

	/* ── walk program headers ─────────────────────────────────── */
	if (ehdr.e_phoff == 0 || ehdr.e_phnum == 0) {
		info.interp = INTERP_NONE;
		goto out;
	}

	for (unsigned i = 0; i < ehdr.e_phnum; i++) {
		Elf64_Phdr phdr;
		off_t off = (off_t)(ehdr.e_phoff + (Elf64_Off)i * ehdr.e_phentsize);

		if (pread(fd, &phdr, sizeof(phdr), off) != sizeof(phdr))
			break;

		if (phdr.p_type != PT_INTERP)
			continue;

		if (phdr.p_filesz == 0 || phdr.p_filesz >= sizeof(info.interp_path))
			break;

		if (pread(fd, info.interp_path, phdr.p_filesz,
			   (off_t)phdr.p_offset) != (ssize_t)phdr.p_filesz)
			break;

		info.interp_path[phdr.p_filesz] = '\0';

		if (strstr(info.interp_path, "ld-linux"))
			info.interp = INTERP_GLIBC;
		else if (strstr(info.interp_path, "linker64") ||
			 strstr(info.interp_path, "linker"))
			info.interp = INTERP_BIONIC;
		else if (strstr(info.interp_path, "ld-musl"))
			info.interp = INTERP_MUSL;
		else
			info.interp = INTERP_OTHER;
		break;
	}

out:
	close(fd);
	return info;
}

/* ── path resolution ─────────────────────────────────────────────── */

/*
 * Resolve @name to an executable path.  Behaviour follows POSIX:
 *   - contains '/' → treat as relative/absolute path directly
 *   - bare name    → search $PATH only (never implicit CWD)
 *
 * Returns @resolved on success, NULL on failure.
 */
static char *find_in_path(const char *name, char *resolved, size_t size)
{
	if (strchr(name, '/') != NULL) {
		if (name[0] == '/') {
			snprintf(resolved, size, "%s", name);
		} else {
			char cwd[PATH_MAX];
			if (!getcwd(cwd, sizeof(cwd)))
				return NULL;
			snprintf(resolved, size, "%s/%s", cwd, name);
		}
		return access(resolved, X_OK) == 0 ? resolved : NULL;
	}

	/* bare name → search PATH only */
	const char *path_env = getenv("PATH");
	if (!path_env)
		return NULL;

	char *dup = strdup(path_env);
	if (!dup)
		return NULL;

	char *saveptr;
	for (char *dir = strtok_r(dup, ":", &saveptr);
	     dir;
	     dir = strtok_r(NULL, ":", &saveptr)) {
		snprintf(resolved, size, "%s/%s", dir, name);
		if (access(resolved, X_OK) == 0) {
			free(dup);
			return resolved;
		}
	}

	free(dup);
	return NULL;
}

static const char *get_prefix(void)
{
	const char *p = getenv("PREFIX");
	return p ? p : "/data/data/com.termux/files/usr";
}

static char *find_box64(char *resolved, size_t size)
{
	char tmp[PATH_MAX];

	snprintf(tmp, sizeof(tmp), "%s/bin/box64", get_prefix());
	if (access(tmp, X_OK) == 0) {
		char *rp = realpath(tmp, resolved);
		if (rp)
			return rp;
	}

	return find_in_path("box64", resolved, size);
}

/* ── box64 wrapper script ────────────────────────────────────────── */

/*
 * When a child x86_64 process re-execs box64, box64 looks for itself
 * in BOX64_PATH.  We place a tiny shell wrapper at
 * $PREFIX/glibc/bin/box64 that invokes box64 through the glibc loader.
 */
static void ensure_box64_wrapper(const char *real_box64)
{
	const char *prefix = get_prefix();
	char wrapper[PATH_MAX], shebang[PATH_MAX];
	struct stat st;
	FILE *fp;

	snprintf(wrapper, sizeof(wrapper), "%s/glibc/bin/box64", prefix);
	snprintf(shebang, sizeof(shebang), "#!%s/bin/sh", prefix);

	/* quick check: if wrapper exists and already mentions real box64, skip */
	if (stat(wrapper, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111)) {
		char line[512];
		FILE *f = fopen(wrapper, "r");
		if (f) {
			/* read shebang + exec line */
			if (fgets(line, sizeof(line), f) &&
			    fgets(line, sizeof(line), f) &&
			    strstr(line, real_box64)) {
				fclose(f);
				return; /* already up to date */
			}
			fclose(f);
		}
	}

	unlink(wrapper);
	fp = fopen(wrapper, "w");
	if (!fp)
		return;

	if (fprintf(fp, "%s\nexec %s --library-path %s %s \"$@\"\n",
		    shebang, GLIBC_LOADER, GLIBC_LIB, real_box64) < 0) {
		fclose(fp);
		unlink(wrapper);
		return;
	}

	if (fclose(fp) != 0) {
		unlink(wrapper);
		return;
	}

	chmod(wrapper, 0755);
}

/* ── preload library extraction ──────────────────────────────────── */

static char *extract_preload(char *buf, size_t bufsz)
{
#ifdef EMBED_PRELOAD
	struct stat st;
	int fd;
	ssize_t written;

	if (preload_so_size == 0)
		return NULL;

	snprintf(buf, bufsz, "%s/libovgl_preload.so", GLIBC_LIB);

	/* skip write if size matches (common case) */
	if (stat(buf, &st) == 0 && (size_t)st.st_size == preload_so_size)
		return buf;

	fd = open(buf, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (fd < 0)
		return NULL;

	{
		const unsigned char *p = preload_so_data;
		size_t remaining = preload_so_size;

		while (remaining > 0) {
			written = write(fd, p, remaining);
			if (written < 0) {
				close(fd);
				unlink(buf);
				return NULL;
			}
			p += written;
			remaining -= (size_t)written;
		}
	}

	if (close(fd) != 0) {
		unlink(buf);
		return NULL;
	}

	return buf;
#else
	snprintf(buf, bufsz, "%s/libovgl_preload.so", GLIBC_LIB);
	return access(buf, R_OK) == 0 ? buf : NULL;
#endif
}

/* ── environment construction ────────────────────────────────────── */

/*
 * Helper: duplicate string with NULL check.
 * Returns NULL on OOM (caller must cope).
 */
static inline char *xstrdup(const char *s)
{
	char *d = strdup(s);
	if (!d)
		msg_warn("strdup failed (out of memory)");
	return d;
}

/*
 * Helper: snprintf into a freshly allocated string.
 */
__attribute__((format(printf, 1, 2)))
static char *xasprintf(const char *fmt, ...)
{
	char buf[PATH_MAX + 128];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	return xstrdup(buf);
}

static void free_env(char **env)
{
	if (!env)
		return;
	for (int i = 0; env[i]; i++)
		free(env[i]);
	free(env);
}

/*
 * Build a new environment array for the child process.
 *
 * @preload_path  – path to libovgl_preload.so (may be NULL)
 * @for_box64     – true when launching an x86_64 binary via box64
 * @use_preload   – false when user passed -n
 * @orig_binary   – resolved path of the target binary
 * @debug         – enable OVGL_DEBUG in child
 */
static char **build_environment(const char *preload_path, int for_box64,
				int use_preload, const char *orig_binary,
				int debug)
{
	extern char **environ;
	size_t envc = 0, j = 0;
	char **env;

	while (environ[envc])
		envc++;

	/* room for existing vars + ≤10 new ones + NULL */
	env = calloc(envc + 12, sizeof(char *));
	if (!env)
		return NULL;

	/* copy existing, filtering vars we'll override */
	for (size_t i = 0; i < envc; i++) {
		if (strncmp(environ[i], "LD_PRELOAD=", 11) == 0)        continue;
		if (strncmp(environ[i], "OVGL_GLIBC_LIB=", 16) == 0)   continue;
		if (strncmp(environ[i], "OVGL_GLIBC_LOADER=", 18) == 0) continue;
		if (strncmp(environ[i], "OVGL_ORIG_EXE=", 14) == 0)     continue;
		if (strncmp(environ[i], "BOX64_LD_PRELOAD=", 17) == 0)  continue;
		if (strncmp(environ[i], "BOX64_PATH=", 11) == 0)        continue;

		/* keep user's BOX64_LD_LIBRARY_PATH only in box64 mode */
		if (!for_box64 &&
		    strncmp(environ[i], "BOX64_LD_LIBRARY_PATH=", 22) == 0)
			continue;

		env[j] = xstrdup(environ[i]);
		if (!env[j]) { free_env(env); return NULL; }
		j++;
	}

	/* OVGL env vars for the preload library */
	env[j] = xasprintf("OVGL_GLIBC_LIB=%s", GLIBC_LIB);
	if (!env[j]) { free_env(env); return NULL; } j++;

	env[j] = xasprintf("OVGL_GLIBC_LOADER=%s", GLIBC_LOADER);
	if (!env[j]) { free_env(env); return NULL; } j++;

	if (orig_binary) {
		env[j] = xasprintf("OVGL_ORIG_EXE=%s", orig_binary);
		if (!env[j]) { free_env(env); return NULL; } j++;
	}

	if (for_box64) {
		/* set BOX64_LD_LIBRARY_PATH if user hasn't overridden it */
		if (!getenv("BOX64_LD_LIBRARY_PATH")) {
			env[j] = xasprintf("BOX64_LD_LIBRARY_PATH=%s",
					   GLIBC_LIB_X86);
			if (!env[j]) { free_env(env); return NULL; } j++;
		}

		/* BOX64_PATH for child process re-exec */
		env[j] = xasprintf("BOX64_PATH=%s/glibc/bin/:%s/bin/",
				    get_prefix(), get_prefix());
		if (!env[j]) { free_env(env); return NULL; } j++;

		env[j] = xstrdup("BOX64_UNAME=x86_64");
		if (!env[j]) { free_env(env); return NULL; } j++;

		/*
		 * Do NOT set BOX64_LD_PRELOAD — the preload .so is ARM64
		 * glibc and cannot be loaded into box64's x86_64 context.
		 * Clear LD_PRELOAD too so the native box64 binary itself
		 * isn't affected.
		 */
		env[j] = xstrdup("LD_PRELOAD=");
		if (!env[j]) { free_env(env); return NULL; } j++;
	} else {
		if (preload_path && use_preload) {
			env[j] = xasprintf("LD_PRELOAD=%s", preload_path);
		} else {
			env[j] = xstrdup("LD_PRELOAD=");
		}
		if (!env[j]) { free_env(env); return NULL; } j++;
	}

	if (debug) {
		env[j] = xstrdup("OVGL_DEBUG=1");
		if (!env[j]) { free_env(env); return NULL; } j++;
	}

	env[j] = NULL;
	return env;
}

/* ── wake lock ───────────────────────────────────────────────────── */

/*
 * termux-wake-lock / termux-wake-unlock are fire-and-forget commands.
 * We fork, exec, wait — and that's it.  No PID tracking needed.
 */
static void run_wakelock_cmd(const char *cmd, int debug)
{
	char path[PATH_MAX];
	pid_t pid;
	int status;

	snprintf(path, sizeof(path), "%s/bin/%s", get_prefix(), cmd);
	if (access(path, X_OK) != 0) {
		if (debug)
			msg_warn("%s not found, skipping", cmd);
		return;
	}

	pid = fork();
	if (pid == 0) {
		int fd = open("/dev/null", O_WRONLY);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
		execl(path, cmd, (char *)NULL);
		_exit(127);
	} else if (pid > 0) {
		waitpid(pid, &status, 0);
		if (debug)
			msg_ok("%s done", cmd);
	}
}

static inline void acquire_wake_lock(int debug)
{
	run_wakelock_cmd("termux-wake-lock", debug);
}

static inline void release_wake_lock(int debug)
{
	run_wakelock_cmd("termux-wake-unlock", debug);
}

/* ── signal forwarding ───────────────────────────────────────────── */

static volatile pid_t g_child_pid = -1;

static void forward_signal(int sig)
{
	if (g_child_pid > 0)
		kill(g_child_pid, sig);
}

static void setup_signal_forwarding(pid_t child)
{
	static const int sigs[] = {
		SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2,
	};
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = forward_signal;
	sa.sa_flags   = SA_RESTART;
	sigemptyset(&sa.sa_mask);

	g_child_pid = child;

	for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
		sigaction(sigs[i], &sa, NULL);
}

/* ── child process execution ─────────────────────────────────────── */

/*
 * Reset signal dispositions so the child starts with defaults.
 * Called between fork() and execve() — only uses async-signal-safe
 * functions.
 */
static void child_reset_signals(void)
{
	static const int sigs[] = {
		SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2,
	};

	for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
		signal(sigs[i], SIG_DFL);
}

/*
 * Change CWD to the directory containing @binary_path.
 * Many programs (servers, benchmarks) expect to run from their own
 * directory so they can find adjacent data files.
 */
static void chdir_to_binary(const char *binary_path)
{
	char *copy, *dir;

	copy = strdup(binary_path);
	if (!copy)
		return;

	dir = dirname(copy);
	if (dir && *dir && strcmp(dir, ".") != 0)
		(void)chdir(dir);

	free(copy);
}

/*
 * Unified fork → exec → wait.  Used by both arm64 and x86_64 paths.
 *
 * @exec_path  – binary to execve() (loader or box64)
 * @argv       – full argv array (already constructed by caller)
 * @envp       – full envp array
 * @binary     – user's target binary (for chdir)
 * @debug      – debug flag
 *
 * Returns the process exit code (0–255), or 1 on fork failure.
 */
static int run_child(const char *exec_path, char **argv, char **envp,
		     const char *binary, int debug)
{
	pid_t child;
	int status;

	acquire_wake_lock(debug);

	child = fork();
	if (child == 0) {
		/* child */
		child_reset_signals();
		chdir_to_binary(binary);
		execve(exec_path, argv, envp);
		msg_err("execve %s: %s", exec_path, strerror(errno));
		_exit(127);
	}

	if (child < 0) {
		perror("fork");
		release_wake_lock(debug);
		return 1;
	}

	/* parent */
	setup_signal_forwarding(child);
	waitpid(child, &status, 0);
	release_wake_lock(debug);

	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return 1;
}

/* ── CLI ─────────────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
	fprintf(stderr,
		C_BLUE "ovgl" C_RESET " v" OVGL_VERSION
		" — Run glibc/x86_64 binaries on Termux\n\n"
		C_YELLOW "Usage:" C_RESET " %s [options] <binary> [args...]\n\n"
		C_YELLOW "Options:" C_RESET "\n"
		"  -h, --help        Show this help\n"
		"  -d, --debug       Verbose output\n"
		"  -n, --no-preload  Skip LD_PRELOAD (for simple binaries)\n"
		"  -v, --version     Show version\n"
		"  --                End option parsing\n\n"
		C_YELLOW "Examples:" C_RESET "\n"
		"  %s ./my_glibc_app\n"
		"  %s ./x86_64_app\n"
		"  %s -d geekbench6\n\n"
		C_YELLOW "Paths:" C_RESET "\n"
		"  glibc loader : %s\n"
		"  glibc libs   : %s\n"
		"  x86_64 libs  : %s\n\n",
		prog, prog, prog, prog,
		GLIBC_LOADER, GLIBC_LIB, GLIBC_LIB_X86);
}

static void print_version(void)
{
	printf("ovgl %s\n", OVGL_VERSION);
	printf("loader : %s\n", GLIBC_LOADER);
	printf("x86_64 : %s\n", GLIBC_LIB_X86);
#ifdef EMBED_PRELOAD
	printf("preload: embedded (%u bytes)\n", preload_so_size);
#else
	printf("preload: external\n");
#endif
}

/* ── main ────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
	int debug = 0, use_preload = 1, arg_start = 1;

	/* ── parse options ────────────────────────────────────────── */
	while (arg_start < argc && argv[arg_start][0] == '-') {
		const char *opt = argv[arg_start];

		if (!strcmp(opt, "-h") || !strcmp(opt, "--help"))
			{ print_usage(argv[0]); return 0; }
		if (!strcmp(opt, "-v") || !strcmp(opt, "--version"))
			{ print_version(); return 0; }
		if (!strcmp(opt, "-d") || !strcmp(opt, "--debug"))
			{ debug = 1; arg_start++; continue; }
		if (!strcmp(opt, "-n") || !strcmp(opt, "--no-preload"))
			{ use_preload = 0; arg_start++; continue; }
		if (!strcmp(opt, "--"))
			{ arg_start++; break; }

		msg_err("unknown option: %s", opt);
		return 1;
	}

	if (arg_start >= argc) {
		print_usage(argv[0]);
		return 1;
	}

	/* ── resolve binary ───────────────────────────────────────── */
	const char *binary_name = argv[arg_start];
	char binary_path[PATH_MAX];

	if (!find_in_path(binary_name, binary_path, sizeof(binary_path))) {
		msg_err("binary not found: %s", binary_name);
		return 127;
	}

	if (debug)
		msg_info("resolved: %s", binary_path);

	/* ── analyse ELF ──────────────────────────────────────────── */
	binary_info_t info = analyze_binary(binary_path);

	switch (info.arch) {
	case ARCH_ERROR:   msg_err("cannot read: %s",              binary_path); return 1;
	case ARCH_NOT_ELF: msg_err("not an ELF binary: %s",       binary_path); return 1;
	case ARCH_UNKNOWN: msg_err("unsupported architecture: %s", binary_path); return 1;
	default: break;
	}

	if (debug) {
		const char *arch_s = info.arch == ARCH_AARCH64 ? "arm64" : "x86_64";
		const char *interp_s;
		switch (info.interp) {
		case INTERP_GLIBC:  interp_s = "glibc";   break;
		case INTERP_BIONIC: interp_s = "bionic";   break;
		case INTERP_MUSL:   interp_s = "musl";     break;
		case INTERP_NONE:   interp_s = "none";     break;
		default:            interp_s = "other";    break;
		}
		msg_info("arch=%s interp=%s (%s)", arch_s, interp_s,
			 info.interp_path);
	}

	/* ── musl: unsupported ────────────────────────────────────── */
	if (info.interp == INTERP_MUSL) {
		msg_err("musl binaries are not supported "
			"(ABI-incompatible with glibc)");
		return 1;
	}

	/* ── extract preload library ──────────────────────────────── */
	char preload_buf[PATH_MAX];
	char *preload = extract_preload(preload_buf, sizeof(preload_buf));

	if (debug) {
		if (preload) msg_info("preload: %s", preload);
		else         msg_warn("no preload library available");
	}

	/* ── x86_64 via box64 ─────────────────────────────────────── */
	if (info.arch == ARCH_X86_64) {
		char box64_path[PATH_MAX];

		if (!find_box64(box64_path, sizeof(box64_path))) {
			msg_err("box64 is required for x86_64 binaries "
				"but not found!");
			fprintf(stderr, C_YELLOW "hint:" C_RESET
				" Install box64 or add it to your PATH\n");
			return 127;
		}

		ensure_box64_wrapper(box64_path);

		binary_info_t b64 = analyze_binary(box64_path);
		int b64_glibc = (b64.interp == INTERP_GLIBC);

		if (debug)
			msg_info("box64: %s (glibc=%s)", box64_path,
				 b64_glibc ? "yes" : "no");

		char **env = build_environment(preload, 1, use_preload,
					       binary_path, debug);
		if (!env) { perror("build_environment"); return 1; }

		size_t orig_argc = (size_t)(argc - arg_start);
		size_t extra = b64_glibc ? 8 : 3;
		char **av = calloc(orig_argc + extra, sizeof(char *));
		if (!av) { perror("calloc"); free_env(env); return 1; }

		const char *exec_path;
		int k = 0;

		if (b64_glibc) {
			av[k++] = (char *)GLIBC_LOADER;
			av[k++] = (char *)"--library-path";
			av[k++] = (char *)GLIBC_LIB;
			av[k++] = (char *)"--argv0";
			av[k++] = (char *)"box64";
			av[k++] = box64_path;
			av[k++] = binary_path;
			exec_path = GLIBC_LOADER;
		} else {
			av[k++] = box64_path;
			av[k++] = binary_path;
			exec_path = box64_path;
		}

		for (size_t i = 1; i < orig_argc; i++)
			av[k++] = argv[arg_start + (int)i];
		av[k] = NULL;

		int rc = run_child(exec_path, av, env, binary_path, debug);
		free(av);
		free_env(env);
		return rc;
	}

	/* ── arm64 ────────────────────────────────────────────────── */
	if (info.arch == ARCH_AARCH64) {

		/* native bionic → just exec directly */
		if (info.interp == INTERP_BIONIC) {
			if (debug)
				msg_info("native bionic binary, exec directly");
			execv(binary_path, &argv[arg_start]);
			perror("execv");
			return 1;
		}

		/* need glibc loader */
		if (access(GLIBC_LOADER, X_OK) != 0) {
			msg_err("glibc loader not found: %s", GLIBC_LOADER);
			fprintf(stderr, C_YELLOW "hint:" C_RESET
				" pkg install glibc-repo && "
				"pkg install glibc\n");
			return 1;
		}

		if (access(GLIBC_LIB, F_OK) != 0) {
			msg_err("glibc lib not found: %s", GLIBC_LIB);
			return 1;
		}

		size_t orig_argc = (size_t)(argc - arg_start);
		char **av = calloc(orig_argc + 7, sizeof(char *));
		if (!av) { perror("calloc"); return 1; }

		int k = 0;
		av[k++] = (char *)GLIBC_LOADER;
		av[k++] = (char *)"--library-path";
		av[k++] = (char *)GLIBC_LIB;
		av[k++] = (char *)"--argv0";
		av[k++] = argv[arg_start];
		av[k++] = binary_path;
		for (size_t i = 1; i < orig_argc; i++)
			av[k++] = argv[arg_start + (int)i];
		av[k] = NULL;

		char **env = build_environment(preload, 0, use_preload,
					       binary_path, debug);
		if (!env) { perror("build_environment"); free(av); return 1; }

		if (debug)
			msg_info("exec: %s --library-path %s %s",
				 GLIBC_LOADER, GLIBC_LIB, binary_path);

		int rc = run_child(GLIBC_LOADER, av, env, binary_path, debug);
		free(av);
		free_env(env);
		return rc;
	}

	msg_err("unreachable");
	return 1;
}

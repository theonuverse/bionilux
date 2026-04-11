// SPDX-License-Identifier: MIT
/*
 * bionilux - Run unpatched glibc/x86_64 binaries on Android Termux
 *
 * Native bionic executable that detects binary architecture, invokes
 * the glibc dynamic linker for arm64 glibc binaries, or box64 for
 * x86_64 binaries.  Embeds a preload library for seamless child
 * process support.
 *
 * Build (bionic, on Termux):
 *   clang -O2 -Wall -Wextra -Wpedantic -o bionilux bionilux.c -DEMBED_PRELOAD
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
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

#include "bionilux_elf.h"

/* ── version ─────────────────────────────────────────────────────── */

/*
 * Allow the build system to override the version string at compile
 * time via -DBIONILUX_VERSION_OVERRIDE="\"x.y.z\"".
 */
#ifdef BIONILUX_VERSION_OVERRIDE
#define BIONILUX_VERSION BIONILUX_VERSION_OVERRIDE
#else
#define BIONILUX_VERSION "0.3.0"
#endif

/* ── paths ───────────────────────────────────────────────────────── */

#define GLIBC_PREFIX  "/data/data/com.termux/files/usr/glibc"
#define GLIBC_LIB     GLIBC_PREFIX "/lib"
#define GLIBC_LOADER  GLIBC_LIB "/ld-linux-aarch64.so.1"
#define GLIBC_ETC     GLIBC_PREFIX "/etc"
#define GLIBC_RESOLV_CONF GLIBC_ETC "/resolv.conf"
#define BIONIC_HELPER_LIBPATH "/system/lib64:/data/data/com.termux/files/usr/lib"
#define BIONILUX_INTERNAL_BOX64_REL "bionilux/box64/bin/box64"
#define BIONILUX_INTERNAL_BOX64_DIR_REL "bionilux/box64/bin"

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

/* compile-time prefix match for environment variables */
#define ENVPREFIX(var, lit)	(strncmp((var), (lit), sizeof(lit) - 1) == 0)

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

/* runtime toggle env vars */
#define ENV_CLEANUP_STALE "BIONILUX_CLEANUP_STALE"
#define ENV_WAKELOCK "BIONILUX_WAKELOCK"
#define ENV_CHDIR_TO_BINARY "BIONILUX_CHDIR_TO_BINARY"
#define ENV_BOX64_PATH "BIONILUX_BOX64"

/* ── logging helpers ─────────────────────────────────────────────── */

#define msg_info(...) \
	do { fprintf(stderr, C_BLUE   "bionilux: " C_RESET __VA_ARGS__); \
	     fputc('\n', stderr); } while (0)
#define msg_warn(...) \
	do { fprintf(stderr, C_YELLOW "bionilux: " C_RESET __VA_ARGS__); \
	     fputc('\n', stderr); } while (0)
#define msg_err(...)  \
	do { fprintf(stderr, C_RED    "bionilux: " C_RESET __VA_ARGS__); \
	     fputc('\n', stderr); } while (0)
#define msg_ok(...)   \
	do { fprintf(stderr, C_GREEN  "bionilux: " C_RESET __VA_ARGS__); \
	     fputc('\n', stderr); } while (0)

/* ── embedded preload library ────────────────────────────────────── */

#ifdef EMBED_PRELOAD
#include "preload_data.h"
#else
static const unsigned char preload_so_data[] __attribute__((unused)) = {0};
static const unsigned int  preload_so_size   __attribute__((unused)) = 0;
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
	char          interp_path[PATH_MAX];
} binary_info_t;


/* ── launcher modules ───────────────────────────────────────────── */

#include "src/launcher/elf_path.c"
#include "src/launcher/environment.c"
#include "src/launcher/runtime.c"
#include "src/launcher/main_cli.c"

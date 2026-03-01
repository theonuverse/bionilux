/* SPDX-License-Identifier: MIT */
/*
 * bionilux_elf.h — Shared ELF inspection helpers
 *
 * Used by both bionilux.c (bionic) and bionilux_preload.c (glibc).
 * Only uses POSIX + ELF headers available on both runtimes.
 */
#ifndef BIONILUX_ELF_H
#define BIONILUX_ELF_H

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

/* ── EINTR-safe pread ────────────────────────────────────────────── */

/*
 * Wrapper around pread() that retries on EINTR.  Signals can arrive
 * at any time and a single interrupted pread must not abort ELF
 * analysis.
 */
static inline ssize_t elf_pread(int fd, void *buf, size_t count, off_t offset)
{
	ssize_t n;

	do {
		n = pread(fd, buf, count, offset);
	} while (n < 0 && errno == EINTR);

	return n;
}

/* ── glibc ELF detection ─────────────────────────────────────────── */

/*
 * Check whether @path is a dynamically-linked glibc ELF that needs
 * to be routed through the Termux glibc loader.
 *
 * @glibc_lib  If non-NULL, interpreter paths that already contain
 *             this string are considered "set up" and return 0.
 *
 * Returns:
 *    1  →  glibc binary, redirect through loader
 *    0  →  not glibc / static / already configured / musl
 *   -1  →  I/O error (cannot open or read)
 */
static inline int is_glibc_elf(const char *path, const char *glibc_lib)
{
	Elf64_Ehdr ehdr;
	int fd, ret = 0;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	if (elf_pread(fd, &ehdr, sizeof(ehdr), 0) != (ssize_t)sizeof(ehdr))
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
		off_t off = (off_t)(ehdr.e_phoff +
				    (Elf64_Off)i * ehdr.e_phentsize);

		if (elf_pread(fd, &phdr, sizeof(phdr), off) !=
		    (ssize_t)sizeof(phdr))
			break;

		if (phdr.p_type != PT_INTERP)
			continue;

		if (phdr.p_filesz == 0 || phdr.p_filesz >= PATH_MAX)
			break;

		char interp[PATH_MAX];

		if (elf_pread(fd, interp, phdr.p_filesz,
			      (off_t)phdr.p_offset) !=
		    (ssize_t)phdr.p_filesz)
			break;

		/*
		 * The ELF spec includes the NUL terminator in p_filesz.
		 * Be safe: always NUL-terminate at the end of the data,
		 * never truncate by using p_filesz - 1.
		 */
		interp[phdr.p_filesz] = '\0';

		/* musl != glibc — ABI-incompatible, never redirect */
		if (strstr(interp, "ld-musl"))
			break;

		if (strstr(interp, "ld-linux")) {
			/*
			 * If the interpreter already points into the
			 * Termux glibc prefix, no redirect is needed.
			 */
			if (glibc_lib && strstr(interp, glibc_lib))
				break;
			ret = 1;
		}
		break; /* PT_INTERP found, stop */
	}

out:
	close(fd);
	return ret;
}

#endif /* BIONILUX_ELF_H */

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

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return info;

	/* ── read ELF header ──────────────────────────────────────── */
	if (elf_pread(fd, &ehdr, sizeof(ehdr), 0) != (ssize_t)sizeof(ehdr)) {
		info.arch = ARCH_NOT_ELF;
		goto out;
	}

	if (!elf_validate_header_and_phdr_table(fd, &ehdr)) {
		info.arch = ARCH_NOT_ELF;
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

		if (elf_pread(fd, &phdr, sizeof(phdr), off) !=
		    (ssize_t)sizeof(phdr))
			break;

		if (phdr.p_type != PT_INTERP)
			continue;

		if (phdr.p_filesz == 0 || phdr.p_filesz >= sizeof(info.interp_path))
			break;
		if (!elf_range_in_file(fd, phdr.p_offset, phdr.p_filesz))
			break;

		if (elf_pread(fd, info.interp_path, phdr.p_filesz,
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

static int env_flag_enabled(const char *name, int default_value)
{
	const char *v = getenv(name);

	if (!v || !*v)
		return default_value;

	if (!strcasecmp(v, "1") || !strcasecmp(v, "true") ||
	    !strcasecmp(v, "yes") || !strcasecmp(v, "on"))
		return 1;
	if (!strcasecmp(v, "0") || !strcasecmp(v, "false") ||
	    !strcasecmp(v, "no") || !strcasecmp(v, "off"))
		return 0;

	return default_value;
}

/* ── path resolution ─────────────────────────────────────────────── */

/*
 * Resolve @name to an executable path.
 *   - contains '/' → treat as relative/absolute path directly
 *   - bare name    → search $PATH, then fall back to CWD
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

	/* bare name → search PATH first */
	const char *path_env = getenv("PATH");
	if (path_env) {
		char *dup = strdup(path_env);
		if (dup) {
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
		}
	}

	/* fall back to CWD — convenient for local binaries */
	{
		char cwd[PATH_MAX];
		if (getcwd(cwd, sizeof(cwd))) {
			snprintf(resolved, size, "%s/%s", cwd, name);
			if (access(resolved, X_OK) == 0)
				return resolved;
		}
	}

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
	const char *override = getenv(ENV_BOX64_PATH);

	if (override && *override && access(override, X_OK) == 0) {
		char *rp = realpath(override, resolved);
		if (rp)
			return rp;
		snprintf(resolved, size, "%s", override);
		return resolved;
	}

	snprintf(tmp, sizeof(tmp), "%s/%s",
			 get_prefix(), BIONILUX_INTERNAL_BOX64_REL);
	if (access(tmp, X_OK) == 0) {
		char *rp = realpath(tmp, resolved);
		if (rp)
			return rp;
	}

	snprintf(tmp, sizeof(tmp), "%s/bin/box64", get_prefix());
	if (access(tmp, X_OK) == 0) {
		char *rp = realpath(tmp, resolved);
		if (rp)
			return rp;
	}

	return find_in_path("box64", resolved, size);
}


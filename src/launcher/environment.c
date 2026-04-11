/* ── preload library extraction ──────────────────────────────────── */

static char *extract_preload(char *buf, size_t bufsz)
{
#ifdef EMBED_PRELOAD
	struct stat st;
	int fd;
	ssize_t written;

	if (preload_so_size == 0)
		return NULL;

	snprintf(buf, bufsz, "%s/libbionilux_preload.so", GLIBC_LIB);

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
				if (errno == EINTR)
					continue;
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
	snprintf(buf, bufsz, "%s/libbionilux_preload.so", GLIBC_LIB);
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
 * Helper: format into a freshly allocated string.
 * Uses proper asprintf(3) — no truncation, no fixed-size buffer.
 */
__attribute__((format(printf, 1, 2)))
static char *xasprintf(const char *fmt, ...)
{
	char *p = NULL;
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = vasprintf(&p, fmt, ap);
	va_end(ap);

	if (ret < 0) {
		msg_warn("vasprintf failed (out of memory)");
		return NULL;
	}
	return p;
}

static void free_env(char **env)
{
	if (!env)
		return;
	for (int i = 0; env[i]; i++)
		free(env[i]);
	free(env);
}

static int ensure_resolv_conf(int debug)
{
	int fd;
	static const char fallback[] = "nameserver 8.8.8.8\n";
	const size_t n = sizeof(fallback) - 1;

	if (access(GLIBC_RESOLV_CONF, F_OK) == 0)
		return 0;

	fd = open(GLIBC_RESOLV_CONF,
		  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
		  0644);
	if (fd < 0) {
		if (errno == EEXIST)
			return 0;
		msg_err("cannot create %s: %s",
			GLIBC_RESOLV_CONF, strerror(errno));
		return -1;
	}

	if (write(fd, fallback, n) != (ssize_t)n) {
		msg_err("cannot write %s: %s",
			GLIBC_RESOLV_CONF, strerror(errno));
		close(fd);
		unlink(GLIBC_RESOLV_CONF);
		return -1;
	}

	if (close(fd) != 0) {
		msg_err("cannot close %s: %s",
			GLIBC_RESOLV_CONF, strerror(errno));
		return -1;
	}

	if (debug)
		msg_info("created DNS fallback: %s", GLIBC_RESOLV_CONF);

	return 0;
}

/*
 * Build a new environment array for the child process.
 *
 * @preload_path  – path to libbionilux_preload.so (may be NULL)
 * @for_box64     – true when launching an x86_64 binary via box64
 * @use_preload   – false when user passed -n
 * @orig_binary   – resolved path of the target binary
 * @debug         – enable BIONILUX_DEBUG in child
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
	env = calloc(envc + 16, sizeof(char *));
	if (!env)
		return NULL;

	/* copy existing, filtering vars we'll override */
	for (size_t i = 0; i < envc; i++) {
		if (ENVPREFIX(environ[i], "LD_PRELOAD="))        continue;
		if (ENVPREFIX(environ[i], "BIONILUX_GLIBC_LIB="))   continue;
		if (ENVPREFIX(environ[i], "BIONILUX_GLIBC_LOADER=")) continue;
		if (ENVPREFIX(environ[i], "BIONILUX_ORIG_EXE="))     continue;
		if (ENVPREFIX(environ[i], "BOX64_LD_PRELOAD="))  continue;
		if (ENVPREFIX(environ[i], "BOX64_PATH="))        continue;
		if (for_box64 && ENVPREFIX(environ[i], "LD_LIBRARY_PATH="))
			continue;
		if (for_box64 && ENVPREFIX(environ[i], "BOX64_LD_LIBRARY_PATH="))
			continue;

		/*
		 * Strip glibc-specific LD variables that could
		 * interfere with bionic or the child process.
		 */
		if (ENVPREFIX(environ[i], "LD_AUDIT="))   continue;
		if (ENVPREFIX(environ[i], "LD_DEBUG="))   continue;

		/* keep user's BOX64_LD_LIBRARY_PATH only in box64 mode */
		if (!for_box64 &&
		    ENVPREFIX(environ[i], "BOX64_LD_LIBRARY_PATH="))
			continue;

		env[j] = xstrdup(environ[i]);
		if (!env[j]) { free_env(env); return NULL; }
		j++;
	}

	/* BIONILUX env vars for the preload library */
	env[j] = xasprintf("BIONILUX_GLIBC_LIB=%s", GLIBC_LIB);
	if (!env[j]) { free_env(env); return NULL; } j++;

	env[j] = xasprintf("BIONILUX_GLIBC_LOADER=%s", GLIBC_LOADER);
	if (!env[j]) { free_env(env); return NULL; } j++;

	if (orig_binary) {
		env[j] = xasprintf("BIONILUX_ORIG_EXE=%s", orig_binary);
		if (!env[j]) { free_env(env); return NULL; } j++;
	}

	if (for_box64) {
		const char *user_b64_ld = getenv("BOX64_LD_LIBRARY_PATH");
		const char *user_box64_path = getenv("BOX64_PATH");
		const char *user_box64_uname = getenv("BOX64_UNAME");
		const char *user_host_ld = getenv("LD_LIBRARY_PATH");
		const char *user_path = getenv("PATH");
		char binary_dir[PATH_MAX];
		const char *orig_dir = NULL;

		/* Never propagate ARM64 preload into box64/x86_64 context. */
		env[j] = xstrdup("LD_PRELOAD=");
		if (!env[j]) { free_env(env); return NULL; } j++;

		/* Keep host helpers (sh/lscpu/...) on bionic libs first. */
		if (user_host_ld && *user_host_ld) {
			env[j] = xasprintf("LD_LIBRARY_PATH=%s:%s",
					   BIONIC_HELPER_LIBPATH, user_host_ld);
		} else {
			env[j] = xstrdup("LD_LIBRARY_PATH=" BIONIC_HELPER_LIBPATH);
		}
		if (!env[j]) { free_env(env); return NULL; } j++;

		if (user_b64_ld && *user_b64_ld)
			env[j] = xasprintf("BOX64_LD_LIBRARY_PATH=%s", user_b64_ld);
		else
			env[j] = xasprintf("BOX64_LD_LIBRARY_PATH=%s", GLIBC_LIB_X86);
		if (!env[j]) { free_env(env); return NULL; } j++;

		env[j] = xasprintf("RESOLV_CONF=%s", GLIBC_RESOLV_CONF);
		if (!env[j]) { free_env(env); return NULL; } j++;

		if (orig_binary && *orig_binary) {
			char tmp[PATH_MAX];
			char *dir;

			snprintf(tmp, sizeof(tmp), "%s", orig_binary);
			dir = dirname(tmp);
			if (dir && *dir) {
				snprintf(binary_dir, sizeof(binary_dir), "%s", dir);
				orig_dir = binary_dir;
			}
		}

		if (user_box64_path && *user_box64_path) {
			env[j] = xasprintf("BOX64_PATH=%s", user_box64_path);
		} else if (orig_dir && user_path && *user_path) {
			env[j] = xasprintf("BOX64_PATH=%s:%s/%s:%s",
					   orig_dir,
					   get_prefix(),
					   BIONILUX_INTERNAL_BOX64_DIR_REL,
					   user_path);
		} else if (orig_dir) {
			env[j] = xasprintf("BOX64_PATH=%s:%s/%s:%s/bin",
					   orig_dir,
					   get_prefix(),
					   BIONILUX_INTERNAL_BOX64_DIR_REL,
					   get_prefix());
		} else if (user_path && *user_path) {
			env[j] = xasprintf("BOX64_PATH=%s/%s:%s",
					   get_prefix(),
					   BIONILUX_INTERNAL_BOX64_DIR_REL,
					   user_path);
		} else {
			env[j] = xasprintf("BOX64_PATH=%s/%s:%s/bin",
					   get_prefix(),
					   BIONILUX_INTERNAL_BOX64_DIR_REL,
					   get_prefix());
		}
		if (!env[j]) { free_env(env); return NULL; } j++;

		/* Compatibility default: some x86_64 binaries expect uname -m=x86_64. */
		if (!user_box64_uname || !*user_box64_uname) {
			env[j] = xstrdup("BOX64_UNAME=x86_64");
			if (!env[j]) { free_env(env); return NULL; } j++;
		}

		/*
		 * Do NOT set BOX64_LD_PRELOAD — the preload .so is ARM64
		 * glibc and cannot be loaded into box64's x86_64 context.
		 * Omit LD_PRELOAD entirely so neither box64 nor the
		 * emulated process inherits a stale value.
		 */
	} else {
		if (preload_path && use_preload) {
			env[j] = xasprintf("LD_PRELOAD=%s", preload_path);
			if (!env[j]) { free_env(env); return NULL; } j++;
		}
		/* No preload → don't set LD_PRELOAD at all */
	}

	if (debug) {
		env[j] = xstrdup("BIONILUX_DEBUG=1");
		if (!env[j]) { free_env(env); return NULL; } j++;
	}

	env[j] = NULL;
	return env;
}


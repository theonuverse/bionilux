/* ── open/openat path redirect for NSS/FHS ──────────────────────── */

/*
 * Resolve GLIBC prefix with minimal overhead.
 * Priority: $GLIBC_PREFIX -> derive from $BIONILUX_GLIBC_LIB -> fallback.
 */
static const char *get_glibc_prefix(void)
{
	const char *p = getenv("GLIBC_PREFIX");
	const char *lib;
	static char derived[PATH_MAX];
	const char *s;
	size_t n;

	if (p && *p)
		return p;

	lib = getenv(GLIBC_LIB_ENV);
	if (!lib || !*lib)
		return DEFAULT_GLIBC_PREFIX;

	/* /.../glibc/lib or /.../glibc/lib/... -> /.../glibc */
	s = strstr(lib, "/lib");
	if (!s)
		return DEFAULT_GLIBC_PREFIX;

	n = (size_t)(s - lib);
	if (n == 0 || n >= sizeof(derived))
		return DEFAULT_GLIBC_PREFIX;

	memcpy(derived, lib, n);
	derived[n] = '\0';
	return derived;
}

/*
 * Rewrite selected absolute paths to Termux glibc layout.
 * Returns @buf when rewritten, otherwise returns original @path.
 */
static const char *redirect_path_if_needed(const char *path,
					   char *buf, size_t bufsz)
{
	const char *prefix;
	int n;

	if (bufsz == 0)
		return path;

	if (!path || path[0] != '/')
		return path;

	/* Do not rewrite suspicious paths with dot-segments. */
	if (strstr(path, "/../") || strstr(path, "/./") ||
	    !strcmp(path, "/..") || !strcmp(path, "/."))
		return path;

	prefix = get_glibc_prefix();
	if (!prefix || prefix[0] != '/')
		return path;

	/* NSS + resolver files from /etc -> $GLIBC_PREFIX/etc */
	if (!strcmp(path, "/etc/resolv.conf") ||
	    !strcmp(path, "/etc/hosts") ||
	    !strcmp(path, "/etc/nsswitch.conf") ||
	    !strncmp(path, "/etc/", 5)) {
		n = snprintf(buf, bufsz, "%s/etc/%s", prefix, path + 5);
		if (n <= 0 || (size_t)n >= bufsz)
			return path;
		debug_print("redirect: %s -> %s", path, buf);
		return buf;
	}

	/* Multiarch library paths -> $GLIBC_PREFIX/lib/... */
	if (!strncmp(path, "/lib/x86_64-linux-gnu/", 20)) {
		n = snprintf(buf, bufsz, "%s/lib/x86_64-linux-gnu/%s",
			     prefix, path + 20);
		if (n <= 0 || (size_t)n >= bufsz)
			return path;
		debug_print("redirect: %s -> %s", path, buf);
		return buf;
	}

	if (!strncmp(path, "/usr/lib/", 9)) {
		n = snprintf(buf, bufsz, "%s/lib/%s", prefix, path + 9);
		if (n <= 0 || (size_t)n >= bufsz)
			return path;
		debug_print("redirect: %s -> %s", path, buf);
		return buf;
	}

	return path;
}

static inline int open_needs_mode(int flags)
{
	return (flags & O_CREAT) != 0;
}

static int fallback_openat_call(int dirfd, const char *path,
				int flags, mode_t mode)
{
	return (int)syscall(SYS_openat, dirfd, path, flags, mode);
}

static int call_real_open(const char *path, int flags, mode_t mode)
{
	if (real_open) {
		if (open_needs_mode(flags))
			return real_open(path, flags, mode);
		return real_open(path, flags);
	}
	return fallback_openat_call(AT_FDCWD, path, flags, mode);
}

static int call_real_open64(const char *path, int flags, mode_t mode)
{
	if (real_open64) {
		if (open_needs_mode(flags))
			return real_open64(path, flags, mode);
		return real_open64(path, flags);
	}
	return call_real_open(path, flags, mode);
}

static int call_real_openat(int dirfd, const char *path,
			    int flags, mode_t mode)
{
	if (real_openat) {
		if (open_needs_mode(flags))
			return real_openat(dirfd, path, flags, mode);
		return real_openat(dirfd, path, flags);
	}
	return fallback_openat_call(dirfd, path, flags, mode);
}

static int call_real_openat64(int dirfd, const char *path,
			      int flags, mode_t mode)
{
	if (real_openat64) {
		if (open_needs_mode(flags))
			return real_openat64(dirfd, path, flags, mode);
		return real_openat64(dirfd, path, flags);
	}
	return call_real_openat(dirfd, path, flags, mode);
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
static char **build_clean_envp(char *const envp[], int strip_all_ld_preload)
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

		/* optionally strip all LD_PRELOAD for native helper tools */
		if (ENVPREFIX(envp[i], "LD_PRELOAD=")) {
			if (strip_all_ld_preload) {
				debug_print("stripping LD_PRELOAD for native helper");
				continue;
			}
			if (strstr(envp[i], "libbionilux_preload")) {
				debug_print("stripping glibc LD_PRELOAD for "
					    "bionic child");
				continue;
			}
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

static int has_path_suffix(const char *path, const char *suffix)
{
	size_t lp, ls;

	if (!path || !suffix)
		return 0;

	lp = strlen(path);
	ls = strlen(suffix);
	if (lp < ls)
		return 0;

	return memcmp(path + (lp - ls), suffix, ls) == 0;
}

/*
 * Native helper binaries should run outside the glibc/preload context.
 * We match both canonical /usr/bin paths and Termux absolute paths.
 */
static int is_native_helper_exec(const char *resolved)
{
	return has_path_suffix(resolved, "/usr/bin/sh") ||
	       has_path_suffix(resolved, "/bin/sh") ||
	       has_path_suffix(resolved, "/system/bin/sh") ||
	       has_path_suffix(resolved, "/usr/bin/lscpu") ||
	       has_path_suffix(resolved, "/bin/lscpu");
}


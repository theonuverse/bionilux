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
		int native_helper = is_native_helper_exec(resolved);

		debug_print("not glibc (result=%d), cleaning env", glibc_bin);
		if (native_helper)
			debug_print("native helper detected: %s", resolved);

		char **clean = build_clean_envp(envp, native_helper);

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

int execveat(int dirfd, const char *pathname, char *const argv[],
	    char *const envp[], int flags)
{
	char fd_path[64];

	if ((flags & AT_EMPTY_PATH) && pathname[0] == '\0') {
		snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", dirfd);
		return execve(fd_path, argv, envp);
	}

	if (pathname[0] == '/')
		return execve(pathname, argv, envp);

	if (dirfd == AT_FDCWD)
		return execve(pathname, argv, envp);

	return safe_execveat(dirfd, pathname, argv, envp, flags);
}

int fexecve(int fd, char *const argv[], char *const envp[])
{
	char fd_path[64];
	int ret;

	snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", fd);
	ret = execve(fd_path, argv, envp);
	if (ret == 0)
		return ret;

	if (real_fexecve)
		return real_fexecve(fd, argv, envp);

	return safe_execveat(fd, "", argv, envp, AT_EMPTY_PATH);
}


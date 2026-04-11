/* ── hooked open/open64/openat/openat64 ─────────────────────────── */

int open(const char *pathname, int flags, ...)
{
	mode_t mode = 0;
	char redirected[PATH_MAX];
	const char *target = redirect_path_if_needed(pathname, redirected,
					     sizeof(redirected));

	if (open_needs_mode(flags)) {
		va_list ap;

		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}

	return call_real_open(target, flags, mode);
}

int open64(const char *pathname, int flags, ...)
{
	mode_t mode = 0;
	char redirected[PATH_MAX];
	const char *target = redirect_path_if_needed(pathname, redirected,
					     sizeof(redirected));

	if (open_needs_mode(flags)) {
		va_list ap;

		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}

	return call_real_open64(target, flags, mode);
}

int openat(int dirfd, const char *pathname, int flags, ...)
{
	mode_t mode = 0;
	char redirected[PATH_MAX];
	const char *target = redirect_path_if_needed(pathname, redirected,
					     sizeof(redirected));

	if (open_needs_mode(flags)) {
		va_list ap;

		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}

	return call_real_openat(dirfd, target, flags, mode);
}

int openat64(int dirfd, const char *pathname, int flags, ...)
{
	mode_t mode = 0;
	char redirected[PATH_MAX];
	const char *target = redirect_path_if_needed(pathname, redirected,
					     sizeof(redirected));

	if (open_needs_mode(flags)) {
		va_list ap;

		va_start(ap, flags);
		mode = va_arg(ap, mode_t);
		va_end(ap);
	}

	return call_real_openat64(dirfd, target, flags, mode);
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


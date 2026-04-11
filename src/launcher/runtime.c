/* ── wake lock ───────────────────────────────────────────────────── */

/*
 * termux-wake-lock / termux-wake-unlock are optional and disabled by
 * default. When enabled we fork, exec and wait for completion.
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
		while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
		if (debug)
			msg_ok("%s done", cmd);
	}
}

static inline void acquire_wake_lock(int debug)
{
	if (!env_flag_enabled(ENV_WAKELOCK, 0))
		return;
	run_wakelock_cmd("termux-wake-lock", debug);
}

static inline void release_wake_lock(int debug)
{
	if (!env_flag_enabled(ENV_WAKELOCK, 0))
		return;
	run_wakelock_cmd("termux-wake-unlock", debug);
}

/* ── stale process cleanup ───────────────────────────────────────── */

/*
 * Optional stale cleanup: terminate running instances with the exact same
 * executable path. Disabled by default and enabled via ENV_CLEANUP_STALE.
 * This avoids shell expansion hazards and fixed startup delays.
 */
static void cleanup_stale_processes(const char *binary_path, int debug)
{
	DIR *proc;
	struct dirent *ent;
	char target[PATH_MAX];
	const char *target_path;
	pid_t mypid = getpid();

	if (realpath(binary_path, target))
		target_path = target;
	else
		target_path = binary_path;

	if (debug)
		msg_info("cleanup-stale enabled for: %s", target_path);

	proc = opendir("/proc");
	if (!proc)
		return;

	while ((ent = readdir(proc)) != NULL) {
		char *end = NULL;
		long pid_l;
		pid_t pid;
		char link_path[64];
		char exe_path[PATH_MAX];
		ssize_t n;

		if (!isdigit((unsigned char)ent->d_name[0]))
			continue;

		pid_l = strtol(ent->d_name, &end, 10);
		if (!end || *end != '\0' || pid_l <= 1)
			continue;

		pid = (pid_t)pid_l;
		if (pid == mypid)
			continue;

		snprintf(link_path, sizeof(link_path), "/proc/%ld/exe", pid_l);
		n = readlink(link_path, exe_path, sizeof(exe_path) - 1);
		if (n <= 0)
			continue;
		exe_path[n] = '\0';

		if (strcmp(exe_path, target_path) != 0)
			continue;

		if (kill(pid, SIGTERM) == 0 && debug)
			msg_ok("sent SIGTERM to stale pid %ld", pid_l);
	}

	closedir(proc);
}

static void maybe_cleanup_stale_processes(const char *binary_path, int debug)
{
	if (!env_flag_enabled(ENV_CLEANUP_STALE, 0))
		return;
	cleanup_stale_processes(binary_path, debug);
}

/* ── signal forwarding ───────────────────────────────────────────── */

/*
 * Signals to forward to the child process.
 * Includes SIGWINCH for correct terminal-resize handling in TUI apps.
 */
static const int forwarded_sigs[] = {
	SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2, SIGWINCH,
};

/*
 * Use sig_atomic_t — pid_t writes are not guaranteed atomic on all
 * architectures.  Valid PIDs fit comfortably in sig_atomic_t.
 */
static volatile sig_atomic_t g_child_pid;

static void forward_signal(int sig)
{
	pid_t pid = (pid_t)g_child_pid;

	if (pid > 0)
		kill(pid, sig);
}

/*
 * Install signal forwarding handlers.
 * Must be called BEFORE fork() to avoid a race where a signal arrives
 * between fork() and handler installation.
 * g_child_pid is set to 0 initially — the handler checks pid > 0, so
 * signals arriving before we record the real PID are harmlessly ignored.
 */
static void install_signal_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = forward_signal;
	sa.sa_flags   = SA_RESTART;
	sigemptyset(&sa.sa_mask);

	g_child_pid = 0;

	for (size_t i = 0; i < ARRAY_SIZE(forwarded_sigs); i++)
		sigaction(forwarded_sigs[i], &sa, NULL);
}

/* ── child process execution ─────────────────────────────────────── */

/*
 * Reset signal dispositions so the child starts with defaults.
 * Called between fork() and execve() — only uses async-signal-safe
 * functions.
 */
static void child_reset_signals(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(forwarded_sigs); i++)
		signal(forwarded_sigs[i], SIG_DFL);
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
	if (dir && *dir && strcmp(dir, ".") != 0) {
		if (chdir(dir) < 0) { /* best-effort, non-fatal */ }
	}

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
	int wait_rc;

	acquire_wake_lock(debug);

	/*
	 * Install signal handlers BEFORE fork() to close the race
	 * window where a signal could arrive after fork() but before
	 * handler installation.
	 */
	install_signal_handlers();

	child = fork();
	if (child == 0) {
		/* child */
		child_reset_signals();
		if (env_flag_enabled(ENV_CHDIR_TO_BINARY, 0))
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

	/* parent — record PID so the handler can forward signals */
	g_child_pid = (sig_atomic_t)child;
	do {
		wait_rc = waitpid(child, &status, 0);
	} while (wait_rc < 0 && errno == EINTR);

	if (wait_rc < 0) {
		msg_err("waitpid failed: %s", strerror(errno));
		release_wake_lock(debug);
		return 1;
	}

	release_wake_lock(debug);

	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return 1;
}

static int parse_shebang(const char *path, char *interp, size_t interp_sz,
				 char *interp_arg, size_t interp_arg_sz)
{
	char buf[PATH_MAX + 64];
	char *p, *q;
	int fd;
	ssize_t n;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 2)
		return 0;

	buf[n] = '\0';
	if (buf[0] != '#' || buf[1] != '!')
		return 0;

	p = buf + 2;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '\0' || *p == '\n' || *p == '\r')
		return -1;

	q = p;
	while (*q && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r')
		q++;

	if ((size_t)(q - p) >= interp_sz)
		return -1;
	memcpy(interp, p, (size_t)(q - p));
	interp[q - p] = '\0';

	interp_arg[0] = '\0';
	while (*q == ' ' || *q == '\t')
		q++;
	if (*q && *q != '\n' && *q != '\r') {
		char *r = q;
		size_t len;

		while (*r && *r != '\n' && *r != '\r')
			r++;
		while (r > q && (r[-1] == ' ' || r[-1] == '\t'))
			r--;

		len = (size_t)(r - q);
		if (len >= interp_arg_sz)
			len = interp_arg_sz - 1;
		memcpy(interp_arg, q, len);
		interp_arg[len] = '\0';
	}

	return 1;
}

static int maybe_exec_script(const char *script_path, int argc,
			     char *argv[], int arg_start, int debug)
{
	char interp[PATH_MAX], interp_arg[PATH_MAX];
	char resolved_interp[PATH_MAX];
	int rc;
	size_t orig_argc, extra, k = 0;
	char **av;

	rc = parse_shebang(script_path, interp, sizeof(interp),
			   interp_arg, sizeof(interp_arg));
	if (rc == 0)
		return -1;
	if (rc < 0) {
		msg_err("invalid shebang: %s", script_path);
		return 1;
	}

	if (!find_in_path(interp, resolved_interp, sizeof(resolved_interp))) {
		msg_err("interpreter not found: %s", interp);
		return 127;
	}

	if (debug)
		msg_info("script: %s via %s", script_path, resolved_interp);

	orig_argc = (size_t)(argc - arg_start);
	extra = interp_arg[0] ? 3 : 2;
	av = calloc(orig_argc + extra, sizeof(char *));
	if (!av) {
		perror("calloc");
		return 1;
	}

	av[k++] = resolved_interp;
	if (interp_arg[0])
		av[k++] = interp_arg;
	av[k++] = (char *)script_path;
	for (size_t i = 1; i < orig_argc; i++)
		av[k++] = argv[arg_start + (int)i];
	av[k] = NULL;

	execv(resolved_interp, av);
	msg_err("exec script interpreter failed: %s", strerror(errno));
	free(av);
	return errno == ENOENT ? 127 : 1;
}


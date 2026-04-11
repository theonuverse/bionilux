/* ── CLI ─────────────────────────────────────────────────────────── */

static void print_usage(const char *prog)
{
	fprintf(stderr,
		C_BLUE "bionilux" C_RESET " v" BIONILUX_VERSION
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
	printf("bionilux %s\n", BIONILUX_VERSION);
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
	case ARCH_NOT_ELF: {
		int script_rc = maybe_exec_script(binary_path, argc, argv,
						 arg_start, debug);
		if (script_rc >= 0)
			return script_rc;
		msg_err("not an ELF binary: %s", binary_path);
		return 1;
	}
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
		if (!use_preload)
			msg_info("preload: disabled (-n flag)");
		else if (preload)
			msg_info("preload: %s", preload);
		else
			msg_warn("no preload library available");
	}

	/* ── x86_64 via box64 ─────────────────────────────────────── */
	if (info.arch == ARCH_X86_64) {
		char box64_path[PATH_MAX];

		if (ensure_resolv_conf(debug) < 0)
			return 1;

		if (!find_box64(box64_path, sizeof(box64_path))) {
			msg_err("box64 is required for x86_64 binaries "
				"but not found!");
			fprintf(stderr, C_YELLOW "hint:" C_RESET
				" Install box64 or add it to your PATH\n");
			return 127;
		}

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

		maybe_cleanup_stale_processes(binary_path, debug);
		int rc = run_child(exec_path, av, env, binary_path, debug);
		free(av);
		free_env(env);
		return rc;
	}

	/* ── arm64 ────────────────────────────────────────────────── */
	if (info.arch == ARCH_AARCH64) {

		/* native bionic or static ELF → exec directly */
		if (info.interp == INTERP_BIONIC || info.interp == INTERP_NONE) {
			if (debug)
				msg_info("direct execution path");
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

		maybe_cleanup_stale_processes(binary_path, debug);
		int rc = run_child(GLIBC_LOADER, av, env, binary_path, debug);
		free(av);
		free_env(env);
		return rc;
	}

	msg_err("unreachable");
	return 1;
}

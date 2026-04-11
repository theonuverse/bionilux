/* ── constructor ─────────────────────────────────────────────────── */

__attribute__((constructor))
static void init(void)
{
	/*
	 * Use *(void **)& to assign dlsym results to function pointers
	 * without triggering -Wpedantic warnings about void* → fptr
	 * conversion (which is technically undefined in ISO C but
	 * required by POSIX dlsym).
	 */
	*(void **)&real_execve = dlsym(RTLD_NEXT, "execve");
	if (!real_execve)
		constructor_warn("execve", dlerror());

	*(void **)&real_execveat = dlsym(RTLD_NEXT, "execveat");
	if (!real_execveat)
		constructor_warn("execveat", dlerror());

	*(void **)&real_fexecve = dlsym(RTLD_NEXT, "fexecve");
	if (!real_fexecve)
		constructor_warn("fexecve", dlerror());

	*(void **)&real_readlink = dlsym(RTLD_NEXT, "readlink");
	if (!real_readlink)
		constructor_warn("readlink", dlerror());

	*(void **)&real_readlinkat = dlsym(RTLD_NEXT, "readlinkat");
	if (!real_readlinkat)
		constructor_warn("readlinkat", dlerror());

	*(void **)&real_open = dlsym(RTLD_NEXT, "open");
	if (!real_open)
		constructor_warn("open", dlerror());

	*(void **)&real_open64 = dlsym(RTLD_NEXT, "open64");
	if (!real_open64)
		constructor_warn("open64", dlerror());

	*(void **)&real_openat = dlsym(RTLD_NEXT, "openat");
	if (!real_openat)
		constructor_warn("openat", dlerror());

	*(void **)&real_openat64 = dlsym(RTLD_NEXT, "openat64");
	if (!real_openat64)
		constructor_warn("openat64", dlerror());

	debug_enabled = (getenv(BIONILUX_DEBUG_ENV) != NULL);

	debug_print("bionilux_preload loaded (pid=%d)", (int)getpid());
}

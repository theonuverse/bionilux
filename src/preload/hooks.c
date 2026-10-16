// bionilux LD_PRELOAD hooks (glibc side).
//
// Three responsibilities:
//   1. Reroute exec*/posix_spawn* through the launcher so glibc children
//      stay on the loader path.
//   2. Make /proc/self/exe report the real target, not ld.so.
//   3. Redirect /etc/* to $GLIBC/etc/* when the file exists, and provide
//      a getifaddrs fallback for Android where netlink is blocked.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/ioctl.h>

extern char **environ;

// ── debug ──────────────────────────────────────────────────────────
static int bionilux_debug(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *v = getenv("BIONILUX_DEBUG");
        cached = (v && *v) ? 1 : 0;
    }
    return cached;
}

#define DBG(...) do { \
    if (bionilux_debug()) fprintf(stderr, "[BIONILUX HOOK] " __VA_ARGS__); \
} while (0)

// Resolve next definition of `name` into `var`, cached.
#define REAL(var, name) \
    do { if (!(var)) (var) = (__typeof__(var))dlsym(RTLD_NEXT, name); } while (0)

// ── rerouting ──────────────────────────────────────────────────────
static int is_launcher(const char *path, const char *launcher) {
    return launcher && path && strcmp(path, launcher) == 0;
}

static char **strip_ld_preload(char *const envp[]) {
    size_t envc = 0;
    while (envp && envp[envc]) envc++;
    char **ev = malloc((envc + 1) * sizeof *ev);
    if (!ev) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < envc; i++)
        if (strncmp(envp[i], "LD_PRELOAD=", 11) != 0) ev[j++] = envp[i];
    ev[j] = NULL;
    return ev;
}

// Envp for the child: strips LD_PRELOAD and stale BIONILUX_ARGV0,
// appends BIONILUX_ARGV0=<argv0> so the launcher can restore the
// caller's argv[0]. Caller frees the array; the BIONILUX_ARGV0 string
// points into a thread-local buffer.
static char **build_child_envp(char *const envp[], const char *argv0) {
    size_t envc = 0;
    while (envp && envp[envc]) envc++;

    char **ev = malloc((envc + 2) * sizeof *ev);
    if (!ev) return NULL;

    size_t j = 0;
    for (size_t i = 0; i < envc; i++) {
        if (strncmp(envp[i], "LD_PRELOAD=", 11) == 0) continue;
        if (strncmp(envp[i], "BIONILUX_ARGV0=", 15) == 0) continue;
        ev[j++] = envp[i];
    }

    if (argv0) {
        static __thread char argv0_buf[PATH_MAX + 16];
        int n = snprintf(argv0_buf, sizeof argv0_buf, "BIONILUX_ARGV0=%s", argv0);
        if (n > 0 && (size_t)n < sizeof argv0_buf)
            ev[j++] = argv0_buf;
    }
    ev[j] = NULL;
    return ev;
}

// Look up a bare name in PATH. Returns 0 if found, -1 otherwise.
static int resolve_bare_name(const char *name, char *buf, size_t size) {
    const char *path = getenv("PATH");
    if (!path || !*path) return -1;

    char *dup = strdup(path);
    if (!dup) return -1;

    int found = -1;
    char *save = NULL;
    for (char *dir = strtok_r(dup, ":", &save);
         dir;
         dir = strtok_r(NULL, ":", &save)) {
        int n = snprintf(buf, size, "%s/%s", dir, name);
        if (n > 0 && (size_t)n < size && access(buf, X_OK) == 0) {
            found = 0;
            break;
        }
    }
    free(dup);
    return found;
}

// Build [launcher, target, argv[1..]] with a stripped envp. Returns 1
// on reroute, 0 to pass through so the caller sees ENOENT synchronously.
static int reroute(const char *target, char *const argv[], char *const envp[],
                   const char **launcher, char ***nargv, char ***nenvp) {
    *launcher = getenv("BIONILUX_BINARY_PATH");
    if (!*launcher || !target || is_launcher(target, *launcher)) return 0;

    if (strchr(target, '/')) {
        if (access(target, X_OK) != 0) return 0;
    } else {
        char resolved[PATH_MAX];
        if (resolve_bare_name(target, resolved, sizeof resolved) != 0) return 0;
    }

    size_t argc = 0;
    while (argv && argv[argc]) argc++;
    size_t n = argc ? argc : 1;

    char **av = malloc((n + 2) * sizeof *av);
    char **ev = build_child_envp(envp, argv && argv[0] ? argv[0] : target);
    if (!av || !ev) { free(av); free(ev); return 0; }

    av[0] = (char *)*launcher;
    av[1] = (char *)target;
    for (size_t i = 1; i < argc; i++) av[i + 1] = argv[i];
    av[n + 1] = NULL;

    *nargv = av;
    *nenvp = ev;
    DBG("Routing through bionilux: %s\n", target);
    return 1;
}

static int real_execve(const char *p, char *const a[], char *const e[]) {
    typedef int (*fn_t)(const char *, char *const[], char *const[]);
    static fn_t fn;
    REAL(fn, "execve");
    return fn(p, a, e);
}

static int run_rerouted(const char *launcher, char **av, char **ev) {
    int r = real_execve(launcher, av, ev);
    int e = errno;
    free(av); free(ev);
    errno = e;
    return r;
}

static int exec_launcher(const char *path, char *const argv[], char *const envp[]) {
    char **clean = strip_ld_preload(envp);
    int r = real_execve(path, argv, clean ? clean : envp);
    int e = errno;
    free(clean);
    errno = e;
    return r;
}

// ── exec* family ───────────────────────────────────────────────────
int execve(const char *path, char *const argv[], char *const envp[]) {
    if (is_launcher(path, getenv("BIONILUX_BINARY_PATH")))
        return exec_launcher(path, argv, envp);
    const char *l; char **av, **ev;
    if (reroute(path, argv, envp, &l, &av, &ev))
        return run_rerouted(l, av, ev);
    return real_execve(path, argv, envp);
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[]) {
    typedef int (*fn_t)(const char *, char *const[]);
    static fn_t real;
    REAL(real, "execvp");
    if (is_launcher(file, getenv("BIONILUX_BINARY_PATH")))
        return exec_launcher(file, argv, environ);
    const char *l; char **av, **ev;
    if (reroute(file, argv, environ, &l, &av, &ev))
        return run_rerouted(l, av, ev);
    return real(file, argv);
}

int execvpe(const char *file, char *const argv[], char *const envp[]) {
    if (is_launcher(file, getenv("BIONILUX_BINARY_PATH"))) {
        char **clean = strip_ld_preload(envp);
        int r = real_execve(file, argv, clean ? clean : envp);
        int e = errno; free(clean); errno = e;
        return r;
    }
    const char *l; char **av, **ev;
    if (reroute(file, argv, envp, &l, &av, &ev))
        return run_rerouted(l, av, ev);

    typedef int (*fn_t)(const char *, char *const[], char *const[]);
    static fn_t real;
    REAL(real, "execvpe");
    if (real) return real(file, argv, envp);
    errno = ENOSYS;
    return -1;
}

// va_list -> argv. Pointer so the caller's position advances on aarch64
// (va_list is a struct, not an array). Caller frees.
static char **va_to_argv(const char *arg0, va_list *app) {
    size_t n = 1;
    va_list copy;
    va_copy(copy, *app);
    while (va_arg(copy, const char *)) n++;
    va_end(copy);

    char **argv = malloc((n + 1) * sizeof *argv);
    if (!argv) return NULL;

    argv[0] = (char *)arg0;
    for (size_t i = 1; i < n; i++) argv[i] = va_arg(*app, char *);
    argv[n] = NULL;
    return argv;
}

int execl(const char *path, const char *arg0, ...) {
    va_list ap; va_start(ap, arg0);
    char **argv = va_to_argv(arg0, &ap);
    va_end(ap);
    if (!argv) { errno = ENOMEM; return -1; }
    int r = execve(path, argv, environ);
    int e = errno; free(argv); errno = e;
    return r;
}

int execlp(const char *file, const char *arg0, ...) {
    va_list ap; va_start(ap, arg0);
    char **argv = va_to_argv(arg0, &ap);
    va_end(ap);
    if (!argv) { errno = ENOMEM; return -1; }
    int r = execvp(file, argv);
    int e = errno; free(argv); errno = e;
    return r;
}

int execle(const char *path, const char *arg0, ...) {
    va_list ap; va_start(ap, arg0);
    char **argv = va_to_argv(arg0, &ap);
    (void)va_arg(ap, char *);  // consume trailing NULL
    char *const *envp = va_arg(ap, char *const *);
    va_end(ap);
    if (!argv) { errno = ENOMEM; return -1; }
    int r = execve(path, argv, envp);
    int e = errno; free(argv); errno = e;
    return r;
}

// /proc/self/fd/N -> real path. -1 for memfd/(deleted) or on error.
static ssize_t resolve_fd_path(int fd, char *buf, size_t size) {
    char link[64];
    int n = snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    if (n < 0 || (size_t)n >= sizeof link) return -1;
    ssize_t r = readlink(link, buf, size);
    if (r <= 0 || (size_t)r >= size) return -1;
    buf[r] = '\0';
    if (strstr(buf, " (deleted)")) return -1;
    if (strncmp(buf, "memfd:", 6) == 0) return -1;
    return r;
}

int fexecve(int fd, char *const argv[], char *const envp[]) {
    typedef int (*fn_t)(int, char *const[], char *const[]);
    static fn_t real;
    REAL(real, "fexecve");
    if (getenv("BIONILUX_BINARY_PATH")) {
        char path[PATH_MAX];
        if (resolve_fd_path(fd, path, sizeof path) > 0)
            return execve(path, argv, envp);
    }
    if (real) return real(fd, argv, envp);
    errno = ENOSYS; return -1;
}

int execveat(int dirfd, const char *path, char *const argv[],
             char *const envp[], int flags) {
    typedef int (*fn_t)(int, const char *, char *const[], char *const[], int);
    static fn_t real;
    REAL(real, "execveat");
    if (getenv("BIONILUX_BINARY_PATH")) {
        if ((flags & AT_EMPTY_PATH) && path[0] == '\0') {
            char fd_path[PATH_MAX];
            if (resolve_fd_path(dirfd, fd_path, sizeof fd_path) > 0)
                return execve(fd_path, argv, envp);
        } else if (path[0] == '/' || dirfd == AT_FDCWD) {
            return execve(path, argv, envp);
        }
    }
    if (real) return real(dirfd, path, argv, envp, flags);
    errno = ENOSYS; return -1;
}

// ── posix_spawn* ───────────────────────────────────────────────────
typedef int (*spawn_fn)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                        const posix_spawnattr_t *, char *const[], char *const[]);

static spawn_fn spawn_plain(void) {
    static spawn_fn fn;
    REAL(fn, "posix_spawn");
    return fn;
}

static int spawn_impl(int search_path, pid_t *pid, const char *path,
                      const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *attr,
                      char *const argv[], char *const envp[]) {
    spawn_fn plain = spawn_plain();
    if (!plain) { errno = ENOSYS; return -1; }

    if (is_launcher(path, getenv("BIONILUX_BINARY_PATH"))) {
        char **clean = strip_ld_preload(envp);
        int r = plain(pid, path, fa, attr, argv, clean ? clean : envp);
        int e = errno; free(clean); errno = e;
        return r;
    }

    const char *l; char **av, **ev;
    if (reroute(path, argv, envp, &l, &av, &ev)) {
        int r = plain(pid, l, fa, attr, av, ev);
        int e = errno; free(av); free(ev); errno = e;
        return r;
    }

    if (search_path) {
        typedef int (*fn_t)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                            const posix_spawnattr_t *, char *const[], char *const[]);
        static fn_t real;
        REAL(real, "posix_spawnp");
        if (real) return real(pid, path, fa, attr, argv, envp);
        errno = ENOSYS; return -1;
    }
    return plain(pid, path, fa, attr, argv, envp);
}

int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
    return spawn_impl(0, pid, path, fa, attr, argv, envp);
}

int posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa,
                 const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
    return spawn_impl(1, pid, file, fa, attr, argv, envp);
}

// ── /etc/* -> $GLIBC/etc/* and open* ───────────────────────────────
static const char *glibc_prefix(void) {
    static const char *cached;
    if (!cached) {
        cached = getenv("BIONILUX_GLIBC_PREFIX");
        if (!cached || !cached[0])
            cached = "/data/data/com.termux/files/usr/glibc";
    }
    return cached;
}

static const char *redirect_etc(const char *path, char *buf, size_t size) {
    if (path[0] != '/' || strncmp(path, "/etc/", 5) != 0) return path;
    int n = snprintf(buf, size, "%s%s", glibc_prefix(), path);
    if (n <= 0 || (size_t)n >= size) return path;
    if (access(buf, F_OK) != 0) return path;
    return buf;
}

// O_TMPFILE does not include O_CREAT but still takes a mode argument.
static int open_uses_mode(int flags) {
#ifdef O_TMPFILE
    if ((flags & O_TMPFILE) == O_TMPFILE) return 1;
#endif
    return (flags & O_CREAT) != 0;
}

typedef int (*open_fn)(const char *, int, ...);
typedef int (*openat_fn)(int, const char *, int, ...);

int open(const char *path, int flags, ...) {
    static open_fn real; REAL(real, "open");
    char buf[PATH_MAX];
    const char *t = redirect_etc(path, buf, sizeof buf);
    if (open_uses_mode(flags)) {
        va_list ap; va_start(ap, flags);
        mode_t mode = va_arg(ap, mode_t); va_end(ap);
        return real(t, flags, mode);
    }
    return real(t, flags);
}

int open64(const char *path, int flags, ...) {
    static open_fn real; REAL(real, "open64");
    char buf[PATH_MAX];
    const char *t = redirect_etc(path, buf, sizeof buf);
    if (open_uses_mode(flags)) {
        va_list ap; va_start(ap, flags);
        mode_t mode = va_arg(ap, mode_t); va_end(ap);
        return real(t, flags, mode);
    }
    return real(t, flags);
}

int openat(int dirfd, const char *path, int flags, ...) {
    static openat_fn real; REAL(real, "openat");
    char buf[PATH_MAX];
    const char *t = (path[0] == '/') ? redirect_etc(path, buf, sizeof buf) : path;
    if (open_uses_mode(flags)) {
        va_list ap; va_start(ap, flags);
        mode_t mode = va_arg(ap, mode_t); va_end(ap);
        return real(dirfd, t, flags, mode);
    }
    return real(dirfd, t, flags);
}

int openat64(int dirfd, const char *path, int flags, ...) {
    static openat_fn real; REAL(real, "openat64");
    char buf[PATH_MAX];
    const char *t = (path[0] == '/') ? redirect_etc(path, buf, sizeof buf) : path;
    if (open_uses_mode(flags)) {
        va_list ap; va_start(ap, flags);
        mode_t mode = va_arg(ap, mode_t); va_end(ap);
        return real(dirfd, t, flags, mode);
    }
    return real(dirfd, t, flags);
}

// ── /proc/self/exe ─────────────────────────────────────────────────
static ssize_t fake_exe(char *buf, size_t size) {
    const char *exe = getenv("BIONILUX_TARGET_EXE");
    if (!exe) return -1;
    size_t len = strlen(exe);
    size_t n = len < size ? len : size;
    memcpy(buf, exe, n);
    return (ssize_t)n;
}

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t size) {
    typedef ssize_t (*fn_t)(int, const char *, char *, size_t);
    static fn_t real; REAL(real, "readlinkat");
    if (strcmp(path, "/proc/self/exe") == 0) {
        ssize_t r = fake_exe(buf, size);
        if (r >= 0) return r;
    }
    return real(dirfd, path, buf, size);
}

ssize_t readlink(const char *path, char *buf, size_t size) {
    return readlinkat(AT_FDCWD, path, buf, size);
}

// ── getifaddrs fallback via SIOCGIFCONF ────────────────────────────
// Android's SELinux blocks the netlink RTM_GETLINK query glibc's
// getifaddrs uses, so it returns EACCES. WebRTC/ICE needs the local
// interface list to build candidates; without it, connections to
// BDS hang at "Connecting to multiplayer game". ioctl(SIOCGIFCONF)
// is still permitted and gives us the IPv4 interfaces.
struct bn_ifa {
    struct ifaddrs ifa;
    char name[IFNAMSIZ];
    struct sockaddr_in addr, mask;
};

static int fallback_getifaddrs(struct ifaddrs **ifap) {
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return -1;

    struct ifreq reqs[32];
    struct ifconf ic;
    memset(&ic, 0, sizeof ic);
    ic.ifc_len = sizeof reqs;
    ic.ifc_req = reqs;
    if (ioctl(s, SIOCGIFCONF, &ic) < 0) {
        int e = errno; close(s); errno = e; return -1;
    }

    int n = ic.ifc_len / (int)sizeof(struct ifreq);
    struct bn_ifa *arr = calloc(n ? n : 1, sizeof *arr);
    if (!arr) { close(s); errno = ENOMEM; return -1; }

    int k = 0;
    for (int i = 0; i < n; i++) {
        if (reqs[i].ifr_addr.sa_family != AF_INET) continue;
        struct bn_ifa *e = &arr[k];

        memcpy(e->name, reqs[i].ifr_name, IFNAMSIZ - 1);
        memcpy(&e->addr, &reqs[i].ifr_addr, sizeof e->addr);

        struct ifreq q;
        memset(&q, 0, sizeof q);
        memcpy(q.ifr_name, e->name, IFNAMSIZ - 1);
        unsigned flags = 0;
        if (ioctl(s, SIOCGIFFLAGS, &q) == 0)
            flags = (unsigned short)q.ifr_flags;
        if (!(flags & IFF_UP)) continue;

        memset(&q, 0, sizeof q);
        memcpy(q.ifr_name, e->name, IFNAMSIZ - 1);
        if (ioctl(s, SIOCGIFNETMASK, &q) == 0)
            memcpy(&e->mask, &q.ifr_netmask, sizeof e->mask);

        e->ifa.ifa_name    = e->name;
        e->ifa.ifa_flags   = flags;
        e->ifa.ifa_addr    = (struct sockaddr *)&e->addr;
        e->ifa.ifa_netmask = (struct sockaddr *)&e->mask;
        if (k > 0) arr[k - 1].ifa.ifa_next = &e->ifa;
        k++;
    }
    close(s);

    if (k == 0) { free(arr); *ifap = NULL; return 0; }
    *ifap = &arr[0].ifa;
    return 0;
}

int getifaddrs(struct ifaddrs **ifap) {
    typedef int (*fn_t)(struct ifaddrs **);
    static fn_t real;
    REAL(real, "getifaddrs");

    int r = real ? real(ifap) : -1;
    int e = errno;

    if (r != 0) {
        DBG("getifaddrs real failed (errno=%d), using SIOCGIFCONF fallback\n", e);
        r = fallback_getifaddrs(ifap);
        e = errno;
    }

    if (bionilux_debug() && r == 0) {
        for (struct ifaddrs *i = *ifap; i; i = i->ifa_next) {
            char ip[INET6_ADDRSTRLEN] = "-";
            if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET)
                inet_ntop(AF_INET, &((struct sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof ip);
            DBG("  if=%s fam=%d addr=%s\n", i->ifa_name,
                i->ifa_addr ? i->ifa_addr->sa_family : -1, ip);
        }
    } else if (r != 0) {
        DBG("getifaddrs fallback failed (errno=%d)\n", e);
    }
    errno = e;
    return r;
}

__attribute__((constructor)) static void on_init(void) {
    DBG("Injection successful!\n");
}
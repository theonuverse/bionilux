# bionilux — Design

Why bionilux is built the way it is, and what it deliberately does not do.

## The problem

Termux runs on Android's Bionic libc. Released Linux binaries are almost
always linked against glibc and expect a dynamic linker that does not
exist on Android. The kernel refuses them with ENOENT.

The obvious fix — invoke the glibc loader by hand — works for the first
process and then breaks: when that process spawns a child, the child's
execve hits the same missing-interpreter problem, and the chain stops.

## The shape of the solution

Two components plus one external dependency:

- A launcher, written in Rust, compiled for Bionic. It reads the target's
  ELF header, decides which loader or emulator to use, and execve's into
  it.
- A preload library, written in C, compiled for glibc. It hooks the exec
  family and a few related functions so children of the target keep
  flowing through the launcher.
- box64-glibc, used only when the target is x86_64. It is itself an
  aarch64-glibc binary and runs through the same loader path.

## Why preload instead of ptrace

PRoot solves the same problem with ptrace. Every syscall in the guest is
intercepted, its arguments rewritten, and its result written back. This
works for everything — including programs that use raw syscalls or that
need full filesystem virtualization — but it costs about 2 microseconds
per syscall. On a workload that makes 100,000 file syscalls (a compiler
invocation, pip install, git status in a large repository), that is
seconds of extra wall time.

bionilux does not intercept syscalls. It hooks only the exec family via
LD_PRELOAD and leaves the rest of the syscall path untouched. The cost is
that bionilux cannot virtualize the filesystem, cannot fake identity, and
cannot intercept raw syscalls. Programs that need those features must use
PRoot or proot-distro.

The trade-off is deliberate. bionilux is the fast path for programs that
already work with a shared /etc and the host's normal filesystem.

## Why the launcher is bionic and the hook is glibc

The launcher runs as a normal Termux process, so it must be compiled for
Bionic. The preload library is injected into glibc processes; it must be
compiled against glibc, because the glibc loader refuses a .so that needs
Bionic symbols.

The two worlds do not mix. The bionic launcher strips LD_PRELOAD from its
own environment before running, so it never accidentally loads a glibc
.so. Conversely, the glibc preload removes the bionic libtermux-exec
entry from any LD_PRELOAD before handing it to a glibc child.

## Why argv[0] is passed through BIONILUX_ARGV0

The kernel sets argv[0] to whatever the caller passed. The glibc loader's
--argv0 flag lets a caller override that, but the launcher does not know
what the original argv[0] was once the hook has rewritten the command
line.

So the hook writes the caller's original argv[0] into a child environment
variable, and the launcher reads it back and passes it to the loader via
--argv0. That is how exec -a name, login shells (-bash), and multicall
binaries (BusyBox style) see their real name.

## Why hooks.c is a single file

It is about 600 lines, well-commented, and structured into clear
sections. Every hook shares the same bionilux_debug, REAL, and reroute
helpers. Splitting it now would duplicate those helpers, obscure the data
flow, and add no value.

The file will be split when it crosses roughly 800 lines or when the
hooks start needing genuinely different state. The natural split would be
hooks_exec.c, hooks_fs.c, and hooks_net.c, each with its own copy of the
small REAL macro.

## Why no VFS virtualization

bionilux redirects a small number of paths to make glibc programs work:

- /proc/self/exe (fake the target path)
- /etc/* when a file exists under $GLIBC/etc/ (NSS, resolver)

It does not simulate a Linux root filesystem. Programs that hardcode
paths like /var/run, /usr/share, or /root will not find them.

This is a documented limitation, not a bug. Full virtualization is what
PRoot and proot-distro are for. bionilux covers the case where the host's
own /etc and /usr are good enough, which is most compiled binaries and
CLI tools.

## Why no --rootfs, no fake root, no virtual identity

An earlier design consideration was to add a rootfs mode: map a Debian
directory as the guest's /, fake uid=0, redirect apt and dpkg. A related
project (dsh-bionilux by kelai141) does exactly this and is a legitimate
second product line.

That mode turns a launcher into a userspace compatibility layer. It grows
the preload library from a few hundred lines to a few thousand, adds
stat*, open*, realpath, dlopen, getpwnam, getgrnam, DNS overrides, and
virtual identity. Every new hook is a new chance for a program to hit an
unimplemented case.

bionilux deliberately does not do this. It is the small, fast path. If
you need a Debian userland with apt, that is a different project.

## Why hook version negotiation

The launcher and the preload library communicate through internal
environment variables (BIONILUX_TARGET_EXE, BIONILUX_BINARY_PATH,
BIONILUX_GLIBC_PREFIX, BIONILUX_ARGV0). These are an internal protocol
between two components that are released together.

If a future release changes the protocol, the launcher and preload must
be upgraded in lockstep. The current design does not version this
protocol. That is fine while the two components ship together in the same
release tarball. If a fork ever wants to mix them across versions, add
BIONILUX_PROTOCOL=1 to the env and check it in both.

## Supply chain

The CI pipeline builds two things that are not covered by cargo test or
the smoke tests:

- box64-glibc, cloned from ptitSeb/box64 at whatever HEAD is when CI
  runs. This is a known reproducibility gap.
- x86_64-libs.tar.xz, extracted from Ubuntu's daily resolute rootfs base
  image. This is also not reproducible across days.

Both are documented as risks. Pinning them requires either maintaining a
mirror of box64 source or a SHA-256 of the tarball and would slow the
release cadence for little gain at the current project size.

## What the smoke tests cover

- execle with multiple args and custom envp (the aarch64 va_list bug)
- fexecve on a real file
- Launcher self-call through execve, execvp, posix_spawn
- Shebang with env -S
- Shebang recursion limit
- Exit code propagation
- argv[0] preservation, both direct and via exec -a
- execvp on a missing name returns ENOENT to the caller
- system()
- getifaddrs (the SIOCGIFCONF fallback)
- Missing path passes through to the shell as ENOENT

The suite is glibc-side end-to-end. It exercises the full chain:
launcher, loader, preload, target, child, launcher.

## What bionilux does not test

- Aarch64 cross-compilation in CI produces a working binary. Only the
  native Termux build is smoke-tested.
- x86_64 emulation is tested only when a suitable x86_64 binary is
  available on the test host.
- The getifaddrs fallback is tested in isolation, not in the context of a
  WebRTC/ICE handshake.
# bionilux

Run glibc and x86_64 Linux binaries on Termux — natively, no PRoot.

Termux uses Android's Bionic libc. Almost all released Linux binaries are
linked against glibc and expect a dynamic linker
(`/lib/ld-linux-aarch64.so.1`) that does not exist on Android. The kernel
refuses them with `ENOENT`. bionilux invokes the glibc loader manually and
keeps children on the same path with an `LD_PRELOAD` hook.

## Install

Prebuilt binaries from the latest GitHub release. Launcher, preload,
box64-glibc, and x86_64 runtime libs all come from our own releases.

```bash
curl -fsSL theonuverse.github.io/bionilux/setup | bash
```

<<<<<<< HEAD
Pin a version with `BIONILUX_VERSION=v0.4.2`. Uninstall:
=======
Pin a version with `BIONILUX_VERSION=v0.5.0`. Uninstall:
>>>>>>> 0994076 (v0.5.0: reroute fixes, argv[0] preservation, RuntimeConfig)

```bash
rm -f "$PREFIX/bin/bionilux" "$PREFIX/glibc/lib/libbionilux_preload.so"
rm -rf "$PREFIX/bionilux"
```

## Build from source

Requires Termux with `glibc-repo`, `glibc`, `clang`, `rust` and `git`.

```bash
git clone https://github.com/theonuverse/bionilux
cd bionilux

# Preload (glibc) — builds libbionilux_preload.so in the project root
scripts/build-preload.sh

# Launcher (bionic)
cargo build --release
```

`scripts/build-preload.sh --install` also copies the preload into
`$GLIBC/lib/libbionilux_preload.so` in one step. Override the glibc
location with `GLIBC=/path scripts/build-preload.sh`.

Install what you just built:

```bash
install -m755 target/release/bionilux "$PREFIX/bin/bionilux"
install -m755 libbionilux_preload.so "$GLIBC/lib/"
```

## x86_64 support (optional)

The launcher itself handles aarch64 glibc binaries. To also run x86_64
binaries, you need two extra pieces: `box64-glibc` (the aarch64-glibc
build of box64) and the x86_64 runtime libraries it loads.

Fetch both from the latest release:

```bash
VERSION=$(curl -fsSL https://api.github.com/repos/theonuverse/bionilux/releases/latest \
          | grep -oP '"tag_name":\s*"\K[^"]+')
BASE="https://github.com/theonuverse/bionilux/releases/download/$VERSION"

# box64-glibc
mkdir -p "$PREFIX/bionilux"
curl -fSL -o "$PREFIX/bionilux/box64-glibc" "$BASE/box64-glibc"
chmod 755 "$PREFIX/bionilux/box64-glibc"

# x86_64 runtime libraries
cd /tmp
curl -fSL -o x86_64-libs.tar.xz "$BASE/x86_64-libs.tar.xz"
rm -rf "$PREFIX/glibc/lib/x86_64-linux-gnu"
tar -xf x86_64-libs.tar.xz -C "$PREFIX/glibc/lib"
rm x86_64-libs.tar.xz
```

After that, `bionilux ./some_x86_64_binary` works transparently. The
installer (`setup`) does all of the above automatically.

## Usage

```bash
bionilux <binary> [args...]
```

Examples:

```bash
bionilux ./my_binary                     # glibc or x86_64, auto-detected
bionilux $PREFIX/glibc/bin/bash          # glibc shell
BIONILUX_DEBUG=1 bionilux ./my_app       # verbose
BIONILUX_CHDIR_TO_BINARY=1 bionilux ./server
bionilux --print-config                  # show resolved runtime paths
```

To enter a glibc shell where every child you spawn is rerouted through
bionilux automatically:

```bash
bionilux $PREFIX/glibc/bin/bash
```

## Environment variables

| Variable | Purpose |
|----------|---------|
| BIONILUX_DEBUG | Print debug lines to stderr |
| BIONILUX_PRELOAD | Override preload .so path |
| BIONILUX_CHDIR_TO_BINARY | chdir to target's directory before exec |
| BIONILUX_PREFIX | Override the install prefix (default: `$PREFIX` or `/data/data/com.termux/files/usr`) |
| BIONILUX_GLIBC_DIR | Override the glibc directory (default: `$prefix/glibc`) |
| BIONILUX_LOADER | Override the dynamic loader path (default: `$glibc_dir/lib/ld-linux-aarch64.so.1`) |
| BIONILUX_BOX64 | Override the box64-glibc binary path (default: `$prefix/bionilux/box64-glibc`) |
| BIONILUX_CA_FILE | Override the CA bundle path (default: `$prefix/etc/tls/cert.pem`) |
| BIONILUX_CA_DIR | Override the CA directory path (default: `$prefix/etc/tls/certs`) |

`bionilux --print-config` prints the fully resolved paths and exits.

## The problem

On Linux, running a program looks like this:

```
kernel reads ELF header
    ↓
sees PT_INTERP = /lib/ld-linux-aarch64.so.1
    ↓
loads ld.so, ld.so loads libc.so.6 + program
    ↓
program starts
```

On Termux the kernel reads the header, tries to load the interpreter, does
not find it, and returns `ENOENT`:

```
bash: ./myapp: cannot execute: required file not found
```

Three layers to the problem:

1. Missing interpreter. The glibc loader is not at the path the binary
   expects.
2. Missing libraries. The binary needs glibc's `libc.so.6`, `libm.so.6`,
   `libpthread.so.0`, not Bionic's.
3. Missing kernel ABI. This part actually works — Bionic and glibc use the
   same Linux syscalls on Android. The problem is entirely in userspace.

## How it can be solved

Three approaches on Termux:

PRoot / proot-distro. A ptrace-based sandbox. Intercepts every syscall and
simulates a full root filesystem. Transparent and general, but 2x to 10x
slower because every syscall goes through ptrace. Crippling for CPU-bound
work.

Recompile against Bionic. Fastest possible, but not general. You cannot
recompile a proprietary benchmark or a released vendor binary.

Ship glibc as a second libc and invoke its loader manually. Termux already
offers glibc through `glibc-repo`. Invoking `ld-linux-aarch64.so.1`
manually solves the first process but not its children — the child's
`execve` hits the same missing-interpreter problem.

bionilux uses the third approach, plus one extra ingredient: an
`LD_PRELOAD` hook that reroutes child exec calls through the launcher
again. Native speed, no ptrace, no sandbox. The trade-off is that it does
not virtualize the filesystem — programs that hardcode Linux paths like
`/etc/resolv.conf` need targeted redirection, which we handle for the
important cases (DNS and NSS).

## How bionilux works

Two components plus one external dependency.

The launcher, a small Rust binary compiled for Bionic. It parses the
target's ELF header, decides which interpreter or emulator to use,
then calls `execve()` on it.

The preload library, a shared library compiled for glibc. Injected via
`LD_PRELOAD` into every glibc process bionilux starts. It hooks `exec*`,
`posix_spawn*`, `open*`, `readlink*`, and `getifaddrs` so that children
of the target process are themselves rerouted, common Linux paths resolve,
and WebRTC can find local interfaces.

Box64 (glibc build), used only when the target is x86_64. Box64 is itself
an aarch64-glibc binary; bionilux starts it through the same loader path,
and Box64 does the rest.

Termux's native binaries keep running natively. Termux never sees a glibc
preload — the bionic launcher strips it before calling itself.

## Architecture

```
        bionilux <target>
              │
       read ELF header
              │
   ┌──────────┼───────────┬─────────────┐
   ▼          ▼           ▼             ▼
bionic     aarch64     x86_64        shebang
/static    glibc       glibc         script
   │          │           │             │
   │          ▼           ▼             │
   │      ld-linux   ld-linux +         │
   │      +preload   box64 +preload     │
   │          │           │             │
   ▼          ▼           ▼             ▼
execve    target      x86_64 bin    interpreter
native    runs        runs under    resolved and
                      box64         rerun
```

The launcher is a decision tree with four outputs:

| Detected | Action |
|----------|--------|
| aarch64 bionic or static ELF | `execve(target)` — natively |
| aarch64 glibc ELF | loader + preload |
| x86_64 glibc ELF | loader + box64 + preload |
| shebang script | parse interpreter, resolve, recurse |

## Detailed operation

### aarch64 glibc binary

```
bionilux ./my_glibc_app arg1 arg2
```

1. main.rs resolves the target to an absolute path via which().
2. elf::analyze() reads the header:
   machine = 0xB7 (aarch64), libc = Glibc, interpreter = /lib/ld-linux-aarch64.so.1.
3. resolve() looks up the loader: /lib/ld-linux-aarch64.so.1 does not
   exist, falls back to $GLIBC/lib/ld-linux-aarch64.so.1. Finds the
   preload library. Builds argv with the absolute target path first.
4. env::build_target_env() sets LD_PRELOAD to our .so,
   BIONILUX_TARGET_EXE to the target, BIONILUX_BINARY_PATH to the
   launcher, and BIONILUX_GLIBC_PREFIX to the glibc directory.
5. exec::execute() calls:
   execve($GLIBC/lib/ld-linux-aarch64.so.1,
          [ld.so, --argv0, argv0, target, arg1, arg2],
          env)
6. Kernel loads ld.so. ld.so sees LD_PRELOAD, loads our hook, then loads
   libc.so.6, libm.so.6 and friends from $GLIBC/lib/.
7. Our constructor prints "Injection successful".
8. ld.so jumps to the target's _start. The program runs.

If the program spawns a child with execvp("ls", ...):

9. The hook's reroute() sees target = "ls", resolves it via PATH, and
   builds [bionilux, "ls"], stripping LD_PRELOAD from env and setting
   BIONILUX_ARGV0 to the caller's argv[0].
10. execve(bionilux, ...) is called.
11. bionilux runs again, resolves "ls" to /data/.../usr/bin/ls, sees
    libc = Bionic, and calls execve(ls) directly.
12. ls runs.

The chain stops because the child is Bionic.

### x86_64 glibc binary

```
bionilux ./x86_64_server
```

Same first four steps, except machine = 0x3E (x86_64). Then:

5. resolve() sees x86_64. Checks that $PREFIX/bionilux/box64-glibc
   exists. Builds argv with box64's full path as argv[0]. Box64 uses
   argv[0] to find its own directory — a bare "box64" crashes it with
   strlen(NULL) inside libc.
6. execve(ld.so, [ld.so, --argv0, box64-path, box64-path, server, args...], env)
7. ld.so loads box64-glibc. Our preload is also injected, prints
   "Injection successful".
8. Box64 initializes Dynarec, sets up x86_64 memory, loads server. It
   prints "Warning, cannot pre-load libbionilux_preload.so" — expected:
   the preload is aarch64 and cannot enter an x86_64 process. Box64
   ignores it and continues.
9. server runs under emulation.

The preload's purpose here is only for the intermediate aarch64 stage
(box64's own process), which may call lscpu or uname on the host.

### Shebang script

`/tmp/greet.sh`:

```
#!/usr/bin/env -S python3 -u
print("hello")
```

1. elf::analyze() returns None (not ELF).
2. resolve_at() falls into script().
3. parse_shebang() reads the first line:
   interp = /usr/bin/env, arg = "-S python3 -u".
4. rewrite_termux("/usr/bin/env") -> $PREFIX/bin/env.
5. Basename is "env", so we parse the argument list: strip -S, first word
   is python3, remaining is ["-u"].
6. which("python3") -> interpreter path. Recurse: resolve python3 itself.
7. Resulting argv is [python3, "-u", script, args...].
8. execve(loader, [loader, --argv0, python3, python3, "-u", script, ...], env).

This handles the common #!/usr/bin/env PROGRAM shebang and the -S
variant.

## Implementation

### Launcher (Rust, bionic)

| File | Role |
|------|------|
| src/main.rs | CLI entry, calls resolver, calls exec |
| src/debug.rs | BIONILUX_DEBUG-gated debug!() macro |
| src/analyzer/elf.rs | ELF header + PT_INTERP via goblin |
| src/launcher/config.rs | RuntimeConfig: resolve all paths once |
| src/launcher/resolve.rs | Decision tree + shebang + loader/preload lookup |
| src/launcher/env.rs | Build env for target |
| src/launcher/exec.rs | execve with --argv0 |

### Detection

elf.rs reads the ELF header via mmap and returns:

machine: 0x3E (x86_64), 0xB7 (aarch64), or anything else (rejected).
libc: Bionic, Glibc, Musl, Static, Other.
interpreter: the PT_INTERP string, if present.

Classification:

| PT_INTERP | Class |
|-----------|-------|
| absent | Static |
| /system/bin/linker64 | Bionic |
| contains ld-linux | Glibc |
| contains ld-musl | Musl |
| anything else | Other |

Musl and Other are rejected with a clear error — ABI-incompatible with
glibc, cannot be routed through the same loader.

### Preload hooks

When a glibc process starts under bionilux, LD_PRELOAD injects our .so.
The constructor resolves symbols via dlsym(RTLD_NEXT, ...) and hooks:

exec family:

| Hook | Behavior |
|------|----------|
| execve | Reroute glibc targets through launcher; strip LD_PRELOAD if target is the launcher itself |
| execv | execve(path, argv, environ) |
| execvp | Bare-name PATH lookup before reroute; miss passes through |
| execvpe | Same as execvp with custom envp |
| execl, execle, execlp | Convert va_list to argv[], delegate |
| fexecve | Resolve /proc/self/fd/N to real path, then execve |
| execveat | Same for AT_EMPTY_PATH; absolute/AT_FDCWD paths go through execve |
| posix_spawn, posix_spawnp | Same reroute logic |

The hook passes the caller's argv[0] to the launcher via BIONILUX_ARGV0.
The launcher restores it with --argv0 so login shells (-bash), exec -a,
and multicall binaries see their real name.

Note on aarch64: va_list is a struct, not an array. Our va_to_argv takes
a pointer so the caller's position advances correctly.

open family:

| Hook | Behavior |
|------|----------|
| open, open64, openat, openat64 | Redirect /etc/* to $GLIBC/etc/* if the target exists |

readlink family:

| Hook | Behavior |
|------|----------|
| readlink | If path is /proc/self/exe, return BIONILUX_TARGET_EXE |
| readlinkat | Same for the at-variant |

getifaddrs:

| Hook | Behavior |
|------|----------|
| getifaddrs | Android blocks netlink RTM_GETLINK for apps; fall back to ioctl(SIOCGIFCONF) so WebRTC/ICE can build candidates |

Without the getifaddrs fallback, connections to a NetherNet Bedrock
server hang at "Connecting to multiplayer game".

## Testing

### Unit tests (`cargo test`)

22 tests:

- rewrite_termux — POSIX prefix rewriting (3)
- parse_shebang — well-formed, missing arg, trailing whitespace, empty
  file, no #!, non-UTF-8 after line 1 (7)
- RuntimeConfig::with_prefix derives all paths (1)
- find_loader returns None for missing (1)
- filter_preload — empty, blocklist, keep normal (3)
- build_from — preload setup, prepend, strip libtermux-exec, no-preload,
  BOX64_UNAME set/clear, stale var removal (7)

### End-to-end smoke tests (`tests/smoke.sh`)

14 tests that run the real binary on the device:

| Test | Proves |
|------|--------|
| execle (multi-arg + envp) | aarch64 va_list handling |
| fexecve | /proc/self/fd/N resolution |
| launcher self-call: execve | LD_PRELOAD stripped when target == launcher |
| launcher self-call: execvp | Same, via execvp |
| launcher self-call: posix_spawn | Same, via posix_spawn |
| shebang env -S | #!/usr/bin/env -S prog arg parses |
| shebang recursion limit | Self-referencing scripts error cleanly |
| exit code propagated | exit(42) reaches the caller as 42 |
| argv[0] preserved (direct) | bionilux passes the caller's argv[0] |
| exec -a preserves custom argv[0] | Shell `exec -a name` works |
| execvp missing name -> ENOENT | Bare-name miss returns ENOENT synchronously |
| system() | glibc system() runs |
| getifaddrs fallback | SIOCGIFCONF fallback returns IPv4 interfaces |
| missing path -> shell ENOENT | Hook passes through |

Run from the project root in a native Termux shell:

```bash
cargo test
tests/smoke.sh
```

Real-world verification: Geekbench 7 (aarch64 glibc preview) runs a full
single-core and multi-core suite without crashes. Minecraft Bedrock Server
(x86_64) starts, generates a world, and accepts LAN connections through
the NetherNet transport.

## How we know it works

Every layer of the routing has a test:

| Layer | Verified by |
|-------|-------------|
| ELF classification | cargo test (via parse_shebang and rewrite_termux inputs) |
| Loader lookup | find_loader unit tests |
| Preload injection | smoke test: execle, fexecve |
| Child rerouting | smoke test: launcher self-call (3 variants), box64 chain |
| /proc/self/exe fix | smoke test: bedrock_server (uses it) |
| Shebang handling | smoke test: env -S, recursion limit |
| Exit codes | smoke test: exit code 42 |
| argv[0] preservation | smoke test: direct and exec -a |
| Bare-name miss semantics | smoke test: execvp missing name |
| getifaddrs | smoke test: ifaddrs-ok |

Additionally: cargo clippy -- -D warnings passes with zero lints. The
preload is cross-compiled with clang against the actual Termux glibc
sysroot; the launcher is cross-compiled for aarch64-linux-android. Both
are exercised together in the smoke tests, which run the real binaries
end-to-end.

## Known limitations

These are documented design choices, not bugs.

execveat(dirfd, relative_path): bypasses the hook, runs natively. Rare
pattern.

Native interpreters: a shebang script whose interpreter is a bionic
binary (Termux sh, bash, python) runs without the preload. If that
interpreter spawns a glibc child, the child is not rerouted.

User LD_PRELOAD: dropped for rerouted children. Unavoidable — a bionic
launcher cannot load a glibc preload.

musl: rejected at detection. ABI-incompatible with glibc.

No VFS virtualization. bionilux redirects /etc/* accesses to $GLIBC/etc/*
when the target exists, but does not simulate a full Linux root filesystem.
Programs that hardcode /var/run/ or /usr/share/ will not find them.

For full filesystem virtualization, use PRoot or proot-distro. bionilux is
the fast path for programs that already work with a shared /etc.

## Disclaimer

The idea, the architecture, the choice of approach, and the direction of
every design decision are mine. Code and text were written with AI
assistance; the AI also helped find and fix bugs across the review cycles.
Every change was validated by building and running the project on a real
device.

## License

MIT
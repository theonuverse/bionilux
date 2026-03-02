# bionilux

**bionilux** (Bionic Linux Glibc Loader) runs **unpatched** glibc ARM64 and x86\_64 binaries on Termux — no proot, no container.

---

## Overview

Termux uses Android's **Bionic libc**, but most Linux binaries are built against **glibc**.
They fail on Termux because:

1. The ELF interpreter (`/lib/ld-linux-aarch64.so.1`) does not exist on Android.
2. The kernel's `execve()` cannot resolve it, returning *ENOENT*.
3. Even invoking the loader manually breaks child processes and `/proc/self/exe`.

**bionilux** fixes all three problems:

- Invokes the glibc dynamic linker directly.
- Intercepts `execve()` in child processes via an `LD_PRELOAD` library so they are
  transparently re-routed through the loader.
- Hooks `readlink("/proc/self/exe")` so binaries can locate their own resources.

For **x86\_64** binaries bionilux additionally chains through
[box64](https://github.com/ptitSeb/box64) for user-space emulation.

## How It Works

![bionilux Architecture](assets/bionilux_architecture_diagram.png)

## Installation

### Option 1 — Quick installer (recommended)

```bash
curl -sL theonuverse.github.io/bionilux/setup | bash
```

The installer:

- Installs `binutils`, `glibc-repo`, `clang`, `curl`, `file`, `git`, and `glibc`.
- Fixes the `libc.so` linker-script symlink.
- Downloads `bionilux` and the preload library from the **v0.2.0** release.
- Fetches `box64` from [Pi-Apps-Coders/box64-debs](https://github.com/Pi-Apps-Coders/box64-debs).
- Fetches x86\_64 runtime libraries from [easycli.sh](https://easycli.sh/proot-distro/).

### Option 2 — Build from source

```bash
yes | pkg up && pkg in git -y
git clone https://github.com/theonuverse/bionilux.git
cd bionilux
./build            # compile + install (auto-installs all dependencies)
```
> ```./build -c       # clean reinstall```

The only prerequisite is `git` (to clone the repo). The build script
automatically installs `binutils`, `glibc-repo`, `clang`, `curl`, `file`,
`git`, and `glibc`, then fetches `box64` and x86\_64 runtime libraries
from upstream before compiling.

The build script installs:

| Artefact | Destination |
|----------|-------------|
| `bionilux` | `$PREFIX/bin/` |
| `libbionilux_preload.so` | `$PREFIX/glibc/lib/` |
| `box64` | `$PREFIX/bin/` |
| x86\_64 compat libs | `$PREFIX/glibc/lib/x86_64-linux-gnu/` |

## Usage

```
bionilux [options] [--] <binary> [args ...]
```

Use `--` to separate bionilux flags from the binary's flags:

```bash
bionilux -- ./program --help
```

### Options

| Flag | Description |
|------|-------------|
| `-d`, `--debug` | Verbose debug output |
| `-n`, `--no-preload` | Do not inject the preload library |
| `-h`, `--help` | Show help text |
| `-v`, `--version` | Print version |

### Examples

```bash
# ARM64 glibc binary
bionilux ./geekbench6

# x86_64 binary (box64 is selected automatically)
bionilux ./bedrock_server

# Debug mode
bionilux -d ./myapp

# Without preload (simple static binaries)
bionilux -n ./static_hello
```

## Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `BOX64_LD_LIBRARY_PATH` | `$PREFIX/glibc/lib/x86_64-linux-gnu` | x86\_64 library search path |
| `BIONILUX_GLIBC_LIB` | `$PREFIX/glibc/lib` | glibc ARM64 library path |
| `BIONILUX_GLIBC_LOADER` | `$PREFIX/glibc/lib/ld-linux-aarch64.so.1` | glibc dynamic linker |
| `BIONILUX_DEBUG` | *(unset)* | Set to `1` for debug output |
| `BIONILUX_ORIG_EXE` | *(internal)* | Original binary path for `/proc/self/exe` fix |

## Example: Running Geekbench 6 for ARM

```bash
cd ~
curl -fLO https://cdn.geekbench.com/Geekbench-6.6.0-LinuxARMPreview.tar.gz
tar xf Geekbench-6.6.0-LinuxARMPreview.tar.gz
cd Geekbench-6.6.0-LinuxARMPreview
bionilux ./geekbench6
```

## Technical Details

### ELF Detection

bionilux uses `pread()` to inspect an ELF binary **without** loading the entire file:

1. Read the ELF header — verify magic, class (64-bit), and machine (aarch64 / x86\_64).
2. Walk `PT_INTERP` program headers to extract the interpreter path.
3. Classify the interpreter: **glibc** (`ld-linux`), **bionic** (`linker64`), or **musl** (`ld-musl`).
4. Musl binaries are rejected (they are incompatible with a glibc loader).

### Hooked Functions (preload library)

| Function | Purpose |
|----------|---------|
| `execve()` | Re-routes glibc binaries through the loader |
| `execv()` | Wrapper → `execve()` |
| `execvp()` | PATH resolution + `execve()` |
| `execvpe()` | PATH resolution + `execve()` with custom envp |
| `readlink()` | Returns `BIONILUX_ORIG_EXE` for `/proc/self/exe` |
| `readlinkat()` | Same fix using `fd` + path |

## Troubleshooting

### "Binary not found"

bionilux searches `$PATH` first, then falls back to the current directory:

```bash
bionilux program            # searches PATH, then CWD
bionilux ./program          # relative path (direct)
bionilux /full/path/program # absolute path (direct)
```

### Child processes crash or hang

Enable debug mode to trace the preload library's decisions:

```bash
bionilux -d ./program
```

### Missing x86\_64 libraries

Ensure the compat libraries are present:

```bash
ls "$PREFIX/glibc/lib/x86_64-linux-gnu/"
# Expected: libgcc_s.so.1  libstdc++.so.6  libssl.so.1.1 …
```

If they are missing, re-run the build script or download them manually into
`$PREFIX/glibc/lib/x86_64-linux-gnu/`.

## License

MIT — see [LICENSE](LICENSE) for details.



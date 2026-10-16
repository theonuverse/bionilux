#!/data/data/com.termux/files/usr/bin/bash
# Build libbionilux_preload.so against the Termux glibc sysroot.
#
# Usage:
#   scripts/build-preload.sh              build into the project root
#   scripts/build-preload.sh --install    build and install into $GLIBC/lib/
#
# Override the glibc location:
#   GLIBC=/path/to/glibc scripts/build-preload.sh
set -eu

PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
GLIBC="${GLIBC:-/data/data/com.termux/files/usr/glibc}"

OUT="$PROJECT/libbionilux_preload.so"
SRC="$PROJECT/src/preload/hooks.c"

install_mode=0
case "${1:-}" in
    --install|-i) install_mode=1 ;;
    --help|-h)
        sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    "") ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
esac

# ── checks ──────────────────────────────────────────────────────────
[ -f "$SRC" ] || { echo "missing: $SRC" >&2; exit 1; }
[ -d "$GLIBC/include" ] \
    || { echo "glibc include dir not found: $GLIBC/include" >&2
         echo "Set GLIBC=/path or install glibc-repo + glibc." >&2
         exit 1; }
[ -f "$GLIBC/lib/libc.so.6" ] \
    || { echo "glibc libc.so.6 not found under $GLIBC/lib" >&2; exit 1; }
command -v clang >/dev/null \
    || { echo "clang not found in PATH" >&2; exit 1; }

# ── build ───────────────────────────────────────────────────────────
echo "building $OUT"
echo "  from $SRC"
echo "  glibc $GLIBC"

clang --target=aarch64-linux-gnu \
      -shared -fPIC -O2 -nostdlib \
      -Wall -Wextra \
      -isystem "$GLIBC/include" \
      -o "$OUT" \
      "$SRC" \
      "$GLIBC/lib/libc.so.6" \
      "$GLIBC/lib/libdl.so.2" \
      -Wl,--dynamic-linker="$GLIBC/lib/ld-linux-aarch64.so.1" \
      -Wl,-rpath,"$GLIBC/lib"

size=$(wc -c < "$OUT")
echo "ok: $OUT ($size bytes)"

# ── optional install ────────────────────────────────────────────────
if [ "$install_mode" -eq 1 ]; then
    DEST="$GLIBC/lib/libbionilux_preload.so"
    install -m755 "$OUT" "$DEST"
    echo "installed: $DEST"
fi
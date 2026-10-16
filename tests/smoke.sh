#!/data/data/com.termux/files/usr/bin/bash
# bionilux end-to-end smoke tests. Run from the project root in a native
# Termux shell. Requires bionilux built, preload present, glibc installed.
set -u

PROJECT="$(cd "$(dirname "$0")/.." && pwd)"
BIONILUX="$PROJECT/target/debug/bionilux"
GLIBC="${GLIBC:-/data/data/com.termux/files/usr/glibc}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

G='\033[0;32m' R='\033[0;31m' Y='\033[1;33m' N='\033[0m'
pass() { printf "${G}[PASS]${N} %s\n" "$*"; }
fail() { printf "${R}[FAIL]${N} %s\n" "$*"; FAILED=$((FAILED + 1)); }
info() { printf "${Y}[info]${N} %s\n" "$*"; }

FAILED=0

# ─── preflight ──────────────────────────────────────────────────────
[ -x "$BIONILUX" ] || { echo "bionilux missing: $BIONILUX"; exit 1; }
[ -d "$GLIBC" ]    || { echo "glibc missing: $GLIBC"; exit 1; }

PRELOAD="$PROJECT/libbionilux_preload.so"
[ -f "$PRELOAD" ] || PRELOAD="$GLIBC/lib/libbionilux_preload.so"
[ -f "$PRELOAD" ] || { echo "preload missing"; exit 1; }

# Warn if hooks.c is newer than the preload we're testing.
if [ "$PROJECT/src/preload/hooks.c" -nt "$PRELOAD" ]; then
    echo "hooks.c is newer than $PRELOAD — rebuild first:"
    echo "  $PROJECT/scripts/build-preload.sh"
    exit 1
fi

BOX64="$PREFIX/bionilux/box64-glibc"
[ -x "$BOX64" ] || info "box64-glibc not installed at $BOX64 (x86 tests skipped)"

# ─── build test helpers (aarch64-glibc) ─────────────────────────────
build_glibc_bin() {
    local src="$1" out="$2"
    clang --target=aarch64-linux-gnu \
          -isystem "$GLIBC/include" \
          -nostdlib \
          -o "$out" \
          "$GLIBC/lib/Scrt1.o" "$GLIBC/lib/crti.o" \
          "$src" \
          "$GLIBC/lib/crtn.o" \
          "$GLIBC/lib/libc.so.6" \
          -Wl,--dynamic-linker="$GLIBC/lib/ld-linux-aarch64.so.1" \
          -Wl,-rpath,"$GLIBC/lib"
}

info "building helpers"
build_glibc_bin "$PROJECT/tests/helpers/execle.c"    "$TMP/execle_test" || exit 1
build_glibc_bin "$PROJECT/tests/helpers/self_call.c" "$TMP/self_call"   || exit 1
build_glibc_bin "$PROJECT/tests/helpers/fexecve.c"   "$TMP/fexecve_test" || exit 1
build_glibc_bin "$PROJECT/tests/helpers/exit_code.c"        "$TMP/exit_code_test"        || exit 1
build_glibc_bin "$PROJECT/tests/helpers/print_argv0.c"      "$TMP/print_argv0_test"      || exit 1
build_glibc_bin "$PROJECT/tests/helpers/execvp_missing.c"   "$TMP/execvp_missing_test"   || exit 1
build_glibc_bin "$PROJECT/tests/helpers/system_test.c"      "$TMP/system_test"           || exit 1
build_glibc_bin "$PROJECT/tests/helpers/getifaddrs_test.c"  "$TMP/getifaddrs_test"       || exit 1
pass "built 8 helpers"

# ─── 1. execle with multiple args + custom envp ─────────────────────
out=$(BIONILUX_DEBUG= "$BIONILUX" "$TMP/execle_test" 2>/dev/null || true)
if [ "$out" = "hello" ]; then
    pass "execle (multi-arg + envp)"
else
    fail "execle (multi-arg + envp): got '$out'"
fi

# ─── 2. fexecve on the helper's own path ────────────────────────────
out=$(BIONILUX_DEBUG= "$BIONILUX" "$TMP/fexecve_test" 2>/dev/null || true)
if [ "$out" = "fexecve-ok" ]; then
    pass "fexecve"
else
    fail "fexecve: got '$out'"
fi

# ─── 3. launcher self-call from glibc process ───────────────────────
for mode in execve execvp posix_spawn; do
    out=$(BIONILUX_DEBUG= "$BIONILUX" "$GLIBC/bin/bash" -c \
          "$TMP/self_call $mode 2>/dev/null" 2>/dev/null || true)
    if [ "$out" = "self-ok" ]; then
        pass "launcher self-call: $mode"
    else
        fail "launcher self-call: $mode (got '$out')"
    fi
done

# ─── 4. env -S shebang ──────────────────────────────────────────────
cat > "$TMP/envS.sh" <<'EOF'
#!/usr/bin/env -S sh -eu
echo "env-S-ok"
EOF
chmod +x "$TMP/envS.sh"
out=$("$BIONILUX" "$TMP/envS.sh" 2>/dev/null || true)
if [ "$out" = "env-S-ok" ]; then
    pass "shebang env -S"
else
    fail "shebang env -S: got '$out'"
fi

# ─── 5. shebang recursion limit ─────────────────────────────────────
printf '#!%s/loop\n' "$TMP" > "$TMP/loop"
chmod +x "$TMP/loop"
out=$("$BIONILUX" "$TMP/loop" 2>&1 || true)
if echo "$out" | grep -q "recursion too deep"; then
    pass "shebang recursion limit"
else
    fail "shebang recursion limit: got '$out'"
fi

# ─── 6. exit code propagates ────────────────────────────────────────
"$BIONILUX" "$TMP/exit_code_test" >/dev/null 2>&1
rc=$?
if [ $rc -eq 42 ]; then
    pass "exit code 42 propagated"
else
    fail "exit code: expected 42, got $rc"
fi

# ─── 7. argv[0] preserved when called directly ──────────────────────
out=$("$BIONILUX" "$TMP/print_argv0_test" 2>/dev/null || true)
if echo "$out" | grep -q "argv0=$TMP/print_argv0_test"; then
    pass "argv[0] preserved (direct)"
else
    fail "argv[0] preserved (direct): got '$out'"
fi

# ─── 8. exec -a name from shell ─────────────────────────────────────
out=$("$BIONILUX" "$GLIBC/bin/bash" -c \
      "exec -a customname $TMP/print_argv0_test" 2>/dev/null || true)
if echo "$out" | grep -q "argv0=customname"; then
    pass "exec -a preserves custom argv[0]"
else
    fail "exec -a: got '$out'"
fi

# ─── 9. execvp on missing name returns ENOENT to caller ─────────────
out=$("$BIONILUX" "$TMP/execvp_missing_test" 2>/dev/null || true)
if [ "$out" = "enoent-ok" ]; then
    pass "execvp missing name -> ENOENT"
else
    fail "execvp missing name: got '$out'"
fi

# ─── 10. system() runs ──────────────────────────────────────────────
out=$("$BIONILUX" "$TMP/system_test" 2>/dev/null || true)
if echo "$out" | grep -q "system-ok"; then
    pass "system() works"
else
    fail "system(): got '$out'"
fi

# ─── 11. getifaddrs returns IPv4 interfaces ─────────────────────────
out=$("$BIONILUX" "$TMP/getifaddrs_test" 2>/dev/null || true)
if echo "$out" | grep -q "ifaddrs-ok"; then
    pass "getifaddrs fallback"
else
    fail "getifaddrs: got '$out'"
fi

# ─── 12. missing path → shell ENOENT ─────────────────────────────────
# Distinguish shell's "No such file or directory" from launcher's
# "command not found": only the former proves the hook passed through.
out=$("$BIONILUX" "$GLIBC/bin/bash" -c '/no/such/path' 2>&1 || true)
if echo "$out" | grep -q "No such file or directory"; then
    pass "missing path -> shell ENOENT"
else
    fail "missing path -> shell ENOENT (got '$out')"
fi

# ─── 13. box64 chain (optional) ──────────────────────────────────────
if [ -x "$BOX64" ] && [ -f "$HOME/server/bedrock_server" ]; then
    timeout 3 "$BIONILUX" "$HOME/server/bedrock_server" >/dev/null 2>&1
    rc=$?
    # timeout kills with 124. Server running > 3s means the chain works.
    if [ $rc -eq 124 ]; then
        pass "box64 chain (bedrock_server ran >3s)"
    else
        info "box64 chain: server exited rc=$rc (may be normal)"
    fi
fi

# ─── summary ────────────────────────────────────────────────────────
if [ "$FAILED" -eq 0 ]; then
    printf "\n${G}all tests passed${N}\n"
    exit 0
else
    printf "\n${R}%d test(s) failed${N}\n" "$FAILED"
    exit 1
fi
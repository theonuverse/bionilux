#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Minimal smoke test for ovgl (runs on Termux after ./build)
#
set -eu

PASS=0
FAIL=0
PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
OVGL="$PREFIX/bin/ovgl"

pass() { printf "\033[0;32mPASS\033[0m  %s\n" "$1"; PASS=$((PASS + 1)); }
fail() { printf "\033[0;31mFAIL\033[0m  %s\n" "$1"; FAIL=$((FAIL + 1)); }

# ── ovgl binary exists and is executable ─────────────────────────────
if [ -x "$OVGL" ]; then
	pass "ovgl binary exists"
else
	fail "ovgl binary not found at $OVGL"
fi

# ── --version prints 0.2.0 ──────────────────────────────────────────
if "$OVGL" --version 2>&1 | grep -q "0\.2\.0"; then
	pass "--version reports 0.2.0"
else
	fail "--version does not report 0.2.0"
fi

# ── --help exits 0 ──────────────────────────────────────────────────
if "$OVGL" --help >/dev/null 2>&1; then
	pass "--help exits 0"
else
	fail "--help exits non-zero"
fi

# ── no args → non-zero exit ─────────────────────────────────────────
if "$OVGL" >/dev/null 2>&1; then
	fail "no args should exit non-zero"
else
	pass "no args exits non-zero"
fi

# ── unknown option → non-zero exit ──────────────────────────────────
if "$OVGL" --bogus >/dev/null 2>&1; then
	fail "--bogus should exit non-zero"
else
	pass "--bogus exits non-zero"
fi

# ── nonexistent binary → exit 127 ───────────────────────────────────
"$OVGL" ./nonexistent_binary_xyz >/dev/null 2>&1
rc=$?
if [ "$rc" -eq 127 ]; then
	pass "nonexistent binary exits 127"
else
	fail "nonexistent binary exits $rc (expected 127)"
fi

# ── preload library exists ──────────────────────────────────────────
PRELOAD="$PREFIX/glibc/lib/libovgl_preload.so"
if [ -f "$PRELOAD" ]; then
	pass "preload library exists"
else
	fail "preload library not found at $PRELOAD"
fi

# ── x86_64 lib dir exists ───────────────────────────────────────────
X86DIR="$PREFIX/glibc/lib/x86_64-linux-gnu"
if [ -d "$X86DIR" ]; then
	pass "x86_64 lib dir exists"
else
	fail "x86_64 lib dir not found at $X86DIR"
fi

# ── summary ─────────────────────────────────────────────────────────
printf "\n%d passed, %d failed\n" "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]

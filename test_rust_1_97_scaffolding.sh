#!/bin/sh
set -eu

version=1.97.1
target=1.97

test "$(tr -d '[:space:]' < rust-version)" = "$version"
test -f "build-$version.sh"
test -f "rustc-$version-src.patch"
test -f "rustc-$version-overrides.toml"
test -f samples/no_core-1_97.rs
test -f "script-overrides/stable-$version-linux/build_libc.txt"
test -f "script-overrides/stable-$version-linux/build_std.txt"
test -f "script-overrides/stable-$version-linux/build_compiler_builtins.txt"
test -f "script-overrides/stable-$version-linux/build_test.txt"

grep -Fq "X_TARGET_VERSION(Rustc1_97, \"$target\")" src/include/target_versions.def
grep -Fq 'TARGETVER_LEAST_1_97' src/include/target_version.hpp
grep -Fq 'TargetVersion::Rustc1_97' src/main.cpp
grep -Fq 'RUSTC_VERSION=1.97.1 MRUSTC_TARGET_VER=1.97 OUTDIR_SUF=-1.97.1' "build-$version.sh"

make_vars=$(mktemp)
trap 'rm -f "$make_vars"' EXIT HUP INT TERM
make -f minicargo.mk -pn RUSTC_VERSION="$version" >"$make_vars" 2>/dev/null || true
grep -Eq '^RUST_LIB_PREFIX[[:space:]]*:?=[[:space:]]*library/' "$make_vars"
grep -Eq '^SRCDIR_RUSTC[[:space:]]*:?=[[:space:]]*compiler/rustc$' "$make_vars"
grep -Eq '^SRCDIR_RUSTC_DRIVER[[:space:]]*:?=[[:space:]]*compiler/rustc_driver$' "$make_vars"
grep -Eq '^SRCDIR_RUST_TESTS[[:space:]]*:?=[[:space:]]*rustc-1.97.1-src/tests/$' "$make_vars"
grep -Eq '^OVERRIDE_DIR[[:space:]]*:?=[[:space:]]*script-overrides/stable-1.97.1-linux/$' "$make_vars"

printf 'Rust %s first-class target scaffolding is internally coherent\n' "$version"

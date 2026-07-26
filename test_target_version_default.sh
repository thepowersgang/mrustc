#!/bin/sh
set -eu

compiler=${1:-bin/mrustc}
rustc_version=$(tr -d '[:space:]' < rust-version)
expected_target=${rustc_version%.*}

unset MRUSTC_TARGET_VER
default_output=$("$compiler" -vV 2>/dev/null)
case "$default_output" in
"rustc $expected_target."*) ;;
*)
    printf 'unset MRUSTC_TARGET_VER selected the wrong target:\n%s\n' "$default_output" >&2
    exit 1
    ;;
esac

for version in 1.19 1.29 1.39 1.54 1.74 1.90 1.97; do
    override_output=$(MRUSTC_TARGET_VER=$version "$compiler" -vV)
    case "$override_output" in
    "rustc $version."*) ;;
    *)
        printf 'explicit MRUSTC_TARGET_VER=%s was not preserved:\n%s\n' "$version" "$override_output" >&2
        exit 1
        ;;
    esac
done

if unknown_output=$(MRUSTC_TARGET_VER=1.98 "$compiler" -vV 2>&1); then
    printf 'unknown MRUSTC_TARGET_VER=1.98 unexpectedly succeeded:\n%s\n' "$unknown_output" >&2
    exit 1
fi
case "$unknown_output" in
*'$MRUSTC_TARGET_VER set to an unknown value'*) ;;
*)
    printf 'unknown MRUSTC_TARGET_VER=1.98 returned the wrong error:\n%s\n' "$unknown_output" >&2
    exit 1
    ;;
esac

printf 'target-version default matches rust-version (%s); known and unknown overrides behave correctly\n' "$rustc_version"

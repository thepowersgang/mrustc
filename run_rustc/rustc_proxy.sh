#!/bin/sh
set -eu

is_target() {
    for a  in "$@"
    do
        case "$a" in
        --target|--target*|-vV)
            return 0
            ;;
        esac
    done
    return 1
}

if is_target "$@"; then
    #echo "  [REAL]" "$@" >&2
    ${PROXY_RUSTC} "$@"
else
    #echo "  [BOOTSTRAP]" "$@" >&2
    ${PROXY_MRUSTC} "$@"
fi


#!/bin/bash
set -e

# Default to using deferred codegen. It reduces peak memory usage, but may cause link errors
# due to race conditions
export MINICARGO_DEFER_CODEGEN=${MINICARGO_DEFER_CODEGEN:-1}

export RUSTC_VERSION=1.97.1 MRUSTC_TARGET_VER=1.97 OUTDIR_SUF=-1.97.1
# Enables use of ccache in mrustc if it's available (i.e. ccache is on PATH)
command -v ccache >/dev/null && export MRUSTC_CCACHE=1
make
make RUSTCSRC
make -f minicargo.mk LIBS $@
make test $@
make local_tests $@

OUTDIR=output-1.97.1
if [[ "x$MRUSTC_TARGET" != "x" ]]; then
	OUTDIR=$OUTDIR-$MRUSTC_TARGET
fi

RUSTC_INSTALL_BINDIR=bin make -f minicargo.mk $OUTDIR/rustc $@
set -x
./$OUTDIR/rustc --version
./$OUTDIR/rustc samples/no_core-1_97.rs
set +x

LIBGIT2_SYS_USE_PKG_CONFIG=1 make -f minicargo.mk -j ${PARLEVEL:-1} $OUTDIR/cargo $@
set -x
./$OUTDIR/cargo --version

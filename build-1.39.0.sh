#!/bin/bash
set -e
export RUSTC_VERSION=1.39.0 MRUSTC_TARGET_VER=1.39 OUTDIR_SUF=-1.39.0
make
make -f minicargo.mk RUSTCSRC $@
make -f minicargo.mk LIBS $@
make -f minicargo.mk test $@
make -f minicargo.mk local_tests $@
RUSTC_INSTALL_BINDIR=bin make -f minicargo.mk output-1.39.0/rustc $@ -j ${PARLEVEL:-1}
LIBGIT2_SYS_USE_PKG_CONFIG=1 make -f minicargo.mk output-1.39.0/cargo $@ -j ${PARLEVEL:-1}
./output-1.39.0/cargo --version

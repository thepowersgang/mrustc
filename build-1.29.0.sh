#!/bin/bash
set -e
export RUSTC_VERSION=1.29.0 MRUSTC_TARGET_VER=1.29 OUTDIR_SUF=-1.29.0
make
make -f minicargo.mk RUSTCSRC $@
make -f minicargo.mk LIBS $@
make -f minicargo.mk test $@
make -f minicargo.mk output-1.29.0/rustc $@
make -f minicargo.mk output-1.29.0/cargo $@
./output-1.29.0/cargo --version

#!/bin/bash
set -e
export RUSTC_VERSION=1.19.0 MRUSTC_TARGET_VER=1.19 OUTDIR_SUF=-1.19.0
make
make -f minicargo.mk RUSTCSRC
make -f minicargo.mk LIBS
make -f minicargo.mk test
make -f minicargo.mk output-1.19.0/stdtest/rustc_data_structures-test_out.txt
make -f minicargo.mk output-1.19.0/rustc -j ${PARLEVEL:-1}
make -f minicargo.mk output-1.19.0/cargo -j ${PARLEVEL:-1}
./output-1.19.0/cargo --version

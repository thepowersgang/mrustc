#!/bin/bash
set -e
make
RUSTC_VERSION=1.19.0 MRUSTC_TARGET_VER=1.19 OUTDIR_SUF=-1.19.0 make RUSTCSRC
RUSTC_VERSION=1.19.0 MRUSTC_TARGET_VER=1.19 OUTDIR_SUF=-1.19.0 make test
RUSTC_VERSION=1.19.0 MRUSTC_TARGET_VER=1.19 OUTDIR_SUF=-1.19.0 make -f minicargo.mk LIBS
RUSTC_VERSION=1.19.0 MRUSTC_TARGET_VER=1.19 OUTDIR_SUF=-1.19.0 make -f minicargo.mk output-1.19.0/stdtest/rustc_data_structures-test_out.txt
RUSTC_VERSION=1.19.0 MRUSTC_TARGET_VER=1.19 OUTDIR_SUF=-1.19.0 make -f minicargo.mk output-1.19.0/rustc
RUSTC_VERSION=1.19.0 MRUSTC_TARGET_VER=1.19 OUTDIR_SUF=-1.19.0 make -f minicargo.mk output-1.19.0/cargo

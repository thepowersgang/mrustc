#!/bin/bash
set -e
make
RUSTC_VERSION=1.54.0 MRUSTC_TARGET_VER=1.54 OUTDIR_SUF=-1.54.0 make RUSTCSRC
RUSTC_VERSION=1.54.0 MRUSTC_TARGET_VER=1.54 OUTDIR_SUF=-1.54.0 make -f minicargo.mk LIBS
RUSTC_VERSION=1.54.0 MRUSTC_TARGET_VER=1.54 OUTDIR_SUF=-1.54.0 make test
RUSTC_VERSION=1.54.0 MRUSTC_TARGET_VER=1.54 OUTDIR_SUF=-1.54.0 make local_tests
# Build just rustc-driver BEFORE building llvm
RUSTC_INSTALL_BINDIR=bin RUSTC_VERSION=1.54.0 MRUSTC_TARGET_VER=1.54 OUTDIR_SUF=-1.54.0 make -f minicargo.mk output-1.54.0/rustc-build/librustc_driver.rlib
RUSTC_INSTALL_BINDIR=bin RUSTC_VERSION=1.54.0 MRUSTC_TARGET_VER=1.54 OUTDIR_SUF=-1.54.0 make -f minicargo.mk output-1.54.0/rustc
./output-1.54.0/rustc --version

LIBGIT2_SYS_USE_PKG_CONFIG=1 RUSTC_VERSION=1.54.0 MRUSTC_TARGET_VER=1.54 OUTDIR_SUF=-1.54.0 make -f minicargo.mk output-1.54.0/cargo
./output-1.54.0/cargo --version

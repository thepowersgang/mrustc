#!/bin/sh
# Builds rustc with the mrustc stage0 and downloaded stage0
set -e

PREFIX=${PWD}/run_rustc/prefix/
WORKDIR=${WORKDIR:-rustc_bootstrap}/

MAKEFLAGS=-j8
export MAKEFLAGS

echo "=== Building stage0 rustc (with libstd)"
make -C run_rustc

echo "--- Working in directory ${WORKDIR}"
echo "=== Building rustc bootstrap mrustc stage0"
mkdir -p ${WORKDIR}mrustc/
tar -xf rustc-1.19.0-src.tar.gz -C ${WORKDIR}mrustc/
cat - > ${WORKDIR}mrustc/rustc-1.19.0-src/config.toml <<EOF
[build]
cargo = "${PREFIX}bin/cargo"
rustc = "${PREFIX}bin/rustc"
EOF
echo "--- Running x.py, see ${WORKDIR}mrustc.log for progress"
(cd ${WORKDIR}mrustc/rustc-1.19.0-src/ && ./x.py build) > ${WORKDIR}mrustc.log 2>&1

echo "=== Building rustc bootstrap downloaded stage0"
mkdir -p ${WORKDIR}official/
tar -xf rustc-1.19.0-src.tar.gz -C ${WORKDIR}official/
echo "--- Running x.py, see ${WORKDIR}official.log for progress"
(cd ${WORKDIR}official/rustc-1.19.0-src/ && ./x.py build) > ${WORKDIR}official.log 2>&1

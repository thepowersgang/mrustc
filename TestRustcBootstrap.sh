#!/bin/sh
# Builds rustc with the mrustc stage0 and downloaded stage0
set -e

PREFIX=${PWD}/run_rustc/prefix/
WORKDIR=${WORKDIR:-rustc_bootstrap}/

MAKEFLAGS=-j8
export MAKEFLAGS

echo "=== Building stage0 rustc (with libstd)"
make -C run_rustc

if [ ! -e rustc-1.20.0-src.tar.gz ]; then
    wget https://static.rust-lang.org/dist/rustc-1.20.0-src.tar.gz
fi

#
# Build rustc using entirely mrustc-built tools
#
echo "--- Working in directory ${WORKDIR}"
echo "=== Building rustc bootstrap mrustc stage0"
mkdir -p ${WORKDIR}mrustc/
tar -xf rustc-1.20.0-src.tar.gz -C ${WORKDIR}mrustc/
cat - > ${WORKDIR}mrustc/rustc-1.20.0-src/config.toml <<EOF
[build]
cargo = "${PREFIX}bin/cargo"
rustc = "${PREFIX}bin/rustc"
full-bootstrap = true
vendor = true
EOF
echo "--- Running x.py, see ${WORKDIR}mrustc.log for progress"
(cd ${WORKDIR} && mv mrustc build)
(cd ${WORKDIR}build/rustc-1.20.0-src/ && ./x.py build --stage 3) > ${WORKDIR}mrustc.log 2>&1
(cd ${WORKDIR} && mv build mrustc)
rm -rf ${WORKDIR}mrustc-output
cp -r ${WORKDIR}mrustc/rustc-1.20.0-src/build/x86_64-unknown-linux-gnu/stage2 ${WORKDIR}mrustc-output
tar -czf ${WORKDIR}mrustc.tar.gz -C ${WORKDIR} mrustc-output

#
# Build rustc by downloading the previous version of rustc (and its matching cargo)
#
echo "=== Building rustc bootstrap downloaded stage0"
mkdir -p ${WORKDIR}official/
tar -xf rustc-1.20.0-src.tar.gz -C ${WORKDIR}official/
cat - > ${WORKDIR}official/rustc-1.20.0-src/config.toml <<EOF
[build]
full-bootstrap = true
vendor = true
EOF
echo "--- Running x.py, see ${WORKDIR}official.log for progress"
(cd ${WORKDIR} && mv official build)
(cd ${WORKDIR}build/rustc-1.20.0-src/ && ./x.py build --stage 3) > ${WORKDIR}official.log 2>&1
(cd ${WORKDIR} && mv build official)
rm -rf ${WORKDIR}official-output
cp -r ${WORKDIR}official/rustc-1.20.0-src/build/x86_64-unknown-linux-gnu/stage2 ${WORKDIR}official-output
tar -czf ${WORKDIR}official.tar.gz -C ${WORKDIR} official-output

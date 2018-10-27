#!/bin/sh
# Builds rustc with the mrustc stage0 and downloaded stage0
set -e

PREFIX=${PWD}/run_rustc/prefix/
WORKDIR=${WORKDIR:-rustc_bootstrap}/

MAKEFLAGS=-j8
export MAKEFLAGS

if [ -z ${RUSTC_SRC_DES} ]; then
RUSTC_SRC_DES=rust-version
fi
if [ -z ${RUSTC_VERSION} ]; then
RUSTC_VERSION=$( cat ${RUSTC_SRC_DES} )
fi
echo "RUSTC_VERSION: ${RUSTC_VERSION}"
RUSTC_SRC_NAME=rustc-${RUSTC_VERSION}-src

echo "=== Building stage0 rustc (with libstd)"
make -C run_rustc

RUSTC_SRC_TARBALL="${RUSTC_SRC_NAME}.tar.gz"
if [ ! -e ${RUSTC_SRC_TARBALL} ]; then
    echo "--- fetching ${RUSTC_SRC_TARBALL}"
    wget https://static.rust-lang.org/dist/${RUSTC_SRC_TARBALL}
fi

#
# Build rustc using entirely mrustc-built tools
#
echo "--- Working in directory ${WORKDIR}"
echo "=== Building rustc bootstrap mrustc stage0"
mkdir -p ${WORKDIR}mrustc/
tar -xf ${RUSTC_SRC_TARBALL} -C ${WORKDIR}mrustc/
cat - > ${WORKDIR}mrustc/${RUSTC_SRC_NAME}/config.toml <<EOF
[build]
cargo = "${PREFIX}bin/cargo"
rustc = "${PREFIX}bin/rustc"
full-bootstrap = true
vendor = true
EOF
echo "--- Running x.py, see ${WORKDIR}mrustc.log for progress"
(cd ${WORKDIR} && mv mrustc build)
(cd ${WORKDIR}build/${RUSTC_SRC_NAME}/ && ./x.py build --stage 3) > ${WORKDIR}mrustc.log 2>&1
(cd ${WORKDIR} && mv build mrustc)
rm -rf ${WORKDIR}mrustc-output
cp -r ${WORKDIR}mrustc/${RUSTC_SRC_NAME}/build/x86_64-unknown-linux-gnu/stage2 ${WORKDIR}mrustc-output
tar -czf ${WORKDIR}mrustc.tar.gz -C ${WORKDIR} mrustc-output

#
# Build rustc by downloading the previous version of rustc (and its matching cargo)
#
echo "=== Building rustc bootstrap downloaded stage0"
mkdir -p ${WORKDIR}official/
tar -xf ${RUSTC_SRC_NAME}.tar.gz -C ${WORKDIR}official/
cat - > ${WORKDIR}official/${RUSTC_SRC_NAME}/config.toml <<EOF
[build]
full-bootstrap = true
vendor = true
EOF
echo "--- Running x.py, see ${WORKDIR}official.log for progress"
(cd ${WORKDIR} && mv official build)
(cd ${WORKDIR}build/${RUSTC_SRC_NAME}/ && ./x.py build --stage 3) > ${WORKDIR}official.log 2>&1
(cd ${WORKDIR} && mv build official)
rm -rf ${WORKDIR}official-output
cp -r ${WORKDIR}official/${RUSTC_SRC_NAME}/build/x86_64-unknown-linux-gnu/stage2 ${WORKDIR}official-output
tar -czf ${WORKDIR}official.tar.gz -C ${WORKDIR} official-output

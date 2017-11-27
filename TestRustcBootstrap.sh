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
# > Patch rustc being built by mrustc, remove a feature stablised in 1.19.0
(cd ${WORKDIR}mrustc/rustc-1.19.0-src && patch -p0) <<EOF
+++ src/librustc/lib.rs
--- src/librustc/lib.rs
@@ -48,1 +48,0 @@
-#![cfg_attr(stage0, feature(loop_break_value))]
+++ src/librustc_typeck/lib.rs
--- src/librustc_typeck/lib.rs
@@ -88,1 +88,0 @@
-#![cfg_attr(stage0, feature(loop_break_value))]
+++ src/librustc_driver/lib.rs
--- src/librustc_driver/lib.rs
@@ -34,1 +34,0 @@
-#![cfg_attr(stage0, feature(loop_break_value))]
EOF
cat - > ${WORKDIR}mrustc/rustc-1.19.0-src/config.toml <<EOF
[build]
cargo = "${PREFIX}bin/cargo"
rustc = "${PREFIX}bin/rustc"
EOF
echo "--- Running x.py, see ${WORKDIR}mrustc.log for progress"
(cd ${WORKDIR} && mv mrustc build)
(cd ${WORKDIR}build/rustc-1.19.0-src/ && ./x.py build) > ${WORKDIR}mrustc.log 2>&1
(cd ${WORKDIR} && mv build mrustc)
cp -r ${WORKDIR}mrustc/rustc-1.19.0-src/build/x86_64-unknown-linux-gnu/stage2 ${WORKDIR}mrustc-output
tar -czvf ${WORKDIR}mrustc.tar.gz -C ${WORKDIR} mrustc-output

echo "=== Building rustc bootstrap downloaded stage0"
mkdir -p ${WORKDIR}official/
tar -xf rustc-1.19.0-src.tar.gz -C ${WORKDIR}official/
echo "--- Running x.py, see ${WORKDIR}official.log for progress"
(cd ${WORKDIR} && mv official build)
(cd ${WORKDIR}build/rustc-1.19.0-src/ && ./x.py build) > ${WORKDIR}official.log 2>&1
(cd ${WORKDIR} && mv build official)
cp -r ${WORKDIR}official/rustc-1.19.0-src/build/x86_64-unknown-linux-gnu/stage2 ${WORKDIR}official-output
tar -czvf ${WORKDIR}official.tar.gz -C ${WORKDIR} official-output

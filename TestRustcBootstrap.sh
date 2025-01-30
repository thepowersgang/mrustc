#!/bin/bash
# Builds rustc with the mrustc stage0 and downloaded stage0
set -e  # Quit script on error
set -u  # Error on unset variables

WORKDIR=${WORKDIR:-rustc_bootstrap}/

RUSTC_TARGET=${RUSTC_TARGET:-x86_64-unknown-linux-gnu}
RUSTC_VERSION=${*-1.29.0}
RUN_RUSTC_SUF=""
if [[ "$RUSTC_VERSION" == "1.29.0" ]]; then
    RUSTC_VERSION_NEXT=1.30.0
elif [[ "$RUSTC_VERSION" == "1.19.0" ]]; then
    RUSTC_VERSION_NEXT=1.20.0
    RUN_RUSTC_SUF=-1.19.0
elif [[ "$RUSTC_VERSION" == "1.39.0" ]]; then
    RUSTC_VERSION_NEXT=1.40.0
    RUN_RUSTC_SUF=-1.39.0
elif [[ "$RUSTC_VERSION" == "1.54.0" ]]; then
    RUSTC_VERSION_NEXT=1.55.0
    RUN_RUSTC_SUF=-1.54.0
elif [[ "$RUSTC_VERSION" == "1.74.0" ]]; then
    RUSTC_VERSION_NEXT=1.75.0
    RUN_RUSTC_SUF=-1.74.0
else
    echo "Unknown rustc version"
fi

MAKEFLAGS=-j8
export MAKEFLAGS

echo "=== Building stage0 rustc (with libstd)"
make -C run_rustc RUSTC_VERSION=${RUSTC_VERSION}

PREFIX=${PWD}/run_rustc/output${RUN_RUSTC_SUF}/prefix/

if [ ! -e rustc-${RUSTC_VERSION_NEXT}-src.tar.gz ]; then
    wget https://static.rust-lang.org/dist/rustc-${RUSTC_VERSION_NEXT}-src.tar.gz
fi

echo "--- Working in directory ${WORKDIR}"
echo "=== Cleaning up"
rm -rf ${WORKDIR}build
#
# Build rustc using entirely mrustc-built tools
#
echo "=== Building rustc bootstrap mrustc stage0"
mkdir -p ${WORKDIR}mrustc/
tar -xzf rustc-${RUSTC_VERSION_NEXT}-src.tar.gz -C ${WORKDIR}mrustc/
cat - > ${WORKDIR}mrustc/rustc-${RUSTC_VERSION_NEXT}-src/config.toml <<EOF
[build]
cargo = "${PREFIX}bin/cargo"
rustc = "${PREFIX}bin/rustc"
full-bootstrap = true
vendor = true
extended = true
[llvm]
ninja = false
download-ci-llvm = false
EOF
echo "--- Running x.py, see ${WORKDIR}mrustc.log for progress"
(cd ${WORKDIR} && mv mrustc build)
cleanup_mrustc() {
    (cd ${WORKDIR} && mv build mrustc)
}
trap cleanup_mrustc EXIT
rm -rf ${WORKDIR}build/rustc-${RUSTC_VERSION_NEXT}-src/build
(cd ${WORKDIR}build/rustc-${RUSTC_VERSION_NEXT}-src/ && LD_LIBRARY_PATH=${PREFIX}lib/rustlib/${RUSTC_TARGET}/lib ./x.py build --stage 3) > ${WORKDIR}mrustc.log 2>&1
cleanup_mrustc
trap - EXIT
rm -rf ${WORKDIR}mrustc-output
rm -rf ${WORKDIR}output
cp -r ${WORKDIR}mrustc/rustc-${RUSTC_VERSION_NEXT}-src/build/${RUSTC_TARGET}/stage3 ${WORKDIR}output
cp ${WORKDIR}mrustc/rustc-${RUSTC_VERSION_NEXT}-src/build/${RUSTC_TARGET}/stage3-tools-bin/* ${WORKDIR}output/bin/
rm -rf ${WORKDIR}output/lib/rustlib/src ${WORKDIR}output/lib/rustlib/rustc-src
tar --mtime="@0" --sort=name -czf ${WORKDIR}mrustc.tar.gz -C ${WORKDIR} output
mv ${WORKDIR}output ${WORKDIR}mrustc-output

#
# Build rustc by downloading the previous version of rustc (and its matching cargo)
#
echo "=== Building rustc bootstrap downloaded stage0"
mkdir -p ${WORKDIR}official/
tar -xzf rustc-${RUSTC_VERSION_NEXT}-src.tar.gz -C ${WORKDIR}official/
cat - > ${WORKDIR}official/rustc-${RUSTC_VERSION_NEXT}-src/config.toml <<EOF
[build]
full-bootstrap = true
vendor = true
extended = true
[llvm]
ninja = false
download-ci-llvm = false
EOF
echo "--- Running x.py, see ${WORKDIR}official.log for progress"
(cd ${WORKDIR} && mv official build)
(cd ${WORKDIR}build/rustc-${RUSTC_VERSION_NEXT}-src/ && ./x.py build --stage 3) > ${WORKDIR}official.log 2>&1
(cd ${WORKDIR} && mv build official)
rm -rf ${WORKDIR}official-output
rm -rf ${WORKDIR}output
cp -r ${WORKDIR}official/rustc-${RUSTC_VERSION_NEXT}-src/build/${RUSTC_TARGET}/stage3 ${WORKDIR}output
cp ${WORKDIR}official/rustc-${RUSTC_VERSION_NEXT}-src/build/${RUSTC_TARGET}/stage3-tools-bin/* ${WORKDIR}output/bin/
rm -rf ${WORKDIR}output/lib/rustlib/src ${WORKDIR}output/lib/rustlib/rustc-src
tar --mtime="@0" --sort=name -czf ${WORKDIR}official.tar.gz -C ${WORKDIR} output
mv ${WORKDIR}output ${WORKDIR}official-output

#
# Compare mrustc-built and official build artifacts
#
diff -qs ${WORKDIR}mrustc.tar.gz ${WORKDIR}official.tar.gz

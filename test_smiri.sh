#!/bin/sh
set -e
cd $(dirname $0)
make -f minicargo.mk MMIR=1 LIBS
make -C tools/standalone_miri
echo "--- mrustc -o output-mmir/hello"

if [ -z ${RUSTC_SRC_DES} ]; then
RUSTC_SRC_DES=rust-version
fi
if [ -z ${RUSTC_VERSION} ]; then
RUSTC_VERSION=$( cat ${RUSTC_SRC_DES} )
fi
echo "RUSTC_VERSION: ${RUSTC_VERSION}"
RUSTC_SRC_NAME=rustc-${RUSTC_VERSION}-src
./bin/mrustc ${RUSTC_SRC_NAME}/src/test/run-pass/hello.rs -O -C codegen-type=monomir -o output-mmir/hello -L output-mmir/ > output-mmir/hello_dbg.txt

echo "--- standalone_miri output-mmir/hello.mir"
time ./tools/bin/standalone_miri output-mmir/hello.mir --logfile smiri_hello.log

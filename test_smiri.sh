#!/bin/sh
set -e
cd $(dirname $0)
make all
make -C tools/standalone_miri || exit 1
make -f minicargo.mk MMIR=1 LIBS V= || exit 1
echo "--- mrustc -o output-mmir/hello"
./bin/mrustc rustc-1.19.0-src/src/test/run-pass/hello.rs -O -C codegen-type=monomir -o output-mmir/hello -L output-mmir/ > output-mmir/hello_dbg.txt || exit 1
echo "--- standalone_miri output-mmir/hello.mir"
./bin/standalone_miri output-mmir/hello.mir --logfile smiri_hello.log

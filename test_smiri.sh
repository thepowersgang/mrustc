#!/bin/sh
set -e
cd $(dirname $0)
make -f minicargo.mk MMIR=1 LIBS
make -C tools/standalone_miri
echo "--- mrustc -o output-mmir/hello"
./bin/mrustc rustc-1.19.0-src/src/test/run-pass/hello.rs -O -C codegen-type=monomir -o output-mmir/hello -L output-mmir/ > output-mmir/hello_dbg.txt
echo "--- standalone_miri output-mmir/hello.mir"
time ./tools/bin/standalone_miri output-mmir/hello.mir --logfile smiri_hello.log

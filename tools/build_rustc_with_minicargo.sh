#!/bin/sh
set -e
make -C minicargo
echo "=== libstd"
./bin/minicargo ../rustc-nightly/src/libstd --script-overrides ../script-overrides/nightly-2017-07-08/
echo "=== libpanic_unwind"
./bin/minicargo ../rustc-nightly/src/libpanic_unwind --script-overrides ../script-overrides/nightly-2017-07-08/
echo "=== libpanic_abort"
./bin/minicargo ../rustc-nightly/src/libpanic_abort --script-overrides ../script-overrides/nightly-2017-07-08/
echo "=== libtest"
./bin/minicargo ../rustc-nightly/src/libtest --vendor-dir ../rustc-nightly/src/vendor
#echo "=== rustc"
#./bin/minicargo ../rustc-nightly/src/rustc --vendor-dir ../rustc-nightly/src/vendor

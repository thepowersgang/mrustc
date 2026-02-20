#!/bin/bash
# Builds rustc with the mrustc stage0 and downloaded stage0
set -e  # Quit script on error
set -u  # Error on unset variables

WORKDIR=${WORKDIR:-rustc_bootstrap}/

RUSTC_TARGET=${RUSTC_TARGET:-x86_64-unknown-linux-gnu}
RUSTC_VERSION=${1-1.29.0}
RUN_RUSTC_SUF=""
if [[ "$RUSTC_VERSION" == "1.29.0" ]]; then
    RUSTC_VERSION_NEXT=${2-1.30.0}
elif [[ "$RUSTC_VERSION" == "1.19.0" ]]; then
    RUSTC_VERSION_NEXT=${2-1.20.0}
    RUN_RUSTC_SUF=-1.19.0
elif [[ "$RUSTC_VERSION" == "1.39.0" ]]; then
    RUSTC_VERSION_NEXT=${2-1.40.0}
    RUN_RUSTC_SUF=-1.39.0
elif [[ "$RUSTC_VERSION" == "1.54.0" ]]; then
    RUSTC_VERSION_NEXT=${2-1.55.0}
    RUN_RUSTC_SUF=-1.54.0
elif [[ "$RUSTC_VERSION" == "1.74.0" ]]; then
    RUSTC_VERSION_NEXT=${2-1.75.0}
    RUN_RUSTC_SUF=-1.74.0
elif [[ "$RUSTC_VERSION" == "1.90.0" ]]; then
    # 1.91 had a patch release
    RUSTC_VERSION_NEXT=${2-1.91.1}
    RUN_RUSTC_SUF=-1.90.0
else
    echo "Unknown rustc version ${RUSTC_VERSION}"
    false
fi
shift || true
shift || true

PARLEVEL=${PARLEVEL:-$(nproc)}
MAKEFLAGS=${MAKEFLAGS:--j${PARLEVEL}}
export MAKEFLAGS
MRUSTC_TARGET_VER=${RUSTC_VERSION}
export MRUSTC_TARGET_VER
OUTDIR_SUF=${RUN_RUSTC_SUF}
export OUTDIR_SUF

echo "=== Building stage0 rustc (with libstd)"
make RUSTCSRC all RUSTC_VERSION=${RUSTC_VERSION}
make LIBS RUSTC_VERSION=${RUSTC_VERSION}
MAKEFLAGS=-j1 make -C run_rustc RUSTC_VERSION=${RUSTC_VERSION} PARLEVEL=${PARLEVEL}

PREFIX=${PWD}/run_rustc/output${RUN_RUSTC_SUF}/prefix/

if [ ! -e rustc-${RUSTC_VERSION_NEXT}-src.tar.gz ]; then
    wget https://static.rust-lang.org/dist/rustc-${RUSTC_VERSION_NEXT}-src.tar.gz
fi

echo "--- Working in directory ${WORKDIR}"
echo "=== Cleaning up"
rm -rf ${WORKDIR}build
CONVERGED=n
#
# Build rustc using entirely mrustc-built tools
#
echo "=== Building rustc ${RUSTC_VERSION_NEXT} bootstrap mrustc stage0"
mkdir -p ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}/
tar -xzf rustc-${RUSTC_VERSION_NEXT}-src.tar.gz -C ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}/
cat - > ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}/rustc-${RUSTC_VERSION_NEXT}-src/config.toml <<EOF
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
echo "--- Running x.py, see ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}.log for progress"
(cd ${WORKDIR} && mv mrustc-${RUSTC_VERSION_NEXT} build)
cleanup_mrustc() {
    (cd ${WORKDIR} && mv build mrustc-${RUSTC_VERSION_NEXT})
}
trap cleanup_mrustc EXIT
rm -rf ${WORKDIR}build/rustc-${RUSTC_VERSION_NEXT}-src/build
(cd ${WORKDIR}build/rustc-${RUSTC_VERSION_NEXT}-src/ && LD_LIBRARY_PATH=${PREFIX}lib/rustlib/${RUSTC_TARGET}/lib ./x.py build --stage 3) > ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}.log 2>&1
cleanup_mrustc
trap - EXIT
rm -rf ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}-output
rm -rf ${WORKDIR}output
cp -r ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}/rustc-${RUSTC_VERSION_NEXT}-src/build/${RUSTC_TARGET}/stage3 ${WORKDIR}output
cp ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}/rustc-${RUSTC_VERSION_NEXT}-src/build/${RUSTC_TARGET}/stage3-tools-bin/* ${WORKDIR}output/bin/
rm -rf ${WORKDIR}output/lib/rustlib/src ${WORKDIR}output/lib/rustlib/rustc-src
tar --mtime="@0" --sort=name -czf ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}.tar.gz -C ${WORKDIR} output
mv ${WORKDIR}output ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}-output

#
# Build rustc by downloading the previous version of rustc (and its matching cargo)
#
echo "=== Building rustc ${RUSTC_VERSION_NEXT} bootstrap downloaded stage0"
mkdir -p ${WORKDIR}official-${RUSTC_VERSION_NEXT}/
tar -xzf rustc-${RUSTC_VERSION_NEXT}-src.tar.gz -C ${WORKDIR}official-${RUSTC_VERSION_NEXT}/
cat - > ${WORKDIR}official-${RUSTC_VERSION_NEXT}/rustc-${RUSTC_VERSION_NEXT}-src/config.toml <<EOF
[build]
full-bootstrap = true
vendor = true
extended = true
[llvm]
ninja = false
download-ci-llvm = false
EOF
echo "--- Running x.py, see ${WORKDIR}official-${RUSTC_VERSION_NEXT}.log for progress"
(cd ${WORKDIR} && mv official-${RUSTC_VERSION_NEXT} build)
(cd ${WORKDIR}build/rustc-${RUSTC_VERSION_NEXT}-src/ && ./x.py build --stage 3) > ${WORKDIR}official-${RUSTC_VERSION_NEXT}.log 2>&1
(cd ${WORKDIR} && mv build official-${RUSTC_VERSION_NEXT})
rm -rf ${WORKDIR}official-${RUSTC_VERSION_NEXT}-output
rm -rf ${WORKDIR}output
cp -r ${WORKDIR}official-${RUSTC_VERSION_NEXT}/rustc-${RUSTC_VERSION_NEXT}-src/build/${RUSTC_TARGET}/stage3 ${WORKDIR}output
cp ${WORKDIR}official-${RUSTC_VERSION_NEXT}/rustc-${RUSTC_VERSION_NEXT}-src/build/${RUSTC_TARGET}/stage3-tools-bin/* ${WORKDIR}output/bin/
rm -rf ${WORKDIR}output/lib/rustlib/src ${WORKDIR}output/lib/rustlib/rustc-src
tar --mtime="@0" --sort=name -czf ${WORKDIR}official-${RUSTC_VERSION_NEXT}.tar.gz -C ${WORKDIR} output
mv ${WORKDIR}output ${WORKDIR}official-${RUSTC_VERSION_NEXT}-output
ln -s ${PWD}/${WORKDIR}official-${RUSTC_VERSION_NEXT}-output ${WORKDIR}chained-${RUSTC_VERSION_NEXT}-output

#
# Compare mrustc-built and official build artifacts
#
if diff -qs ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}.tar.gz ${WORKDIR}official-${RUSTC_VERSION_NEXT}.tar.gz; then
    echo "=== Convergence achieved in rustc version ${RUSTC_VERSION_NEXT}"
    CONVERGED=y
fi

for ARG in ${*}; do
    RUSTC_VERSION=${RUSTC_VERSION_NEXT}
    RUSTC_VERSION_NEXT=${ARG}

    if [ "${SAVE_SPACE:-n}" = y ]; then
        rm -rf ${WORKDIR}mrustc-${RUSTC_VERSION}
        rm -rf ${WORKDIR}chained-${RUSTC_VERSION} || true
        rm -rf ${WORKDIR}official-${RUSTC_VERSION}
    fi

    if [ ! -e rustc-${RUSTC_VERSION_NEXT}-src.tar.gz ]; then
        wget https://static.rust-lang.org/dist/rustc-${RUSTC_VERSION_NEXT}-src.tar.gz
    fi

    #
    # Build rustc using entirely mrustc-built tools
    #
    PREFIX=${PWD}/${WORKDIR}mrustc-${RUSTC_VERSION}-output/
    echo "=== Building rustc ${RUSTC_VERSION_NEXT} bootstrap mrustc stage0"
    mkdir -p ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}/
    tar -xzf rustc-${RUSTC_VERSION_NEXT}-src.tar.gz -C ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}/
    cat - > ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}/rustc-${RUSTC_VERSION_NEXT}-src/config.toml <<EOF
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
    echo "--- Running x.py, see ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}.log for progress"
    (cd ${WORKDIR} && mv mrustc-${RUSTC_VERSION_NEXT} build)
    cleanup_mrustc() {
        (cd ${WORKDIR} && mv build mrustc-${RUSTC_VERSION_NEXT})
    }
    trap cleanup_mrustc EXIT
    rm -rf ${WORKDIR}build/rustc-${RUSTC_VERSION_NEXT}-src/build
    (cd ${WORKDIR}build/rustc-${RUSTC_VERSION_NEXT}-src/ && LD_LIBRARY_PATH=${PREFIX}lib/rustlib/${RUSTC_TARGET}/lib ./x.py build --stage 3) > ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}.log 2>&1
    cleanup_mrustc
    trap - EXIT
    rm -rf ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}-output
    rm -rf ${WORKDIR}output
    cp -r ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}/rustc-${RUSTC_VERSION_NEXT}-src/build/${RUSTC_TARGET}/stage3 ${WORKDIR}output
    cp ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}/rustc-${RUSTC_VERSION_NEXT}-src/build/${RUSTC_TARGET}/stage3-tools-bin/* ${WORKDIR}output/bin/
    rm -rf ${WORKDIR}output/lib/rustlib/src ${WORKDIR}output/lib/rustlib/rustc-src
    tar --mtime="@0" --sort=name -czf ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}.tar.gz -C ${WORKDIR} output
    mv ${WORKDIR}output ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}-output

    if [ "${CONVERGED}" = "n" ]; then
        #
        # Build rustc by chaining from initial downloaded version of rustc (and its matching cargo)
        #
        PREFIX=${PWD}/${WORKDIR}chained-${RUSTC_VERSION}-output/
        echo "=== Building rustc ${RUSTC_VERSION_NEXT} bootstrap chained stage0"
        mkdir -p ${WORKDIR}chained-${RUSTC_VERSION_NEXT}/
        tar -xzf rustc-${RUSTC_VERSION_NEXT}-src.tar.gz -C ${WORKDIR}chained-${RUSTC_VERSION_NEXT}/
        cat - > ${WORKDIR}chained-${RUSTC_VERSION_NEXT}/rustc-${RUSTC_VERSION_NEXT}-src/config.toml <<EOF
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
        echo "--- Running x.py, see ${WORKDIR}chained-${RUSTC_VERSION_NEXT}.log for progress"
        (cd ${WORKDIR} && mv chained-${RUSTC_VERSION_NEXT} build)
        (cd ${WORKDIR}build/rustc-${RUSTC_VERSION_NEXT}-src/ && LD_LIBRARY_PATH=${PREFIX}lib/rustlib/${RUSTC_TARGET}/lib ./x.py build --stage 3) > ${WORKDIR}chained-${RUSTC_VERSION_NEXT}.log 2>&1
        (cd ${WORKDIR} && mv build chained-${RUSTC_VERSION_NEXT})
        rm -rf ${WORKDIR}chained-${RUSTC_VERSION_NEXT}-output
        rm -rf ${WORKDIR}output
        cp -r ${WORKDIR}chained-${RUSTC_VERSION_NEXT}/rustc-${RUSTC_VERSION_NEXT}-src/build/${RUSTC_TARGET}/stage3 ${WORKDIR}output
        cp ${WORKDIR}chained-${RUSTC_VERSION_NEXT}/rustc-${RUSTC_VERSION_NEXT}-src/build/${RUSTC_TARGET}/stage3-tools-bin/* ${WORKDIR}output/bin/
        rm -rf ${WORKDIR}output/lib/rustlib/src ${WORKDIR}output/lib/rustlib/rustc-src
        tar --mtime="@0" --sort=name -czf ${WORKDIR}chained-${RUSTC_VERSION_NEXT}.tar.gz -C ${WORKDIR} output
        mv ${WORKDIR}output ${WORKDIR}chained-${RUSTC_VERSION_NEXT}-output

        #
        # Build rustc by downloading the previous version of rustc (and its matching cargo)
        #
        PREFIX=${PWD}/${WORKDIR}official-${RUSTC_VERSION}-output/
        echo "=== Building rustc ${RUSTC_VERSION_NEXT} bootstrap downloaded stage0"
        mkdir -p ${WORKDIR}official-${RUSTC_VERSION_NEXT}/
        tar -xzf rustc-${RUSTC_VERSION_NEXT}-src.tar.gz -C ${WORKDIR}official-${RUSTC_VERSION_NEXT}/
        cat - > ${WORKDIR}official-${RUSTC_VERSION_NEXT}/rustc-${RUSTC_VERSION_NEXT}-src/config.toml <<EOF
[build]
full-bootstrap = true
vendor = true
extended = true
[llvm]
ninja = false
download-ci-llvm = false
EOF
        echo "--- Running x.py, see ${WORKDIR}official.log for progress"
        (cd ${WORKDIR} && mv official-${RUSTC_VERSION_NEXT} build)
        (cd ${WORKDIR}build/rustc-${RUSTC_VERSION_NEXT}-src/ && ./x.py build --stage 3) > ${WORKDIR}official-${RUSTC_VERSION_NEXT}.log 2>&1
        (cd ${WORKDIR} && mv build official-${RUSTC_VERSION_NEXT})
        rm -rf ${WORKDIR}official-${RUSTC_VERSION_NEXT}-output
        rm -rf ${WORKDIR}output
        cp -r ${WORKDIR}official-${RUSTC_VERSION_NEXT}/rustc-${RUSTC_VERSION_NEXT}-src/build/${RUSTC_TARGET}/stage3 ${WORKDIR}output
        cp ${WORKDIR}official-${RUSTC_VERSION_NEXT}/rustc-${RUSTC_VERSION_NEXT}-src/build/${RUSTC_TARGET}/stage3-tools-bin/* ${WORKDIR}output/bin/
        rm -rf ${WORKDIR}output/lib/rustlib/src ${WORKDIR}output/lib/rustlib/rustc-src
        tar --mtime="@0" --sort=name -czf ${WORKDIR}official-${RUSTC_VERSION_NEXT}.tar.gz -C ${WORKDIR} output
        mv ${WORKDIR}output ${WORKDIR}official-${RUSTC_VERSION_NEXT}-output

        #
        # Compare mrustc-built and official build artifacts
        #
        diff -qs ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}.tar.gz ${WORKDIR}official-${RUSTC_VERSION_NEXT}.tar.gz && CONVERGEDMO=y || CONVERGEDMO=n
        diff -qs ${WORKDIR}mrustc-${RUSTC_VERSION_NEXT}.tar.gz ${WORKDIR}chained-${RUSTC_VERSION_NEXT}.tar.gz && CONVERGEDMC=y || CONVERGEDMC=n
        diff -qs ${WORKDIR}chained-${RUSTC_VERSION_NEXT}.tar.gz ${WORKDIR}official-${RUSTC_VERSION_NEXT}.tar.gz && CONVERGEDCO=y || CONVERGEDCO=n

        if [ "${CONVERGEDMO}" = "y" ] && [ "${CONVERGEDMC}" = "y" ] && [ "${CONVERGEDCO}" = "y" ]; then
            echo "=== Convergence achieved in rustc version ${RUSTC_VERSION_NEXT}"
            CONVERGED=y
        fi
    fi
done

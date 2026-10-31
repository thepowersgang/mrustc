# Solaris 10 (sparcv9-sun-solaris)

- Sysroot: `python3 Notes/solaris_sysroot.py <iso> <sysroot> [7z]` extracts it from the Solaris 10 install DVD, using 7-Zip. Run it without arguments to see which DVD it expects.
- Compiler: binutils and GCC 9.5 (the last GCC for Solaris 10, needs `--enable-obsolete`), configured with `--target=sparcv9-sun-solaris2.10 --with-sysroot=<sysroot>`.
- Std: with `CC_sparcv9_sun_solaris` set to that gcc, `make -f minicargo.mk LIBS RUSTC_VERSION=1.74.0 MRUSTC_TARGET=sparcv9-sun-solaris OVERRIDE_SUFFIX=-solaris RUSTC_ARCH=sparc64`.

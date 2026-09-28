# Building a Solaris sysroot

`sparcv9-sun-solaris` (Solaris 10) and `sparcv9-sun-solaris2.9` (Solaris 9) are cross
targets, so building for them needs the target's headers and libraries on the build
host. That sysroot is the one piece you have to make yourself. Solaris 9 predates
OpenSolaris by three years, so no free substitute exists for it; whether a copy may be
redistributed depends on the licence your install came under, so check before
publishing one.

There are two sources: a **disk image** of an installed system, or the **install media**.
Prefer the disk image -- it is simpler, version-agnostic, and needs no Solaris machine.

---

## Method 1: from a disk image (Solaris 9 or 10)

An installed filesystem already has the development symlinks and needs no package
selection, so this is three directory copies. `rb-cli` reads the Sun disk label and UFS
directly and runs on Windows, macOS and Linux, so no Solaris host and no Unix-only
tooling is involved.

    rb-cli inspect solaris.img              # find the root slice; usually 1
    rb-cli get solaris.img@1 /usr/include  sysroot/usr/include -r
    rb-cli get solaris.img@1 /usr/lib      sysroot/usr/lib     -r
    rb-cli get solaris.img@1 /usr/ccs/lib  sysroot/usr/ccs/lib -r
    rb-cli get solaris.img@1 /lib          sysroot/lib         -r   # Solaris 10
    tar czf sysroot.tar.gz -C sysroot usr lib

**Take `/lib` as well, and mind the difference between releases.** On Solaris 10 the
core libraries live in `/lib` and `/usr/lib` holds symlinks pointing back at them --
`/usr/lib/sparcv9/libc.so.1` is a 30-byte link while `/lib/sparcv9/libc.so.1` is the
real 1.8 MB library. On Solaris 9 it is the other way round: `/usr/lib` has the
libraries and `/lib` is itself a symlink to `usr/lib`, so the copy above finds nothing
and you should create the link yourself:

    [ -d sysroot/lib ] || ln -sfn usr/lib sysroot/lib

Creating that link unconditionally would replace Solaris 10's real libraries with a
dangling one, and the failure appears much later as a link error.

`rusty-backup/docker/sol9-cross/mksysroot.sh disk solaris.img` handles both.

Copy the image to local disk first. Reading one over SMB or NFS is slow enough to look
like a hang -- an `inspect` of a DVD over SMB timed out at five minutes here.

---

## Method 2: from install media

Solaris media stores the OS as SVR4 packages under `Solaris_<ver>/Product/`, each with
its payload in `archive/none.*` -- a compressed cpio archive. **The compression differs
between releases, which is the main reason the two sections below are not identical.**

Nine packages are enough for a C toolchain:

    SUNWhea SUNWlibm SUNWcsl SUNWcslx SUNWlibms SUNWlmsx SUNWarc SUNWarcx SUNWtoo

`SUNWlibm` carries the math *headers* and `SUNWlibms` the math *libraries* -- take both.
Omitting `SUNWlibm` costs you `floatingpoint.h` and fails inside `math.h`, a long way
from the cause.

Whichever release you are on, **replay the symlinks afterwards**. SVR4 keeps them in the
`pkgmap`, not in the cpio payload -- `pkgadd` creates them on install. Skip this and the
development symlinks (`libm.so -> libm.so.1`) are absent, so every `-l` fails with
"cannot find -lm" while the `.so.1` sits right there. There are 335 of them on Solaris 9.

    python3 - <<'PY'
    import os, glob
    for pm in glob.glob("pkgs/*/pkgmap"):
        for line in open(pm, errors="replace"):
            f = line.split()
            if len(f) >= 4 and f[1] == "s" and "=" in f[3]:
                path, target = f[3].split("=", 1)
                dst = os.path.join("root", path)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if not os.path.lexists(dst):
                    os.symlink(target, dst)
    PY

### Solaris 9 media

Payloads are **bzip2** (`archive/none.bz2`), so `bunzip2` and `cpio` are enough:

    for p in SUNWhea SUNWlibm SUNWcsl SUNWcslx SUNWlibms SUNWlmsx SUNWarc SUNWarcx SUNWtoo; do
        xorriso -osirrox on -indev sol-9-sparc-dvd.iso \
            -extract "/Solaris_9/Product/$p" "pkgs/$p" >/dev/null 2>&1
        bunzip2 -c "pkgs/$p/archive/none.bz2" | ( mkdir -p root && cd root && cpio -idmu --quiet )
    done

Then replay the symlinks, `ln -sfn usr/lib root/lib`, and tar it up. About 13 MB, against
~109 MB pulled off a live install: nine packages rather than a whole running `/usr`.

`mksysroot.sh media9 sol-9-sparc-dvd.iso` automates this.

Note the media gives the GA libc (`112874-34`) where a patched install gives whatever it
has (`112874-47`). For a *build* sysroot the GA one is the better choice: a binary linked
against it needs only symbols from the original release, so it runs on patched and
unpatched systems alike.

### Solaris 10 media

Two differences from Solaris 9, both of which break a Solaris 9 recipe:

* Payloads are **7-Zip** (`archive/none.7z`), not bzip2. Use `7z`, or `py7zr` from
  Python. Each archive holds a single member named `none`, which is the cpio stream.
* The package layout moved: the 64-bit libraries are merged into the base packages
  rather than separate `*x` ones, and core libraries live under `/lib` rather than
  `/usr/lib`, so the package that owns `libc.so.1` is not the one it is on 9.

Because of that second point, **Method 1 is strongly preferred for Solaris 10** -- a disk
image sidesteps the package-layout question entirely.

---

## Building the cross toolchain

Unpack the sysroot first. The `chmod` matters: the tarball carries Solaris' original
modes, some of which your user cannot read.

    mkdir -p /opt/sol/sysroot && tar xzf sysroot.tar.gz -C /opt/sol/sysroot
    ln -sfn usr/lib /opt/sol/sysroot/lib
    chmod -R u+rwX /opt/sol/sysroot

### Solaris 9: GCC 4.9.x, and only 4.9.x

GCC obsoleted the target in 4.9 and deleted it in 5, while mrustc's output needs
`<stdatomic.h>` (4.9). The `__builtin_*_overflow` intrinsics that arrived in GCC 5 are
supplied instead by the target's `emulate-overflow-intrinsics` flag.

    curl -fsSLO https://ftp.gnu.org/gnu/binutils/binutils-2.35.2.tar.xz
    tar xJf binutils-2.35.2.tar.xz && mkdir -p build/binutils && cd build/binutils
    ../../binutils-2.35.2/configure --target=sparcv9-sun-solaris2.9 \
        --prefix=/opt/sol --with-sysroot=/opt/sol/sysroot --disable-nls --disable-werror
    make -j"$(nproc)" && make install-strip && cd ../..

    export PATH=/opt/sol/bin:$PATH

    curl -fsSLO https://ftp.gnu.org/gnu/gcc/gcc-4.9.4/gcc-4.9.4.tar.bz2
    tar xjf gcc-4.9.4.tar.bz2 && mkdir -p build/gcc && cd build/gcc
    ../../gcc-4.9.4/configure --target=sparcv9-sun-solaris2.9 \
        --prefix=/opt/sol --with-sysroot=/opt/sol/sysroot \
        --with-build-sysroot=/opt/sol/sysroot \
        --enable-languages=c --enable-obsolete --disable-nls \
        --disable-libssp --disable-libgomp --disable-libquadmath \
        CFLAGS="-O2 -w" CXXFLAGS="-O2 -w -std=gnu++98 -fpermissive"
    make -j"$(nproc)" && make install-strip

`--enable-obsolete` is required; 4.9 already considered the target obsolete. The
`CXXFLAGS` are what let a current compiler build 2016 source -- GCC 4.9's own code
predates C++11 and a modern GCC defaults to C++17 and rejects it. Verified on Ubuntu
24.04 with GCC 13.

Then fix Solaris 9's empty `INTPTR_MAX`. Its `<sys/int_limits.h>` defines `INTPTR_MAX`
and `UINTPTR_MAX` as *empty* macros -- pre-C99 they were existence flags rather than
values -- and GCC copies the header into `include-fixed` without repairing it, so any
C99 `#if UINTPTR_MAX == ...` fails with "operator '==' has no left operand". Skip this
and the failure surfaces much later, in whatever dependency tests those macros.

    H=/opt/sol/lib/gcc/sparcv9-sun-solaris2.9/4.9.4/include-fixed/sys/int_limits.h
    sed -i 's/^#define[[:space:]]*INTPTR_MAX[[:space:]]*$/#define INTPTR_MAX __INTPTR_MAX__/;
            s/^#define[[:space:]]*UINTPTR_MAX[[:space:]]*$/#define UINTPTR_MAX __UINTPTR_MAX__/' "$H"

GCC's own `__INTPTR_MAX__` is per-multilib, so one substitution is right for both the
32- and 64-bit builds.

### Solaris 10: no version ceiling, no fixups

Solaris 10 kept upstream GCC support far longer, so use a much newer GCC and drop
`--enable-obsolete`, the `CXXFLAGS` workarounds and the `INTPTR_MAX` patch -- none of
them apply. Build as above with `--target=sparcv9-sun-solaris`. The target definition in
`src/trans/target.cpp` needs none of the three emulation flags Solaris 9 requires.

---

## Check it works

    echo 'int main(void){return 0;}' > /tmp/t.c
    /opt/sol/bin/sparcv9-sun-solaris2.9-gcc -m64 -mcpu=v9 /tmp/t.c -o /tmp/t
    file /tmp/t        # ELF 64-bit MSB executable, SPARC V9

Then point mrustc at it:

    export CC_sparcv9_sun_solaris2_9=/opt/sol/bin/sparcv9-sun-solaris2.9-gcc
    make -f minicargo.mk LIBS RUSTC_VERSION=1.74.0 \
        MRUSTC_TARGET=sparcv9-sun-solaris2.9 \
        OVERRIDE_SUFFIX=-solaris STD_ENV_ARCH=sparc64

`OVERRIDE_SUFFIX` and `STD_ENV_ARCH` must both be given: the first is otherwise picked
from the *host* OS, and the second decides what `std::env::consts::ARCH` reports
(`sparc64`, which is not the triple's leading component).

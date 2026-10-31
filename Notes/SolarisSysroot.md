# Building a Solaris sysroot

`sparcv9-sun-solaris` (Solaris 10) is a cross target, so building for it needs the
target's headers and libraries on the build host. That sysroot is the one piece you have
to make yourself. Whether a copy may be redistributed depends on the licence your install
came under, so check before publishing one.

There are two sources: a **disk image** of an installed system, or the **install media**.
Prefer the disk image -- it is simpler and needs no Solaris machine.

---

## Method 1: from a disk image

An installed filesystem already has the development symlinks and needs no package
selection, so this is four directory copies. `rb-cli` reads the Sun disk label and UFS
directly and runs on Windows, macOS and Linux, so no Solaris host and no Unix-only
tooling is involved.

    rb-cli inspect solaris.img              # find the root slice; usually 1
    rb-cli get solaris.img@1 /usr/include  sysroot/usr/include -r
    rb-cli get solaris.img@1 /usr/lib      sysroot/usr/lib     -r
    rb-cli get solaris.img@1 /usr/ccs/lib  sysroot/usr/ccs/lib -r
    rb-cli get solaris.img@1 /lib          sysroot/lib         -r
    tar czf sysroot.tar.gz -C sysroot usr lib

**Take `/lib` as well.** On Solaris 10 the core libraries live in `/lib` and `/usr/lib`
holds symlinks pointing back at them -- `/usr/lib/sparcv9/libc.so.1` is a 30-byte link
while `/lib/sparcv9/libc.so.1` is the real 1.8 MB library. Without `/lib` the failure
appears much later, as a link error.

Copy the image to local disk first. Reading one over SMB or NFS is slow enough to look
like a hang -- an `inspect` of a DVD over SMB timed out at five minutes here.

---

## Method 2: from install media

Solaris media stores the OS as SVR4 packages under `Solaris_10/Product/`, each with its
payload in `archive/none.7z` -- a **7-Zip** archive holding a single member named `none`,
which is a cpio stream. Use `7z`, or `py7zr` from Python.

The packages to take are the headers, libc, libm (headers *and* libraries -- omitting the
headers costs you `floatingpoint.h` and fails inside `math.h`, a long way from the cause)
and the archive/linker support. The 64-bit libraries are merged into the base packages,
and core libraries live under `/lib` rather than `/usr/lib`, so check which package owns
`libc.so.1` on your media before relying on a list.

**Replay the symlinks afterwards.** SVR4 keeps them in the `pkgmap`, not in the cpio
payload -- `pkgadd` creates them on install. Skip this and the development symlinks
(`libm.so -> libm.so.1`) are absent, so every `-l` fails with "cannot find -lm" while the
`.so.1` sits right there.

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

Because of the package-layout question, **Method 1 is strongly preferred**.

---

## Building the cross toolchain

Unpack the sysroot first. The `chmod` matters: the tarball carries Solaris' original
modes, some of which your user cannot read.

    mkdir -p /opt/sol/sysroot && tar xzf sysroot.tar.gz -C /opt/sol/sysroot
    chmod -R u+rwX /opt/sol/sysroot

Then build binutils and a GCC that still supports Solaris 10, both with
`--target=sparcv9-sun-solaris --prefix=/opt/sol --with-sysroot=/opt/sol/sysroot`
(GCC also wants `--with-build-sysroot=/opt/sol/sysroot --enable-languages=c`). mrustc's
output needs `<stdatomic.h>` and `__builtin_*_overflow`, so GCC 5 or later.

---

## Check it works

    echo 'int main(void){return 0;}' > /tmp/t.c
    /opt/sol/bin/sparcv9-sun-solaris-gcc -m64 -mcpu=v9 /tmp/t.c -o /tmp/t
    file /tmp/t        # ELF 64-bit MSB executable, SPARC V9

Then point mrustc at it:

    export CC_sparcv9_sun_solaris=/opt/sol/bin/sparcv9-sun-solaris-gcc
    make -f minicargo.mk LIBS RUSTC_VERSION=1.74.0 \
        MRUSTC_TARGET=sparcv9-sun-solaris \
        OVERRIDE_SUFFIX=-solaris STD_ENV_ARCH=sparc64

`OVERRIDE_SUFFIX` and `STD_ENV_ARCH` must both be given: the first is otherwise picked
from the *host* OS, and the second decides what `std::env::consts::ARCH` reports
(`sparc64`, which is not the triple's leading component).

import hashlib, os, shutil, subprocess, sys, tempfile
SHA256 = "2e8603520d87935a2e8c6d5934342e47af319627b2393ba46ef846770fdcaa69"
PKGS = ["SUNWhea", "SUNWlibm", "SUNWarc", "SUNWcsl", "SUNWcslr", "SUNWlibms", "SUNWlibmsr"]
if len(sys.argv) < 3:
    sys.exit(f"usage: {sys.argv[0]} <sol-10-u10-ga2-sparc-dvd.iso> <sysroot dir> [7z executable]")
iso, out, sz = sys.argv[1], os.path.abspath(sys.argv[2]), sys.argv[3] if len(sys.argv) > 3 else "7z"
with open(iso, "rb") as f:
    if hashlib.file_digest(f, "sha256").hexdigest() != SHA256:
        print(f"warning: {iso} does not have the expected SHA-256 {SHA256}", file=sys.stderr)
x = lambda *a: subprocess.run([sz, "x", "-y", *a], check=True, stdout=subprocess.DEVNULL)
links, td = [], tempfile.TemporaryDirectory()
x("-o" + td.name, iso, *[f"Solaris_10/Product/{p}/*" for p in PKGS])
for p in PKGS:
    d = os.path.join(td.name, "Solaris_10", "Product", p)
    if os.path.exists(os.path.join(d, "archive", "none.7z")):
        x("-o" + d, os.path.join(d, "archive", "none.7z"))
        x("-tcpio", "-o" + out, os.path.join(d, "none"))
    links += [l.split()[3].split("=", 1) for l in open(os.path.join(d, "pkgmap")) if l.split()[1:2] == ["s"]]
for path, target in links * 2:
    dst = os.path.join(out, *path.split("/"))
    src = os.path.normpath(os.path.join(os.path.dirname(dst), *target.split("/")))
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if os.path.lexists(dst) or not os.path.exists(src):
        continue
    try:
        os.symlink(target.replace("/", os.sep), dst, os.path.isdir(src))
    except OSError:
        (shutil.copytree if os.path.isdir(src) else shutil.copy2)(src, dst)

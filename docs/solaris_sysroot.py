#!/usr/bin/env python3
#Builds a Solaris 10 SPARC sysroot for mrustc from the Solaris 10 install DVD
#requirements: python 3.11+ (for hashlib.file_digest), 7-Zip
#
#How it works:
#1. Check the ISO is the one we expect (sol-10-u10-ga2-sparc-dvd.iso)
#2. Pull the SVR4 packages we need out of Solaris_10/Product on the DVD
#3. Each package has archive/none.7z, which unpacks to a cpio archive called "none"
#4. Unpack the cpio into the sysroot folder
#5. cpio doesn't bring the symlinks over, they are listed in each package's pkgmap file
#   so read pkgmap and create them ourselves
#
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile

expectedSHA256 = "2e8603520d87935a2e8c6d5934342e47af319627b2393ba46ef846770fdcaa69"

#Headers, libc, libm and the libraries they link against
packages = [
    "SUNWhea",     #system headers
    "SUNWlibm",    #math headers
    "SUNWarc",     #static link libraries
    "SUNWcsl",     #core libraries (/usr/lib)
    "SUNWcslr",    #core libraries (/lib)
    "SUNWlibms",   #math library (/usr/lib)
    "SUNWlibmsr",  #math library (/lib)
]

if len(sys.argv) < 3:
    print(f"USAGE: {os.path.basename(__file__)} <sol-10-u10-ga2-sparc-dvd.iso> <sysroot folder> [path to 7z]")
    sys.exit(1)

isoFile = sys.argv[1]
sysrootFolder = os.path.abspath(sys.argv[2])
if len(sys.argv) > 3:
    sevenZip = sys.argv[3]
else:
    sevenZip = "7z"


def run7z(args):
    #-y answers yes to any overwrite prompts, output is hidden since it lists every file
    command = [sevenZip, "x", "-y"] + args
    subprocess.run(command, check=True, stdout=subprocess.DEVNULL)


def checkISO(isoFile):
    print(f"Checking SHA-256 of {isoFile}...")
    with open(isoFile, "rb") as file:
        sha = hashlib.file_digest(file, "sha256").hexdigest()
    if sha != expectedSHA256:
        #Only a warning, a different update of the DVD might still work
        print(f"Warning: {isoFile} does not match the expected SHA-256 {expectedSHA256}", file=sys.stderr)


def extractPackages(isoFile, workFolder):
    packagePaths = []
    for package in packages:
        packagePaths.append(f"Solaris_10/Product/{package}/*")
    print(f"Extracting {len(packages)} packages from the DVD...")
    run7z(["-o" + workFolder, isoFile] + packagePaths)


def unpackPackage(packageFolder):
    archiveFile = os.path.join(packageFolder, "archive", "none.7z")
    if not os.path.exists(archiveFile):
        #Some packages only have symlinks and no files
        return
    #none.7z unpacks to a cpio archive called "none" in the same folder
    run7z(["-o" + packageFolder, archiveFile])
    cpioFile = os.path.join(packageFolder, "none")
    run7z(["-tcpio", "-o" + sysrootFolder, cpioFile])


def readSymlinks(packageFolder):
    #pkgmap symlink lines look like:
    #1 s none usr/lib/libc.so=./libc.so.1
    #part, type, class, path=target
    symlinks = []
    pkgmapFile = os.path.join(packageFolder, "pkgmap")
    with open(pkgmapFile, "r") as file:
        for line in file:
            fields = line.split()
            if len(fields) >= 4 and fields[1] == "s":
                path, target = fields[3].split("=", 1)
                symlinks.append((path, target))
    return symlinks


def createSymlink(path, target):
    #Returns True once the link exists (or was already there)
    linkFile = os.path.join(sysrootFolder, *path.split("/"))
    if os.path.lexists(linkFile):
        return True
    #target is relative to the folder the link lives in
    targetFile = os.path.normpath(os.path.join(os.path.dirname(linkFile), *target.split("/")))
    if not os.path.exists(targetFile):
        #Target might be another symlink we haven't made yet
        return False
    os.makedirs(os.path.dirname(linkFile), exist_ok=True)
    try:
        os.symlink(target.replace("/", os.sep), linkFile, os.path.isdir(targetFile))
    except OSError:
        #Windows without Developer Mode can't make symlinks, so copy instead
        if os.path.isdir(targetFile):
            shutil.copytree(targetFile, linkFile)
        else:
            shutil.copy2(targetFile, linkFile)
    return True


checkISO(isoFile)
workFolder = tempfile.TemporaryDirectory()
extractPackages(isoFile, workFolder.name)

allSymlinks = []
for package in packages:
    print(f"Unpacking {package}...")
    packageFolder = os.path.join(workFolder.name, "Solaris_10", "Product", package)
    unpackPackage(packageFolder)
    allSymlinks += readSymlinks(packageFolder)

#Links can point at other links, so keep going until a pass doesn't make any new ones
print(f"Creating {len(allSymlinks)} symlinks...")
remaining = allSymlinks
while len(remaining) > 0:
    stillMissing = []
    for path, target in remaining:
        if not createSymlink(path, target):
            stillMissing.append((path, target))
    if len(stillMissing) == len(remaining):
        break
    remaining = stillMissing

for path, target in remaining:
    print(f"Skipped {path} -> {target}, target is not in the sysroot")

workFolder.cleanup()
print(f"Done, sysroot is in {sysrootFolder}")

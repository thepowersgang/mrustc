{
  description = "Build mrustc and directly bootstrap supported rustc releases";

  inputs = {
    # Pinned dated stable release (not nixpkgs-unstable): the packages this flake needs
    # (cmake, openssl, libgit2, gcc) should stay put across `nix flake update` rather than
    # shifting under old-LLVM/old-openssl-sensitive builds. Override with
    # `--override-input nixpkgs github:NixOS/nixpkgs/nixpkgs-unstable` if desired.
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

    # A second, older nixpkgs snapshot used ONLY for the legacy rustc versions
    # (1.19.0/1.29.0/1.39.0), which bundle a ~2017-2019-era LLVM that does not reliably
    # compile with a modern (gcc 13+) compiler or configure with modern (cmake 4+) cmake.
    # nixos-24.05 still ships gcc9Stdenv/gcc10Stdenv and openssl_1_1.
    nixpkgs-legacy.url = "github:NixOS/nixpkgs/nixos-24.05";

  };

  outputs = { self, nixpkgs, nixpkgs-legacy }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      lib = nixpkgs.lib;
      forAllSystems = lib.genAttrs systems;
      latestVersion = "1.97.1";

      # Per-version metadata, encoding the confirmed differences between the supported
      # rustc versions at the *invocation* level (minicargo.mk itself already handles
      # source-layout differences internally via RUSTC_VERSION branches -- this table does
      # not duplicate that).
      versions = {
        "1.19.0" = {
          mrustcTargetVer = "1.19";
          outdirSuf = "-1.19.0";
          srcHash = "sha256-FSMfUFP7cq2CvpH1q/1qpgy3iYxQieTxrFkQpzEJDFE=";
          legacyToolchain = true;
          legacyOpenssl = true;
          libgit2PkgConfig = false;
          smokeSample = null;
          # build-1.19.0.sh itself never runs `make local_tests` for this version -- matched
          # here rather than second-guessed, since local_tests has at least one test
          # (samples/test/tait.rs) that crashes mrustc under MRUSTC_TARGET_VER=1.19/1.29
          # (empirically confirmed for 1.29.0, see below).
          runLocalTests = false;
          darwinSupported = true; # script-overrides/stable-1.19.0-macos exists
        };
        "1.29.0" = {
          mrustcTargetVer = "1.29";
          outdirSuf = "-1.29.0";
          srcHash = "sha256-pOs0/9R/dq/iq9gT85hRLVoZ7wCYnTcwYhfJyewvYek=";
          legacyToolchain = true;
          legacyOpenssl = true;
          libgit2PkgConfig = false;
          smokeSample = null;
          # Empirically confirmed: `make local_tests` crashes mrustc (SIGABRT) compiling
          # samples/test/tait.rs under MRUSTC_TARGET_VER=1.29 -- a real mrustc bug, not a Nix
          # issue. build-1.29.0.sh itself never calls local_tests either, for (presumably) the
          # same reason -- matched here rather than worked around.
          runLocalTests = false;
          darwinSupported = true; # script-overrides/stable-1.29.0-macos exists
        };
        "1.39.0" = {
          mrustcTargetVer = "1.39";
          outdirSuf = "-1.39.0";
          srcHash = "sha256-tKH2tqk5MfJwaRq6T8he7gMv7NqXPmucd0zQaFdgk1c=";
          legacyToolchain = true;
          legacyOpenssl = false; # uses --features vendored-openssl (minicargo.mk)
          libgit2PkgConfig = true;
          smokeSample = null;
          runLocalTests = true; # build-1.39.0.sh runs it
          darwinSupported = true; # script-overrides/stable-1.39.0-macos exists
        };
        "1.54.0" = {
          mrustcTargetVer = "1.54";
          outdirSuf = "-1.54.0";
          srcHash = "sha256-rIURYz6bWmWtAwoaLlvaqEH9/jEy8rqqUswE5xxsaXY=";
          # Empirically confirmed (not just README-inferred): 1.54.0's bundled LLVM fails to
          # compile against nixpkgs-26.05's default gcc 15 with e.g. "'uint32_t' has not been
          # declared" in llvm/lib/Target/X86/MCTargetDesc/X86MCTargetDesc.h -- a standard-library
          # implicit-include regression in modern GCC that old LLVM trees run into. gcc9Stdenv
          # (via nixpkgs-legacy) builds it cleanly.
          legacyToolchain = true;
          legacyOpenssl = false;
          libgit2PkgConfig = true;
          smokeSample = null;
          runLocalTests = true; # build-1.54.0.sh runs it
          darwinSupported = true; # script-overrides/stable-1.54.0-macos exists
        };
        "1.74.0" = {
          mrustcTargetVer = "1.74";
          outdirSuf = "-1.74.0";
          srcHash = "sha256-iCtYS8Mhxdz+d82qafJ3kGuTYlXveAj81cdJKSXPEEk=";
          legacyToolchain = false;
          legacyOpenssl = false;
          libgit2PkgConfig = true;
          smokeSample = "samples/no_core.rs";
          runLocalTests = true; # build-1.74.0.sh runs it
          darwinSupported = false; # no script-overrides/stable-1.74.0-macos in this repo
        };
        "1.90.0" = {
          mrustcTargetVer = "1.90";
          outdirSuf = "-1.90.0";
          srcHash = "sha256-eZqfnLpO1TUeBxBIvPa1VgdV2QCWSN7zOkB91JYfm34=";
          legacyToolchain = false;
          legacyOpenssl = false;
          libgit2PkgConfig = true;
          smokeSample = "samples/no_core-1_90.rs";
          runLocalTests = true; # build-1.90.0.sh runs it
          darwinSupported = false; # no script-overrides/stable-1.90.0-macos in this repo
        };
        "1.97.1" = {
          mrustcTargetVer = "1.97";
          outdirSuf = "-1.97.1";
          # rustc stable manifest dated 2026-07-16:
          # https://static.rust-lang.org/dist/2026-07-16/rustc-1.97.1-src.tar.gz.sha256
          srcHash = "sha256-YiwrQpxTy/3A3TpR0DVU6RzWPr7BkSwfVwlkDN/vGp0=";
          legacyToolchain = false;
          legacyOpenssl = false;
          libgit2PkgConfig = true;
          smokeSample = "samples/no_core-1_97.rs";
          runLocalTests = true;
          darwinSupported = false; # no script-overrides/stable-1.97.1-macos in this repo
        };
      };

      mkOutputsForSystem = system:
        let
          pkgs = import nixpkgs { inherit system; };
          # openssl_1_1 is marked insecure upstream (EOL 2023-09-11); explicitly allowed here
          # because rustc 1.19.0/1.29.0's own openssl-sys bindings predate OpenSSL 3 and don't
          # build against it (this is a real upstream constraint of those two rustc releases,
          # not a choice this flake is making lightly -- see README's own "outdated openssl
          # bindings" caveat).
          pkgsLegacy = import nixpkgs-legacy {
            inherit system;
            config.permittedInsecurePackages = [ "openssl-1.1.1w" ];
          };
          isDarwin = lib.hasSuffix "darwin" system;

          mkRustcSrc = { version, hash }:
            pkgs.fetchurl {
              url = "https://static.rust-lang.org/dist/rustc-${version}-src.tar.gz";
              inherit hash;
            };

          # Shared C++ tool build (mrustc/minicargo/testrunner), also usable standalone,
          # independent of any specific rustc-version bootstrap.
          mkMrustcTools = pkgsSet:
            pkgsSet.stdenv.mkDerivation {
              pname = "mrustc-tools";
              version = "unstable";
              src = self;
              nativeBuildInputs = [ pkgsSet.gnumake pkgsSet.gitMinimal ];
              buildInputs = [ pkgsSet.zlib ];
              dontConfigure = true;
              buildPhase = ''
                runHook preBuild
                export HOME="$TMPDIR"
                make all
                make -C tools/minicargo
                make -C tools/testrunner
                runHook postBuild
              '';
              installPhase = ''
                runHook preInstall
                mkdir -p $out/bin
                cp bin/mrustc bin/minicargo bin/testrunner $out/bin/
                runHook postInstall
              '';
              meta = {
                description = "mrustc: an alternative Rust compiler written in C++, plus its minicargo/testrunner tools";
                license = lib.licenses.mit;
                platforms = lib.platforms.unix;
              };
            };

          mrustcTools = mkMrustcTools pkgs;

          mkBootstrap = version: meta:
            let
              pkgsSet = if meta.legacyToolchain then pkgsLegacy else pkgs;
              stdenv = if meta.legacyToolchain then pkgsSet.gcc9Stdenv else pkgsSet.stdenv;
              openssl = if meta.legacyOpenssl then pkgsSet.openssl_1_1 else pkgsSet.openssl;
              rustcSrcTarball = mkRustcSrc {
                inherit version;
                hash = meta.srcHash;
              };
            in
            stdenv.mkDerivation {
              pname = "mrustc-bootstrap-${version}";
              inherit version;
              src = self;

              nativeBuildInputs = with pkgsSet; [
                gnumake
                patch
                cmake
                pkg-config
                python3
                perl
                gitMinimal
              ];
              buildInputs = [ pkgsSet.zlib pkgsSet.curl openssl pkgsSet.libgit2 ];

              env = {
                RUSTC_VERSION = version;
                MRUSTC_TARGET_VER = meta.mrustcTargetVer;
                OUTDIR_SUF = meta.outdirSuf;
                # README: higher PARLEVEL "can and will break at times" -- default
                # conservative; override via `.overrideAttrs (o: { env = o.env // { PARLEVEL = "N"; }; })`
                # for faster local iteration at your own risk.
                PARLEVEL = "1";
                LLVM_CMAKE_OPTS_EXTRA = ""; # override to "CMAKE_POLICY_VERSION_MINIMUM=3.5" if cmake rejects old LLVM's cmake_minimum_required
              };

              dontConfigure = true;
              # LLVM's own build and mrustc/minicargo's own job scheduling both key off
              # PARLEVEL above -- Nix's generic parallel-building hook doesn't apply here.
              enableParallelBuilding = false;

              buildPhase = ''
                runHook preBuild
                set -euo pipefail
                export HOME="$TMPDIR"

                make all
                make -C tools/minicargo
                make -C tools/testrunner

                cp ${rustcSrcTarball} ./rustc-${version}-src.tar.gz
                make RUSTCSRC

                make -f minicargo.mk LIBS
                make test
                ${lib.optionalString meta.runLocalTests ''
                  make local_tests
                ''}

                RUSTC_INSTALL_BINDIR=bin make -f minicargo.mk output${meta.outdirSuf}/rustc
                ./output${meta.outdirSuf}/rustc --version
                ${lib.optionalString (meta.smokeSample != null) ''
                  ./output${meta.outdirSuf}/rustc ${meta.smokeSample}
                ''}

                LIBGIT2_SYS_USE_PKG_CONFIG=${if meta.libgit2PkgConfig then "1" else "0"} \
                  make -f minicargo.mk output${meta.outdirSuf}/cargo
                ./output${meta.outdirSuf}/cargo --version

                # Real functional check: rebuild libstd *with* the bootstrapped rustc (matching
                # symbol hashes -- mrustc-built rustc and mrustc-built libstd use different,
                # incompatible symbol hashes, see Notes/Bootstrapping.md) and run a std-linked
                # "Hello, world!" through it. This is run_rustc/Makefile's Stage 1, i.e.
                # README's documented "Getting Started" step 3 -- not an invented shortcut.
                make -C run_rustc RUSTC_VERSION=${version} PARLEVEL=1 output${meta.outdirSuf}/prefix-s/bin/hello_world
                # NOTE: deliberately not named `out` -- that would shadow Nix's own $out
                # (the derivation's output store path), which installPhase below relies on.
                helloWorldOutput="$(./run_rustc/output${meta.outdirSuf}/prefix-s/bin/hello_world)"
                test "$helloWorldOutput" = "Hello, world!"

                runHook postBuild
              '';

              installPhase = ''
                runHook preInstall
                mkdir -p $out/output${meta.outdirSuf}
                # Deliberately minimal: excludes rustc-build/, cargo-build/, rust_tests/,
                # stdtest/, local_tests/, rust/ (logs), and rustc-${version}-src/ (contains the
                # multi-GB LLVM build tree) -- none needed at runtime, all would bloat the
                # store across all supported versions.
                shopt -s nullglob
                for f in output${meta.outdirSuf}/*.rlib output${meta.outdirSuf}/*.hir; do
                  cp "$f" "$out/output${meta.outdirSuf}/"
                done
                cp output${meta.outdirSuf}/rustc output${meta.outdirSuf}/cargo "$out/output${meta.outdirSuf}/"
                runHook postInstall
              '';

              meta = {
                description = "mrustc-bootstrapped rustc ${version} + cargo (stage0-equivalent, not a ready-to-use toolchain -- see run_rustc/Makefile for the next self-hosting stage)";
                license = lib.licenses.mit;
                platforms = if meta.darwinSupported then lib.platforms.unix else lib.platforms.linux;
              };
            };

          versionPkgsAll = builtins.mapAttrs mkBootstrap versions;
          versionPkgs =
            if isDarwin
            then lib.filterAttrs (n: _: versions.${n}.darwinSupported) versionPkgsAll
            else versionPkgsAll;

          directVersionPkgs = versionPkgs;
          defaultPkg =
            directVersionPkgs.${latestVersion} or (builtins.head (builtins.attrValues directVersionPkgs));

          # ── Chained rustc bootstrap ─────────────────────────────────────────
          # rustc N is only buildable by rustc N-1 (stage0 policy), so the path
          # from mrustc's 1.90.0 ceiling to a modern rustc is a CHAIN of full
          # toolchain builds. chain-versions.nix is the registry; each stage is
          # its own derivation, so the nix store is the durable checkpoint: a
          # failed stage never rebuilds its predecessors, and re-running after a
          # fix resumes at the frontier for free.
          #
          # x86_64-linux only for now: run_rustc/Makefile defaults RUSTC_TARGET
          # to x86_64-unknown-linux-gnu and no other host has been exercised.
          chainRegistry = import ./chain-versions.nix;
          chainTip = lib.last chainRegistry.chain;
          chainSupported = system == "x86_64-linux";
          rustcTarget = "x86_64-unknown-linux-gnu";
          chainRootMeta = versions.${chainRegistry.root};
          chainRootSrcTarball = mkRustcSrc {
            version = chainRegistry.root;
            hash = chainRootMeta.srcHash;
          };
          chainRuntimeInputs = with pkgs; [
            stdenv.cc.cc.lib
            zlib
            openssl
            curl
            libgit2
            zstd
          ];

          # Full mrustc -> rustc ${root} toolchain: run_rustc's complete `all`
          # (Stage 1 sanity, Stage 2 std, Stage 3 rustc + cargo + matching-ABI
          # std), installed as a RELOCATABLE prefix usable as the x.py stage0
          # of the first chained build. The mkBootstrap packages above stop at
          # the stage0-equivalent binaries and DISCARD this prefix; here it is
          # the entire point. The prefix's `bin/rustc` wrapper and cargo_home
          # bake absolute build paths, so install regenerates the wrapper
          # against $out and drops cargo_home/tmp (chained builds write their
          # own config.toml against the source tarball's vendor dir).
          # Keep the root toolchain source stable when registry metadata, chain
          # patches, notes, or packaging change: none affect the mrustc 1.90.0
          # source build. Without this filter each such change would shift
          # `self`, invalidate the multi-hour stage0 derivation, and force the
          # entire chain to rebuild.
          chainToolchainSrc = lib.cleanSourceWith {
            name = "mrustc-chain-toolchain-src";
            src = self;
            filter =
              path: _type:
              let
                rel = lib.removePrefix (toString self + "/") (toString path);
                top = builtins.head (lib.splitString "/" rel);
              in
              !(builtins.elem top [
                "chain-patches"
                "chain-versions.nix"
                "Notes"
                "README.md"
                "ReleaseNotes.md"
                "flake.lock"
                "flake.nix"
                ".gitignore"
              ]);
          };

          stage0Toolchain =
            let
              version = chainRegistry.root;
              vmeta = chainRootMeta;
            in
            pkgs.stdenv.mkDerivation {
              pname = "rustc-${version}-toolchain";
              inherit version;
              src = chainToolchainSrc;

              nativeBuildInputs = with pkgs; [
                gnumake
                patch
                cmake
                pkg-config
                python3
                perl
                gitMinimal
              ];
              buildInputs = [ pkgs.zlib pkgs.curl pkgs.openssl pkgs.libgit2 ];

              env = {
                RUSTC_VERSION = version;
                MRUSTC_TARGET_VER = vmeta.mrustcTargetVer;
                OUTDIR_SUF = vmeta.outdirSuf;
                # PARLEVEL=1 for the mrustc/minicargo half (README: higher "can
                # and will break at times"); the cargo-driven run_rustc half
                # gets real parallelism via its own PARLEVEL override below.
                PARLEVEL = "1";
                LLVM_CMAKE_OPTS_EXTRA = "";
              };

              dontConfigure = true;
              enableParallelBuilding = false;

              buildPhase = ''
                runHook preBuild
                set -euo pipefail
                export HOME="$TMPDIR"

                make all
                make -C tools/minicargo

                cp ${chainRootSrcTarball} ./rustc-${version}-src.tar.gz
                make RUSTCSRC

                make -f minicargo.mk LIBS
                RUSTC_INSTALL_BINDIR=bin make -f minicargo.mk output${vmeta.outdirSuf}/rustc
                LIBGIT2_SYS_USE_PKG_CONFIG=${if vmeta.libgit2PkgConfig then "1" else "0"} \
                  make -f minicargo.mk output${vmeta.outdirSuf}/cargo

                # Full self-hosting ladder: Stage 1 (mrustc-built std sanity),
                # Stage 2 (std via cargo), Stage 3 (rustc + cargo + final std
                # with matching symbol hashes). Cargo-driven, so it can take
                # real parallelism.
                make -C run_rustc RUSTC_VERSION=${version} RUSTC_TARGET=${rustcTarget} \
                  PARLEVEL=''${NIX_BUILD_CORES:-8} all

                runHook postBuild
              '';

              installPhase = ''
                runHook preInstall
                mkdir -p $out
                cp -r run_rustc/output${vmeta.outdirSuf}/prefix/. $out/
                # cargo_home/config points at the (deleted) build tree's vendor
                # dir; tmp is scratch. Neither survives relocation meaningfully.
                rm -rf $out/cargo_home $out/tmp
                # The generated bin/rustc wrapper bakes absolute build paths in
                # LD_LIBRARY_PATH; regenerate it against $out.
                rm -f $out/bin/rustc
                cat > $out/bin/rustc <<WRAPPER
                #!/bin/sh
                d="\$(cd "\$(dirname "\$0")" && pwd)"
                LD_LIBRARY_PATH="$out/lib:$out/lib/rustlib/${rustcTarget}/lib\''${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}" exec "\$d/rustc_binary" "\$@"
                WRAPPER
                chmod +x $out/bin/rustc
                $out/bin/rustc --version
                runHook postInstall
              '';

              meta = {
                description = "Complete mrustc-bootstrapped rustc ${version} toolchain (relocatable run_rustc prefix; x.py stage0 for the chained builds)";
                license = lib.licenses.mit;
                platforms = [ "x86_64-linux" ];
              };
            };

          # One chained stage: build rustc ${entry.version} with the PREVIOUS
          # stage's toolchain as x.py stage0, install the stage-2 toolchain
          # (rustc + std + cargo) into $out. Per-version fix patches live in
          # chain-patches/<version>/ (applied in sorted order when present);
          # keep one concern per patch. An entry may set
          # `llvmPackage = "llvmPackages_NN"` to link against nixpkgs
          # LLVM instead of building the in-tree one (hours faster, but only
          # when the versions actually match — verify before flipping).
          mkChainStage =
            prevToolchain: entry:
            let
              srcTarball = mkRustcSrc {
                version = entry.version;
                hash = entry.srcHash;
              };
              patchDir = ./chain-patches + "/${entry.version}";
              stagePatches =
                if builtins.pathExists patchDir then
                  map (f: patchDir + "/${f}") (builtins.sort builtins.lessThan (builtins.attrNames (builtins.readDir patchDir)))
                else
                  [ ];
            in
            pkgs.stdenv.mkDerivation {
              pname = "rustc-${entry.version}-chained";
              version = entry.version;
              src = srcTarball;
              sourceRoot = "rustc-${entry.version}-src";
              patches = stagePatches;

              nativeBuildInputs = with pkgs; [
                autoPatchelfHook
                python3
                cmake
                ninja
                pkg-config
                gitMinimal
                which
              ];
              buildInputs = chainRuntimeInputs;

              configurePhase = ''
                runHook preConfigure
                export HOME="$TMPDIR"
                cat > config.toml <<EOF
                [build]
                rustc = "${prevToolchain}/bin/rustc"
                cargo = "${prevToolchain}/bin/cargo"
                vendor = true
                extended = true
                tools = ["cargo"]
                docs = false
                full-bootstrap = false

                [install]
                prefix = "$out"
                sysconfdir = "etc"

                [llvm]
                ninja = true
                download-ci-llvm = false
                ${lib.optionalString (entry ? llvmPackage) ''
                  [target.${rustcTarget}]
                  llvm-config = "${pkgs.${entry.llvmPackage}.llvm.dev}/bin/llvm-config"
                ''}
                [rust]
                channel = "stable"
                EOF
                runHook postConfigure
              '';

              buildPhase = ''
                runHook preBuild
                python3 x.py install --stage 2 -j ''${NIX_BUILD_CORES:-8}
                runHook postBuild
              '';

              # x.py install already populated $out. autoPatchelf runs during
              # fixup; expose the same runtime closure to this earlier smoke.
              installPhase = ''
                runHook preInstall
                export LD_LIBRARY_PATH="${lib.makeLibraryPath chainRuntimeInputs}''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
                $out/bin/rustc --version | grep -F "${entry.version}"
                printf 'fn main() { println!("Hello, world!"); }\n' > "$TMPDIR/hw.rs"
                $out/bin/rustc "$TMPDIR/hw.rs" -o "$TMPDIR/hw"
                helloWorldOutput="$("$TMPDIR/hw")"
                test "$helloWorldOutput" = "Hello, world!"
                runHook postInstall
              '';

              meta = {
                description = "rustc ${entry.version} + cargo, chain-built from rustc ${entry.prev} (mrustc bootstrap ancestry)";
                license = with lib.licenses; [
                  mit
                  asl20
                ];
                platforms = [ "x86_64-linux" ];
              };
            };

          chainPkgs = lib.optionalAttrs chainSupported (
            let
              folded = lib.foldl (
                acc: entry:
                let
                  drv = mkChainStage acc.prev entry;
                in
                {
                  prev = drv;
                  pkgs = acc.pkgs // {
                    "rustc-${entry.version}-chained" = drv;
                  };
                }
              ) { prev = stage0Toolchain; pkgs = { }; } chainRegistry.chain;
            in
            folded.pkgs
            // {
              "rustc-${chainRegistry.root}-toolchain" = stage0Toolchain;
              # Explicit opt-in selector for the historical compiler chain.
              rustc-chain-target = folded.prev;
            }
          );

          # Nix splits attr paths on `.`, so `.#1.97.1` resolves as 1 -> 97 -> 1.
          # Keep the release-string names, add `_` aliases: `nix build .#1_97_1`.
          withDotFreeAliases = attrs:
            attrs // lib.mapAttrs' (n: lib.nameValuePair (builtins.replaceStrings [ "." ] [ "_" ] n))
              (lib.filterAttrs (n: _: lib.hasInfix "." n) attrs);

          basePackages = directVersionPkgs // chainPkgs // {
            mrustc-tools = mrustcTools;
            latest = defaultPkg;
            default = defaultPkg;
          };

          baseChecks = directVersionPkgs // {
            mrustc-tools = mrustcTools;
          };

        in
        {
          # Chain stages are packages but deliberately NOT checks: each is a
          # multi-hour toolchain build, and `nix flake check` must stay usable.
          packages = withDotFreeAliases basePackages;

          checks = withDotFreeAliases baseChecks;

          devShells.default = pkgs.mkShell {
            nativeBuildInputs = with pkgs; [
              gnumake
              patch
              cmake
              pkg-config
              python3
              perl
              git
            ];
            buildInputs = with pkgs; [ zlib curl openssl libgit2 ];
            # Not added to nativeBuildInputs: an unqualified `gcc`/`g++` name collision would
            # make it unpredictable which compiler wins on PATH. Exposed as explicit env vars
            # instead -- set CXX/CC to these when driving 1.19.0/1.29.0/1.39.0/1.54.0 by hand
            # (all four need it: their bundled LLVM doesn't compile against a modern gcc 15,
            # confirmed empirically -- see `versions.*.legacyToolchain` comments above).
            LEGACY_CC = "${pkgsLegacy.gcc9Stdenv.cc}/bin/gcc";
            LEGACY_CXX = "${pkgsLegacy.gcc9Stdenv.cc}/bin/g++";
            LEGACY_OPENSSL = "${pkgsLegacy.openssl_1_1.dev}";
            shellHook = ''
              echo "mrustc dev shell: run e.g. 'make', './build-1.97.1.sh', or"
              echo "  RUSTC_VERSION=1.29.0 MRUSTC_TARGET_VER=1.29 OUTDIR_SUF=-1.29.0 make -f minicargo.mk LIBS"
              echo "by hand."
              echo "For 1.19.0/1.29.0/1.39.0/1.54.0 (old bundled LLVM; 1.19.0/1.29.0 also need old"
              echo "openssl bindings), an older gcc9 toolchain is available:"
              echo "  CXX=\$LEGACY_CXX CC=\$LEGACY_CC make ..."
              echo "and \$LEGACY_OPENSSL points at a matching openssl_1_1 (from nixpkgs-legacy)."
            '';
          };

          apps.mrustc = {
            type = "app";
            program = "${mrustcTools}/bin/mrustc";
            meta.description = "The mrustc compiler binary (run with MRUSTC_TARGET_VER set, see docs/running.md)";
          };
        };

      outputsPerSystem = forAllSystems mkOutputsForSystem;
    in
    {
      packages = builtins.mapAttrs (system: o: o.packages) outputsPerSystem;
      checks = builtins.mapAttrs (system: o: o.checks) outputsPerSystem;
      devShells = builtins.mapAttrs (system: o: o.devShells) outputsPerSystem;
      apps = builtins.mapAttrs (system: o: o.apps) outputsPerSystem;
    };
}

# chain-patches

Per-version fix patches for the chained rustc builds
(`packages."rustc-<version>-chained"`, see `chain-versions.nix`).

Layout: `chain-patches/<version>/*.patch`, applied in sorted filename order
on top of the extracted `rustc-<version>-src` tree before `x.py` runs.

Keep fixes in this directory limited to released-source compatibility
problems: patches must be minimal, one concern per file, and named
`NN-short-slug.patch`. Never patch a released tarball's `vendor/` checksums;
if a vendored crate needs changes, patch the consuming code instead.

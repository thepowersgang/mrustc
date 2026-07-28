# Chained rustc bootstrap registry.
#
# rustc N is only buildable by rustc N-1 (or N), per the stage0 policy, so
# reaching a modern rustc from mrustc's 1.90.0 ceiling means walking every
# intervening stable release. This file is the single source of truth for
# that ascent: the `rustc-<version>-chained` flake packages are generated
# from it.
#
# Each entry pins the official source tarball
#   https://static.rust-lang.org/dist/rustc-<version>-src.tar.gz
# as the SRI form of the published `.sha256` file. Only the newest patch
# release of each minor is listed (the chain never needs two patch levels
# of the same minor). Extend the list only with hashes from the published
# checksums, never from trust-on-first-use downloads.
{
  # Chain root: produced by mrustc itself (packages."rustc-1.90.0-toolchain").
  root = "1.90.0";

  # Ordered ascent, oldest first. `prev` names the toolchain that builds
  # this version; the first entry's prev is the chain root.
  chain = [
    { version = "1.91.1"; prev = "1.90.0"; srcHash = "sha256-ONziBdOfYVcSYfBEQjehzp7+y5cOdg2OxNlXr1tEVyM="; }
    { version = "1.92.0"; prev = "1.91.1"; srcHash = "sha256-ng0sp1x+J1/cdYJVv0sDr7PWXRVDYCdGkHyTO2kBw7g="; }
    { version = "1.93.1"; prev = "1.92.0"; srcHash = "sha256-TCMKRLPZyfPO+VCUNxn4OABY0nyR/aXjapqUfvAT4B8="; }
    { version = "1.94.1"; prev = "1.93.1"; srcHash = "sha256-TBQqYl8S4833FsaK4Z9PYNmK0UgmJ7CFebFYOOla1RQ="; }
    { version = "1.95.0"; prev = "1.94.1"; srcHash = "sha256-6puCqD5GlnU3w1ac6db6FoEcBDqW5lE3bDSecCQcpRU="; }
    { version = "1.96.1"; prev = "1.95.0"; srcHash = "sha256-0Km1GYxBhoU4rhKvKAZBY1UdBtzOqxHvCxvJqm6Yt6c="; }
    { version = "1.97.1"; prev = "1.96.1"; srcHash = "sha256-YiwrQpxTy/3A3TpR0DVU6RzWPr7BkSwfVwlkDN/vGp0="; }
  ];
}

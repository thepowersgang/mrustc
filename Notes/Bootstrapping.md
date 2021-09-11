rustc bootstrap procedure

1. Build libstd,etc with mrustc+minicargo
  > Use build script overrides to avoid need for build scripts (which require libstd)
2. Build rustc/cargo with mrustc+minicargo
3. Build libstd wih rustc+minicargo
  > Also requires script overrides
  - This exists to create a libstd with matching symbol hashes as emitted by rustc
    - mrustc-built rustc emits different (but consistent) symbol hashes to the rustc-built rustc
4. Build a new rustc
  - Required for proc macros to link correctly (PMs built with rustc can't link against the mrustc-built rustc)
5. Build a new libstd with matching rustc
  - This is the final libstd

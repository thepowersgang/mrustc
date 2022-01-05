@REM Builds rustc 1.39.0 using mrustc-built rustc 1.39.0
@REM INPUTS
@REM - mrustc-built rustc & cargo
@REM   Build using build_rustc_minicargo_139.cmd AND build_cargo_minicargo_139.cmd
@REM - LLVM: either by setting LLVM_CONFIG or using %OUTDIR%llvm-prefix
@REM   which is built by build_rustc_minicargo_139.cmd
@REM OUTPUTS
@REM - rust sysroot in output-1.39.0\prefix

@setlocal
@call build_std_139.cmd
@if %errorlevel% neq 0 exit /b %errorlevel%

set RUST_SRC=%~dp0..\rustc-%RUSTC_VERSION%-src\src\
set VENDOR_DIR=%RUST_SRC%..\vendor

@REM Stage X: Standard library built using `rustc` (using cargo)
set PREFIX=%OUTDIR%prefix\
set BINDIR=%PREFIX%bin\
set LIBDIR=%PREFIX%\lib\rustlib\%RUSTC_TARGET%\lib\
set CARGO_HOME=%PREFIX%cargo_home\
@REM Stage 2: standard library built with `rustc` (using minicargo)
set PREFIX_2=%OUTDIR%prefix-2\
set LIBDIR_2=%PREFIX_2%lib\rustlib\%RUSTC_TARGET%\lib\
set BINDIR_2=%PREFIX_2%bin\
@REM Stage 1: standard library built with `rustc_m` (using minicargo)
set PREFIX_S=%OUTDIR%prefix-s\
set LIBDIR_S=%PREFIX_S%lib\rustlib\%RUSTC_TARGET%\lib\
set BINDIR_S=%PREFIX_S%bin\

set CFG_RELEASE=%RUSTC_VERSION%
set CFG_RELEASE_CHANNEL=stable
set CFG_VERSION=%RUSTC_VERSION%-stable-mrustc
set CFG_PREFIX=mrustc
set CFG_LIBDIR_RELATIVE=lib
set REAL_LIBRARY_PATH_VAR=PATH
set REAL_LIBRARY_PATH=%PATH%
set RUSTC_INSTALL_BINDIR=bin
set RUSTC_BOOTSTRAP=1
set CXXFLAGS=/MT

mkdir %BINDIR%
mkdir %BINDIR_2%
mkdir %BINDIR_S%
mkdir %LIBDIR%
mkdir %LIBDIR_2%
mkdir %LIBDIR_S%
mkdir %CARGO_HOME%

@REM Copy bootstrapping binaries
copy %OUTDIR%rustc-build\rustc_binary.exe %BINDIR_S%rustc.exe
copy %OUTDIR%cargo-build\cargo.exe %BINDIR%\cargo.exe

echo [source.crates-io] > %CARGO_HOME%config
echo replace-with = "vendored-sources" >> %CARGO_HOME%config
echo [source.vendored-sources] >> %CARGO_HOME%config
echo directory = "%VENDOR_DIR:\=\\%" >> %CARGO_HOME%config

@REM Build libstd using minicargo + rustc
set MRUSTC_PATH=%~dp0%BINDIR_S%rustc.exe
x64\Release\minicargo.exe %RUST_SRC%libstd --vendor-dir %VENDOR_DIR% --script-overrides ../script-overrides/stable-%RUSTC_VERSION%-windows/ --output-dir %LIBDIR_S%
@if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\minicargo.exe %RUST_SRC%libtest --vendor-dir %VENDOR_DIR% --script-overrides ../script-overrides/stable-%RUSTC_VERSION%-windows/ --output-dir %LIBDIR_S%
@if %errorlevel% neq 0 exit /b %errorlevel%
set MRUSTC_PATH=

@REM Build rustc with itself (so we have a rustc with the right ABI)
@IF "%LLVM_CONFIG%"=="" set LLVM_CONFIG=%~dp0%OUTDIR%llvm-prefix\bin\llvm-config.exe
set TMPDIR=%~dp0%PREFIX%tmp
set CARGO_TARGET_DIR=%OUTDIR%bootstrap-rustc\
set RUSTC=%BINDIR_S%rustc
set RUSTFLAGS=-Z force-unstable-if-unmarked
set RUSTC_BOOTSTRAP=1
set RUSTC_ERROR_METADATA_DST=%PREFIX%
set CFG_RELEASE_CHANNEL=stable
set CFG_PREFIX=mrustc
set CFG_LIBDIR_RELATIVE=lib
mkdir %TMPDIR%
mkdir %CARGO_TARGET_DIR%
%BINDIR%cargo build --manifest-path %RUST_SRC%rustc/Cargo.toml --release -j 1 --verbose
@if %errorlevel% neq 0 exit /b %errorlevel%
%BINDIR%cargo rustc --manifest-path %RUST_SRC%librustc_codegen_llvm/Cargo.toml --release -j 1 --verbose -- -L %~dp0%CARGO_TARGET_DIR%release\deps
@if %errorlevel% neq 0 exit /b %errorlevel%
copy /y %LIBDIR%*.dll %BINDIR%
copy /y %CARGO_TARGET_DIR%release\deps\*.rlib %LIBDIR%
copy /y %CARGO_TARGET_DIR%release\deps\*.dll* %LIBDIR%
copy /y %CARGO_TARGET_DIR%release\deps\*.dll %BINDIR%
copy /y %CARGO_TARGET_DIR%release\rustc_binary.exe %BINDIR%rustc.exe
mkdir %LIBDIR%..\codegen-backends
copy /y %CARGO_TARGET_DIR%release\rustc_codegen_llvm.dll %LIBDIR%..\codegen-backends\rustc_codegen_llvm-llvm.dll

@REM Build libstd and friends with the final rustc, but using minicargo (to avoid running build scripts)
mkdir %LIBDIR_2%..\codegen-backends
copy /y %BINDIR%rustc.exe %BINDIR_2%rustc.exe
copy /y %BINDIR%*.dll %BINDIR_2%
copy /y %LIBDIR%..\codegen-backends\rustc_codegen_llvm-llvm.dll %LIBDIR_2%..\codegen-backends\
set MRUSTC_PATH=%BINDIR%rustc.exe
x64\Release\minicargo.exe %RUST_SRC%libtest --vendor-dir %VENDOR_DIR% --script-overrides ../script-overrides/stable-%RUSTC_VERSION%-windows/ --output-dir %LIBDIR_2%
@if %errorlevel% neq 0 exit /b %errorlevel%
set MRUSTC_PATH=

@REM Actual libstd build (using cargo, and using the above-built libstd as deps)
set CARGO_TARGET_DIR=%OUTDIR%bootstrap-std\
set RUSTC=%BINDIR_2%rustc.exe
%BINDIR%cargo build --manifest-path %RUST_SRC%libtest\Cargo.toml -j 1 --release --features panic-unwind -v
@if %errorlevel% neq 0 exit /b %errorlevel%
copy /y %CARGO_TARGET_DIR%release\deps\*.rlib %LIBDIR%
copy /y %CARGO_TARGET_DIR%release\deps\*.dll* %LIBDIR%


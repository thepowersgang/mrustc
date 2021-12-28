@call build_std_154.cmd
@if %errorlevel% neq 0 exit /b %errorlevel%

@mkdir %OUTDIR%\rustc-build
@set RUST_CHECK=1
@set RUSTC_INSTALL_BINDIR=bin
@set CFG_RELEASE=1.54.0
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\compiler\rustc_driver -L %OUTDIR% --output-dir %OUTDIR%\rustc-build %COMMON_ARGS%
@if %errorlevel% neq 0 exit /b %errorlevel%
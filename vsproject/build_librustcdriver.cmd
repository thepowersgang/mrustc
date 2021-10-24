@call build_std.cmd
@if %errorlevel% neq 0 exit /b %errorlevel%

@mkdir %OUTDIR%\rustc-build
@set RUST_CHECK=1
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\librustc_driver -L %OUTDIR% --output-dir %OUTDIR%\rustc-build %COMMON_ARGS%
@if %errorlevel% neq 0 exit /b %errorlevel%
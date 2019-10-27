@call build_std.cmd
@if %errorlevel% neq 0 exit /b %errorlevel%

@mkdir %OUTDIR%\cargo-build
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\tools\cargo -L %OUTDIR% --output-dir %OUTDIR%\cargo-build --vendor-dir ..\rustc-%RUSTC_VERSION%-src\src\vendor
@if %errorlevel% neq 0 exit /b %errorlevel%
@call build_std.cmd
@if %errorlevel% neq 0 exit /b %errorlevel%

@mkdir %OUTDIR%\cargo-build
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\tools\cargo -L %OUTDIR% --output-dir %OUTDIR%\cargo-build %COMMON_ARGS%
@if %errorlevel% neq 0 exit /b %errorlevel%

%OUTDIR%\cargo-build\cargo.exe --version
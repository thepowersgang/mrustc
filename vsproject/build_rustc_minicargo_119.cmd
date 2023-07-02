@call run_hello_119.cmd
@if %errorlevel% neq 0 exit /b %errorlevel%

@mkdir %OUTDIR%\rustc-build
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\rustc -L %OUTDIR% --output-dir %OUTDIR%\rustc-build %COMMON_ARGS%
@if %errorlevel% neq 0 exit /b %errorlevel%
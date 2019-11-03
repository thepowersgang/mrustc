@call build_std.cmd
@if %errorlevel% neq 0 exit /b %errorlevel%

@mkdir %OUTDIR%\tests\
x64\Release\testrunner.exe ..\rustc-%RUSTC_VERSION%-src\src\test\run-pass -o %OUTDIR%\tests\ -L %OUTDIR%
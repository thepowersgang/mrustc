@call build_std.cmd
@if %errorlevel% neq 0 exit /b %errorlevel%

x64\Release\mrustc.exe ..\rustc-%RUSTC_VERSION%-src\src\test\run-pass\hello.rs -L %OUTDIR% -o %OUTDIR%\hello.exe -g
@if %errorlevel% neq 0 exit /b %errorlevel%
%OUTDIR%\hello.exe
@if %errorlevel% neq 0 exit /b %errorlevel%
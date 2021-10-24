@call build_std_154.cmd
@if %errorlevel% neq 0 exit /b %errorlevel%

x64\Release\mrustc.exe ..\rustc-%RUSTC_VERSION%-src\src\test\ui\hello.rs -L %OUTDIR% -o %OUTDIR%\hello.exe -g > %OUTDIR%\hello.exe_dbg.txt
@if %errorlevel% neq 0 exit /b %errorlevel%
%OUTDIR%\hello.exe
@if %errorlevel% neq 0 exit /b %errorlevel%
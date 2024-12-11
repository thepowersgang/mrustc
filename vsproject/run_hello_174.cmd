@call build_std_174.cmd
@if %errorlevel% neq 0 exit /b %errorlevel%

x64\Release\mrustc.exe ..\rustc-%RUSTC_VERSION%-src\tests\ui\hello_world\main.rs -L %OUTDIR% -o %OUTDIR%\hello.exe -g > %OUTDIR%\hello.exe_dbg.txt
@if %errorlevel% neq 0 exit /b %errorlevel%
%OUTDIR%\hello.exe
@if %errorlevel% neq 0 exit /b %errorlevel%
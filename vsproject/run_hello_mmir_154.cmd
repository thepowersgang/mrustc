@call build_std_154.cmd mmir
@if %errorlevel% neq 0 exit /b %errorlevel%

x64\Release\mrustc.exe ..\rustc-%RUSTC_VERSION%-src\src\test\ui\hello.rs -L %OUTDIR% -o %OUTDIR%\hello.exe -C codegen-type=monomir > %OUTDIR%\hello.exe_dbg.txt
@if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\standalone_miri.exe --logfile smiri.log %OUTDIR%\hello.exe.mir
@if %errorlevel% neq 0 exit /b %errorlevel%
@set MRUSTC_TARGET_VER=1.19
@set RUSTC_VERSION=1.19.0
@set OUTDIR=output-%RUSTC_VERSION%-mmir
@set STD_ARGS=--output-dir %OUTDIR%
@set STD_ARGS=%STD_ARGS% --vendor-dir ..\rustc-%RUSTC_VERSION%-src\src\vendor
@set STD_ARGS=%STD_ARGS% --script-overrides ..\script-overrides\stable-%RUSTC_VERSION%-windows
@set STD_ARGS=%STD_ARGS% --features backtrace
@mkdir %OUTDIR%

x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\libstd %STD_ARGS% -Z emit-mmir
@if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\libpanic_unwind %STD_ARGS% -Z emit-mmir
@if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\libtest %STD_ARGS% -Z emit-mmir
@if %errorlevel% neq 0 exit /b %errorlevel%

x64\Release\mrustc.exe ..\rustc-%RUSTC_VERSION%-src\src\test\run-pass\hello.rs -L %OUTDIR% -o %OUTDIR%\hello.exe -C codegen-type=monomir > %OUTDIR%\hello.exe_dbg.txt
if %errorlevel% neq 0 exit /b %errorlevel%
echo on
x64\Release\standalone_miri.exe --logfile smiri.log %OUTDIR%\hello.exe.mir
@if %errorlevel% neq 0 exit /b %errorlevel%
@echo off
@set MRUSTC_TARGET_VER=1.29
@set RUSTC_VERSION=1.29.0
@set OUTDIR=output-%RUSTC_VERSION%-mmir
@mkdir %OUTDIR%

x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\libstd --script-overrides ..\script-overrides\stable-%RUSTC_VERSION%-windows --output-dir %OUTDIR% -Z emit-mmir
if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\libpanic_unwind --script-overrides ..\script-overrides\stable-%RUSTC_VERSION%-windows --output-dir %OUTDIR% -Z emit-mmir
if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\libtest --vendor-dir ..\rustc-%RUSTC_VERSION%-src\src\vendor --script-overrides ..\script-overrides\stable-%RUSTC_VERSION%-windows --output-dir %OUTDIR% -Z emit-mmir
if %errorlevel% neq 0 exit /b %errorlevel%

x64\Release\mrustc.exe ..\rustc-%RUSTC_VERSION%-src\src\test\run-pass\hello.rs -L %OUTDIR% -o %OUTDIR%\hello.exe -C codegen-type=monomir > %OUTDIR%\hello.exe_dbg.txt
if %errorlevel% neq 0 exit /b %errorlevel%
echo on
x64\Release\standalone_miri.exe --logfile smiri.log %OUTDIR%\hello.exe.mir
@if %errorlevel% neq 0 exit /b %errorlevel%
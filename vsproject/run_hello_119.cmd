@set RUSTC_VERSION=1.19.0
@set MRUSTC_TARGET_VER=1.19
@set OUTDIR=output-%RUSTC_VERSION%
@set COMMON_ARGS=--vendor-dir ..\rustc-%RUSTC_VERSION%-src\src\vendor
@if defined PARLEVEL ( set COMMON_ARGS=%COMMON_ARGS% -j %PARLEVEL% )
@set STD_ARGS=%COMMON_ARGS%
@set STD_ARGS=%STD_ARGS% --output-dir %OUTDIR%
@set STD_ARGS=%STD_ARGS% --script-overrides ..\script-overrides\stable-%RUSTC_VERSION%-windows
@set STD_ARGS=%STD_ARGS% --features backtrace
@mkdir %OUTDIR%
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\libstd %STD_ARGS%
@if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\libpanic_unwind %STD_ARGS%
@if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\libtest %STD_ARGS%
@if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\minicargo.exe ..\lib\libproc_macro --output-dir %OUTDIR%
@if %errorlevel% neq 0 exit /b %errorlevel%

x64\Release\mrustc.exe ..\rustc-%RUSTC_VERSION%-src\src\test\run-pass\hello.rs -L %OUTDIR% -o %OUTDIR%\hello.exe -g > %OUTDIR%\hello.exe_dbg.txt
@if %errorlevel% neq 0 exit /b %errorlevel%
%OUTDIR%\hello.exe
@if %errorlevel% neq 0 exit /b %errorlevel%
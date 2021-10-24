@set RUSTC_VERSION=1.39.0
@set MRUSTC_TARGET_VER=1.39
@set OUTDIR=output-%RUSTC_VERSION%
@set COMMON_ARGS=--vendor-dir ..\rustc-%RUSTC_VERSION%-src\vendor --manifest-overrides ..\rustc-%RUSTC_VERSION%-overrides.toml
@if defined PARLEVEL ( set COMMON_ARGS=%COMMON_ARGS% -j %PARLEVEL% )
@set STD_ARGS=--output-dir %OUTDIR% %COMMON_ARGS%
@set STD_ARGS=%STD_ARGS% --script-overrides ..\script-overrides\stable-%RUSTC_VERSION%-windows
@mkdir %OUTDIR%
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\libstd %STD_ARGS%
@if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\libpanic_unwind %STD_ARGS%
@if %errorlevel% neq 0 exit /b %errorlevel%
@rem Build libproc_macro BEFORE libtest (ensures that it's built instead of the rustc one)
x64\Release\minicargo.exe ..\lib\libproc_macro --output-dir %OUTDIR%
@if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\libtest %STD_ARGS%
@if %errorlevel% neq 0 exit /b %errorlevel%
@rem x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\liballoc_system %STD_ARGS%
@rem @if %errorlevel% neq 0 exit /b %errorlevel%
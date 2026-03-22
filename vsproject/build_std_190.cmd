@set MRUSTC_TARGET_VER=1.90
@set RUSTC_VERSION=1.90.0
@set OUTDIR=output-%RUSTC_VERSION%
@set COMMON_ARGS=--vendor-dir ..\rustc-%RUSTC_VERSION%-src\vendor --manifest-overrides ..\rustc-%RUSTC_VERSION%-overrides.toml
@if defined PARLEVEL ( set COMMON_ARGS=%COMMON_ARGS% -j %PARLEVEL% )
@if "%1" == "mmir" (
	set COMMON_ARGS=%COMMON_ARGS% -Z emit-mmir
	set OUTDIR=%OUTDIR%-mmir
)
@set STD_ARGS=--output-dir %OUTDIR% %COMMON_ARGS%
@set STD_ARGS=%STD_ARGS% --script-overrides ..\script-overrides\stable-%RUSTC_VERSION%-windows
@mkdir %OUTDIR%

@set STD_ENV_ARCH=amd64

x64\Release\minipatch.exe ..\rustc-%RUSTC_VERSION%-src.patch ..\rustc-%RUSTC_VERSION%-src
@if %errorlevel% neq 0 exit /b %errorlevel%

x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\library\test %STD_ARGS%
@if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\minicargo.exe ..\lib\libproc_macro %COMMON_ARGS% --output-dir %OUTDIR%
@if %errorlevel% neq 0 exit /b %errorlevel%
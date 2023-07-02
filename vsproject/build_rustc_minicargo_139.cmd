@call build_std_139.cmd
@if %errorlevel% neq 0 exit /b %errorlevel%

@mkdir %OUTDIR%\llvm
@rem cmake -B %OUTDIR%\llvm -S ..\rustc-%RUSTC_VERSION%-src\src\llvm-project\llvm -G "Visual Studio 14 2015"
cmake -B %OUTDIR%\llvm -S ..\rustc-%RUSTC_VERSION%-src\src\llvm-project\llvm -G "Visual Studio 16 2019"
@if %errorlevel% neq 0 exit /b %errorlevel%
pushd %OUTDIR%\llvm
msbuild
@if %errorlevel% neq 0 exit /b %errorlevel%
popd

@mkdir %OUTDIR%\rustc-build
@set RUSTC_INSTALL_BINDIR=bin
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\rustc -L %OUTDIR% --output-dir %OUTDIR%\rustc-build %COMMON_ARGS%
@if %errorlevel% neq 0 exit /b %errorlevel%
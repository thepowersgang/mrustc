@setlocal
@call build_std_139.cmd
@if %errorlevel% neq 0 exit /b %errorlevel%

@IF "%LLVM_CONFIG%"=="" (
    @REM Build LLVM
    @mkdir %OUTDIR%llvm-build
    pushd %OUTDIR%llvm-build
    cmake "%~dp0..\rustc-%RUSTC_VERSION%-src\src\llvm-project\llvm" "-G" "Ninja" "-DLLVM_ENABLE_ASSERTIONS=OFF" ^
    "-DLLVM_TARGETS_TO_BUILD=X86"  "-DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=" ^
    "-DLLVM_INCLUDE_EXAMPLES=OFF" "-DLLVM_INCLUDE_TESTS=OFF" "-DLLVM_INCLUDE_DOCS=OFF" ^
    "-DLLVM_INCLUDE_BENCHMARKS=OFF" "-DLLVM_ENABLE_ZLIB=OFF" "-DWITH_POLLY=OFF" "-DLLVM_ENABLE_TERMINFO=OFF" ^
    "-DLLVM_ENABLE_LIBEDIT=OFF" "-DLLVM_ENABLE_Z3_SOLVER=OFF" ^
    "-DLLVM_TARGET_ARCH=x86_64" "-DLLVM_DEFAULT_TARGET_TRIPLE=x86_64-pc-windows-msvc" ^
    "-DLLVM_USE_CRT_DEBUG=MT" "-DLLVM_USE_CRT_RELEASE=MT" "-DLLVM_USE_CRT_RELWITHDEBINFO=MT" "-DLLVM_ENABLE_LIBXML2=OFF" ^
    "-DLLVM_VERSION_SUFFIX=-rust-1.39.0-mrustc" "-DCMAKE_INSTALL_MESSAGE=LAZY" ^
    "-DCMAKE_C_FLAGS=/nologo /MT" "-DCMAKE_CXX_FLAGS=/nologo /MT" ^
    "-DCMAKE_INSTALL_PREFIX=%~dp0%OUTDIR%llvm-prefix" "-DCMAKE_BUILD_TYPE=Release"
    popd
    @if %errorlevel% neq 0 exit /b %errorlevel%

    cmake --build %OUTDIR%llvm-build --target install --config Release
    @if %errorlevel% neq 0 exit /b %errorlevel%

    set LLVM_CONFIG=%~dp0%OUTDIR%llvm-prefix\bin\llvm-config.exe
)

@mkdir %OUTDIR%rustc-build
set CFG_RELEASE=%RUSTC_VERSION%
set CFG_VERSION=%RUSTC_VERSION%-stable-mrustc

@set REAL_LIBRARY_PATH_VAR=PATH
@set REAL_LIBRARY_PATH=%PATH%
@set RUSTC_INSTALL_BINDIR=bin
@set CXXFLAGS=/MT

echo HOST_TRIPLE = %CFG_COMPILER_HOST_TRIPLE%
x64\Release\minicargo.exe ..\rustc-%RUSTC_VERSION%-src\src\rustc -L %OUTDIR% --output-dir %OUTDIR%rustc-build %COMMON_ARGS%
@if %errorlevel% neq 0 exit /b %errorlevel%

%OUTDIR%rustc-build\rustc_binary.exe --version

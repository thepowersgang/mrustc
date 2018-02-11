@echo off
x64\Release\minicargo.exe ..\rustc-1.19.0-src\src\libstd --script-overrides ..\script-overrides\stable-1.19.0 --output-dir output_mmir -Z emit-mmir
if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\minicargo.exe ..\rustc-1.19.0-src\src\libpanic_unwind --script-overrides ..\script-overrides\stable-1.19.0 --output-dir output_mmir -Z emit-mmir
if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\minicargo.exe ..\rustc-1.19.0-src\src\libtest --script-overrides ..\script-overrides\stable-1.19.0 --output-dir output_mmir -Z emit-mmir
if %errorlevel% neq 0 exit /b %errorlevel%

x64\Release\mrustc.exe ..\rustc-1.19.0-src\src\test\run-pass\hello.rs -L output_mmir -o output_mmir\hello.exe -C codegen-type=monomir
if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\standalone_miri.exe output_mmir\hello.exe.mir
if %errorlevel% neq 0 exit /b %errorlevel%
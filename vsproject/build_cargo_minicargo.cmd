@echo off
call build_std.cmd
if %errorlevel% neq 0 exit /b %errorlevel%

mkdir output\cargo-build
x64\Release\minicargo.exe ..\rustc-1.19.0-src\src\tools\cargo -L output --output-dir output\cargo-build --vendor-dir ..\rustc-1.19.0-src\src\vendor
if %errorlevel% neq 0 exit /b %errorlevel%
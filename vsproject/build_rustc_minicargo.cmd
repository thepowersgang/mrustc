@echo off
call build_std.cmd
if %errorlevel% neq 0 exit /b %errorlevel%

mkdir output\rustc-build
x64\Release\minicargo.exe ..\rustc-1.19.0-src\src\rustc -L output --output-dir output\rustc-build --vendor-dir ..\rustc-1.19.0-src\src\vendor
if %errorlevel% neq 0 exit /b %errorlevel%
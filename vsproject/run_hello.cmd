@echo off
call build_std.cmd
if %errorlevel% neq 0 exit /b %errorlevel%

x64\Release\mrustc.exe ..\rustc-1.19.0-src\src\test\run-pass\hello.rs -L output -o output\hello.exe -g
if %errorlevel% neq 0 exit /b %errorlevel%
output\hello.exe
if %errorlevel% neq 0 exit /b %errorlevel%
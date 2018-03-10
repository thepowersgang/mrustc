@echo off
call build_std.cmd
if %errorlevel% neq 0 exit /b %errorlevel%

mkdir output\tests\
x64\Release\testrunner.exe ..\rustc-1.19.0-src\src\test\run-pass -o output\tests\
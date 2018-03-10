@echo off
call build_std_and_hello.cmd
if %errorlevel% neq 0 exit /b %errorlevel%

mkdir output\tests\
x64\Release\testrunner.exe ..\rustc-1.19.0-src\src\test\run-pass -o output\tests\
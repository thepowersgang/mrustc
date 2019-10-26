@echo off
x64\Release\minicargo.exe ..\rustc-1.19.0-src\src\libstd --script-overrides ..\script-overrides\stable-1.19.0
if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\minicargo.exe ..\rustc-1.19.0-src\src\libpanic_unwind --script-overrides ..\script-overrides\stable-1.19.0
if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\minicargo.exe ..\rustc-1.19.0-src\src\libtest --script-overrides ..\script-overrides\stable-1.19.0
if %errorlevel% neq 0 exit /b %errorlevel%
x64\Release\minicargo.exe ..\lib\libproc_macro
if %errorlevel% neq 0 exit /b %errorlevel%

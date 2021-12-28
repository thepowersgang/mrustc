@echo off
echo cargo 1.29
call build_cargo_minicargo.cmd
if %errorlevel% neq 0 exit /b %errorlevel%
echo rustc 1.29
call build_librustcdriver.cmd
if %errorlevel% neq 0 exit /b %errorlevel%

echo cargo 1.19
call build_cargo_minicargo_119.cmd
if %errorlevel% neq 0 exit /b %errorlevel%
rem Don't build, llvm
rem echo rustc 1.19
rem call build_librustcdriver_119.cmd
rem if %errorlevel% neq 0 exit /b %errorlevel%

echo cargo 1.39
call build_cargo_minicargo_139.cmd
if %errorlevel% neq 0 exit /b %errorlevel%
echo rustc 1.39
call build_librustcdriver_139.cmd
if %errorlevel% neq 0 exit /b %errorlevel%

echo rustc 1.54
call build_cargo_minicargo_154.cmd
if %errorlevel% neq 0 exit /b %errorlevel%
call build_librustcdriver_154.cmd
if %errorlevel% neq 0 exit /b %errorlevel%

@echo --SUCCESS--
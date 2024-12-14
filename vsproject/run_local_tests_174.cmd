@call build_std_174.cmd
@if %errorlevel% neq 0 exit /b %errorlevel%

@mkdir %OUTDIR%\local_tests\
x64\Release\testrunner.exe ..\samples\test -o %OUTDIR%\local_tests\ -L %OUTDIR%
@if %errorlevel% neq 0 exit /b %errorlevel%

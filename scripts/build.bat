@echo off
rem Build + test libpsrp with MSVC (Ninja). Usage: scripts\build.bat [clean]
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo failed to init MSVC environment & exit /b 1 )

set ROOT=%~dp0..
set BUILD=%ROOT%\build\msvc
if "%1"=="clean" if exist "%BUILD%" rmdir /s /q "%BUILD%"

cmake -G Ninja -S "%ROOT%" -B "%BUILD%" -DCMAKE_BUILD_TYPE=Debug || exit /b 1
cmake --build "%BUILD%" || exit /b 1
ctest --test-dir "%BUILD%" --output-on-failure || exit /b 1
echo.
echo MSVC: build + tests OK

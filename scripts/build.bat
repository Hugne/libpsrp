@echo off
rem Build + test libpsrp with MSVC (Ninja). Usage: scripts\build.bat [clean]
setlocal
rem Prefer the known local install; fall back to vswhere so this also runs on
rem a CI runner, where Visual Studio lives somewhere else entirely.
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not exist "%VCVARS%" ( echo no MSVC install found & exit /b 1 )
call "%VCVARS%" >nul 2>&1
if errorlevel 1 ( echo failed to init MSVC environment & exit /b 1 )

set ROOT=%~dp0..
set BUILD=%ROOT%\build\msvc
if "%1"=="clean" if exist "%BUILD%" rmdir /s /q "%BUILD%"

cmake -G Ninja -S "%ROOT%" -B "%BUILD%" -DCMAKE_BUILD_TYPE=Debug || exit /b 1
cmake --build "%BUILD%" || exit /b 1
ctest --test-dir "%BUILD%" --output-on-failure || exit /b 1
echo.
echo MSVC: build + tests OK

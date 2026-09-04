@echo off
rem Build + test libpsrp with llvm-mingw clang (Ninja).
rem Usage: scripts\build-clang.bat [clean]
setlocal
set LLVM=C:\Users\user\AppData\Local\Programs\llvm-mingw-20260602-ucrt-x86_64\bin
if not exist "%LLVM%\clang.exe" ( echo clang not found at %LLVM% & exit /b 1 )
set PATH=%LLVM%;%PATH%

set ROOT=%~dp0..
set BUILD=%ROOT%\build\clang
if "%1"=="clean" if exist "%BUILD%" rmdir /s /q "%BUILD%"

cmake -G Ninja -S "%ROOT%" -B "%BUILD%" ^
  -DCMAKE_BUILD_TYPE=Debug ^
  -DCMAKE_C_COMPILER=clang.exe ^
  -DCMAKE_RC_COMPILER=llvm-rc.exe || exit /b 1
cmake --build "%BUILD%" || exit /b 1
ctest --test-dir "%BUILD%" --output-on-failure || exit /b 1
echo.
echo clang: build + tests OK

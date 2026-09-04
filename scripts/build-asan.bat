@echo off
rem Build + test libpsrp under AddressSanitizer (MSVC, Ninja).
rem Usage: scripts\build-asan.bat [clean]
rem
rem The normal builds catch crashes. This one catches the reads and writes that
rem land just outside an allocation and would otherwise pass quietly. It is
rem where the fuzzer earns its keep, so this runs the fuzz label as well.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo failed to init MSVC environment & exit /b 1 )

set ROOT=%~dp0..
set BUILD=%ROOT%\build\asan
if "%1"=="clean" if exist "%BUILD%" rmdir /s /q "%BUILD%"

rem /fsanitize=address needs the debug info to symbolise reports, and MSVC
rem refuses to combine it with incremental linking or edit-and-continue debug
rem info, hence the explicit /Zi.
cmake -G Ninja -S "%ROOT%" -B "%BUILD%" ^
  -DCMAKE_BUILD_TYPE=Debug ^
  -DCMAKE_C_FLAGS="/fsanitize=address /Zi" ^
  -DCMAKE_EXE_LINKER_FLAGS="/DEBUG" || exit /b 1
cmake --build "%BUILD%" || exit /b 1

rem A longer fuzz run than the default: this build is the one that can see the
rem errors, so it is worth giving it more input.
set PSRP_FUZZ_ITERATIONS=200000
ctest --test-dir "%BUILD%" --output-on-failure || exit /b 1
echo.
echo ASan: build + tests OK

@echo off
rem Generates the API reference into build\doc\html.
rem Needs doxygen on PATH: winget install DimitriVanHeesch.Doxygen

setlocal
cd /d "%~dp0.."

where doxygen >nul 2>&1
if errorlevel 1 (
  echo doxygen not found on PATH.
  echo Install it with: winget install DimitriVanHeesch.Doxygen
  exit /b 1
)

rem doxygen will not create a nested output directory, and a clean clone has
rem no build\ at all.
if not exist build\doc mkdir build\doc

rem Warnings mean the published docs are wrong (mis-parsed XML tags in comments
rem silently mangle the output), so treat any output on stderr as a failure.
doxygen docs\Doxyfile 2>docs-warnings.txt
if errorlevel 1 goto fail

for %%A in (docs-warnings.txt) do if %%~zA NEQ 0 (
  type docs-warnings.txt
  del docs-warnings.txt
  echo docs: doxygen reported warnings
  exit /b 1
)
del docs-warnings.txt
echo docs: build\doc\html\index.html
exit /b 0

:fail
if exist docs-warnings.txt type docs-warnings.txt & del docs-warnings.txt
echo docs: FAILED
exit /b 1

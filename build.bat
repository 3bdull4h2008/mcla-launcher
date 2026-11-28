@echo off
setlocal
rem Standalone MCLA launcher host - builds mcla_launcher.exe (per-pixel alpha WebView2).
rem Requires: VS BuildTools (vcvars64), LLVM clang-cl, WebView2 Runtime installed.
set "MSYS_NO_PATHCONV=1"
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\Python314;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
  echo ERROR: vcvars64.bat not found - install VS BuildTools with C++ workload.
  exit /b 2
)
cd /d "%~dp0"
if not exist out mkdir out

set "INC=-I sdk -I sdk\wv2"
set "LIBS=user32.lib gdi32.lib ole32.lib dcomp.lib dwmapi.lib shlwapi.lib shell32.lib advapi32.lib comdlg32.lib urlmon.lib"

rc /nologo /fo out\mcla_launcher.res src\mcla_launcher.rc
if errorlevel 1 (
  echo RESOURCE FAILED
  exit /b 3
)

clang-cl /nologo /std:c++17 /EHsc /W3 /DUNICODE /D_UNICODE /DNOMINMAX ^
  %INC% /Fo"out\\" /Fd"out\\" ^
  src\mcla_launcher.cpp out\mcla_launcher.res ^
  /link /OUT:out\mcla_launcher.exe /MANIFEST:EMBED /MANIFESTINPUT:src\mcla_launcher.manifest %LIBS%
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 3
)

echo Built single-file: %~dp0out\mcla_launcher.exe
endlocal

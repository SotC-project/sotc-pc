@echo off
rem Shadow of the Colossus - PC build launcher.
rem Game data is read from the PS4 dump folder below; saves and settings live in .\user
set "GAME=D:\Games\CUSA08809\eboot.bin"
rem The optimised build (build-lto.bat) when present, else the development build.
set "EXE=%~dp0ShadowOfTheColossus.exe"
if not exist "%EXE%" set "EXE=%~dp0..\runtime\Build\x64-Clang-Release\shadps4.exe"
if not exist "%EXE%" set "EXE=%~dp0..\runtime\Build\x64-Clang-RelWithDebInfo\shadps4.exe"
if not exist "%GAME%" (
  echo Game data not found: %GAME%
  pause
  exit /b 1
)
if not exist "%EXE%" (
  echo Runtime not built yet: %EXE%
  echo Run ..\build.bat first.
  pause
  exit /b 1
)
cd /d "%~dp0"
start "" "%EXE%" --game "%GAME%" %*

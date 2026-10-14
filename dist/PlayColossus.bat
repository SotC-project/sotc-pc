@echo off
rem Starts the game directly in a colossus arena (engine debug start), skipping the journey there.
rem Usage: PlayColossus.bat [1-16]   (default 1). Normal play: Play.bat
setlocal
set "N=%~1"
if "%N%"=="" set "N=1"
set /a BOSS=%N%-1
set "GAME=D:\Games\CUSA08809\eboot.bin"
set "ARGS=D:\Games\CUSA08809-mods\CommandLineArgs.txt"
set "EXE=%~dp0ShadowOfTheColossus.exe"
if not exist "%EXE%" set "EXE=%~dp0..\runtime\Build\x64-Clang-Release\shadps4.exe"
if not exist "%EXE%" set "EXE=%~dp0..\runtime\Build\x64-Clang-RelWithDebInfo\shadps4.exe"
if not exist "%ARGS%" (
  echo Launch options not found: %ARGS%
  echo In the game choose Settings ^> Graphics ^> Apply and Restart once first.
  pause
  exit /b 1
)
copy /y "%ARGS%" "%ARGS%.play" >nul
>>"%ARGS%" echo +nointro
>>"%ARGS%" echo +skipSplash=true
>>"%ARGS%" echo +boss=%BOSS%
cd /d "%~dp0"
rem Waits for the game to close, then puts the normal launch options back.
start "" /wait "%EXE%" --game "%GAME%"
move /y "%ARGS%.play" "%ARGS%" >nul

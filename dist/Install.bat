@echo off
rem Makes the program boot straight into the game and adds desktop and Start menu shortcuts.
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install.ps1" %*
pause

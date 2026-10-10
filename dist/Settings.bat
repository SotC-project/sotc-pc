@echo off
rem Opens the settings prompt (frame rate cap, VSync, fullscreen, window size).
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0Settings.ps1" %*
pause

@echo off
rem Free play: pick a colossus and options, then start in that battle.
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0FreePlay.ps1" %*
if errorlevel 1 pause

@echo off
rem ZeroTier One WOA installer (launcher)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
if errorlevel 1 pause

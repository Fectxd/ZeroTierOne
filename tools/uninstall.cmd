@echo off
rem ZeroTier One WOA uninstaller (launcher)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1"
if errorlevel 1 pause

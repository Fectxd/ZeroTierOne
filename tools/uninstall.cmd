@echo off
setlocal EnableExtensions
title ZeroTier One WOA Uninstaller

net session >nul 2>&1
if not %errorlevel%==0 goto :elevate
goto :main

:elevate
echo Requesting administrator privileges...
echo If a UAC prompt appears, click Yes.
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
if not %errorlevel%==0 (
    echo.
    echo Elevation failed or was cancelled.
    echo Please right-click uninstall.cmd and choose "Run as administrator".
)
echo.
pause
exit /b

:main
set "ZT_DIR=C:\ProgramData\ZeroTier\One"

echo [1/3] Stopping service...
net stop ZeroTierOneService >nul 2>&1

echo [2/3] Unregistering service and removing tap devices...
cd /d "%ZT_DIR%"
zerotier-one_arm64.exe -R
zerotier-one_arm64.exe -D

echo [3/3] Removing GUI shortcut and files...
if exist "C:\Program Files\ZeroTier\DesktopUI\zerotier_desktop_ui.exe" (
    del /Q "C:\Program Files\ZeroTier\DesktopUI\zerotier_desktop_ui.exe"
)
powershell -NoProfile -Command "Remove-Item ([Environment]::GetFolderPath('Desktop') + '\ZeroTier.lnk') -ErrorAction SilentlyContinue"

echo.
echo ================================================
echo   Uninstall done.
echo
echo   - Core files remain in C:\ProgramData\ZeroTier\One
echo     (delete that folder manually if you want them gone)
echo   - The TAP driver can be removed via Device Manager
echo     (Network adapters - ZeroTier) if desired.
echo ================================================
echo.
pause
endlocal

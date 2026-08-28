@echo off
setlocal EnableExtensions
title ZeroTier One WOA Installer

rem ============================================
rem  ZeroTier One - Windows on ARM64 (WOA)
rem  One-click installer
rem ============================================

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
    echo Please right-click install.cmd and choose "Run as administrator".
)
echo.
pause
exit /b

:main
set "SRC=%~dp0"
set "ZT_DIR=C:\ProgramData\ZeroTier\One"
set "GUI_DIR=C:\Program Files\ZeroTier\DesktopUI"

echo.
echo [1/6] Installing core binaries...
if not exist "%ZT_DIR%" mkdir "%ZT_DIR%"
copy /Y "%SRC%core\zerotier-one_arm64.exe" "%ZT_DIR%\" >nul
copy /Y "%SRC%core\zerotier-one_arm64.exe" "%ZT_DIR%\zerotier-cli.exe" >nul
copy /Y "%SRC%core\zerotier-one_arm64.exe" "%ZT_DIR%\zerotier-idtool.exe" >nul

echo [2/6] Installing TAP driver files...
if not exist "%ZT_DIR%\tap-windows\arm64" mkdir "%ZT_DIR%\tap-windows\arm64"
copy /Y "%SRC%tap-windows\arm64\zttap300.inf" "%ZT_DIR%\tap-windows\arm64\" >nul
copy /Y "%SRC%tap-windows\arm64\zttap300.sys" "%ZT_DIR%\tap-windows\arm64\" >nul
copy /Y "%SRC%tap-windows\arm64\zttap300.cat" "%ZT_DIR%\tap-windows\arm64\" >nul

echo [3/6] Installing signed TAP driver package...
pnputil /add-driver "%ZT_DIR%\tap-windows\arm64\zttap300.inf" /install
if errorlevel 1 (
    echo   [WARN] pnputil reported an issue (driver may already be installed).
)

echo [4/6] Registering ZeroTier service...
cd /d "%ZT_DIR%"
sc query ZeroTierOneService >nul 2>&1
if not errorlevel 1 (
    echo   Service already exists, skipping.
) else (
    zerotier-one_arm64.exe -I
    if errorlevel 1 (
        echo   [ERROR] Failed to install service. Make sure you are administrator.
    )
)

echo [5/6] Starting ZeroTier service...
net start ZeroTierOneService >nul 2>&1
sc query ZeroTierOneService | findstr /i "RUNNING" >nul
if errorlevel 1 (
    echo   [WARN] Service is not running yet. Check services.msc for ZeroTierOneService.
)

echo [6/6] Installing desktop GUI...
if not exist "%GUI_DIR%" mkdir "%GUI_DIR%"
copy /Y "%SRC%gui\zerotier_desktop_ui.exe" "%GUI_DIR%\" >nul
powershell -NoProfile -Command "$ws = New-Object -ComObject WScript.Shell; $p = Join-Path ([Environment]::GetFolderPath('Desktop')) 'ZeroTier.lnk'; $sc = $ws.CreateShortcut($p); $sc.TargetPath = 'C:\Program Files\ZeroTier\DesktopUI\zerotier_desktop_ui.exe'; $sc.WorkingDirectory = 'C:\Program Files\ZeroTier\DesktopUI'; $sc.Save()"

echo.
echo ================================================
echo   Installation complete!
echo
echo   - Service : ZeroTierOneService
echo   - GUI     : ZeroTier shortcut on your desktop
echo
echo   To join a network, open the ZeroTier GUI, or run:
echo     cd C:\ProgramData\ZeroTier\One
echo     zerotier-cli.exe join NETWORK-ID
echo ================================================
echo.
pause
endlocal

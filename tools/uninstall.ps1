# ZeroTier One WOA uninstaller (native ARM64) - PowerShell version
$Host.UI.RawUI.WindowTitle = 'ZeroTier One WOA Uninstaller'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Requesting administrator privileges..." -ForegroundColor Yellow
    try {
        Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File', "`"$PSCommandPath`"") -Verb RunAs
    } catch {
        Write-Host "Elevation failed or was cancelled: $($_.Exception.Message)" -ForegroundColor Red
    }
    Read-Host "`nPress Enter to exit"
    exit
}

$ZT_DIR = 'C:\ProgramData\ZeroTier\One'
$GUI_DIR = 'C:\Program Files\ZeroTier\DesktopUI'

Write-Host "`n=== [1/3] Stopping service ===" -ForegroundColor Cyan
Stop-Service -Name 'ZeroTierOneService' -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
Write-Host "  done"

Write-Host "=== [2/3] Unregistering service and removing tap devices ===" -ForegroundColor Cyan
Push-Location $ZT_DIR
& .\zerotier-one_arm64.exe -R
& .\zerotier-one_arm64.exe -D
Pop-Location
Write-Host "  done"

Write-Host "=== [3/3] Removing GUI ===" -ForegroundColor Cyan
Remove-Item -Path (Join-Path $GUI_DIR 'zerotier_desktop_ui.exe') -Force -ErrorAction SilentlyContinue
Remove-Item -Path (Join-Path ([Environment]::GetFolderPath('Desktop')) 'ZeroTier.lnk') -Force -ErrorAction SilentlyContinue
Write-Host "  done"

Write-Host "`n================================================"
Write-Host "  Uninstall done."
Write-Host "  - Core files remain in C:\ProgramData\ZeroTier\One"
Write-Host "    (delete that folder manually if you want them gone)"
Write-Host "  - The TAP driver can be removed via Device Manager"
Write-Host "    (Network adapters - ZeroTier) if desired."
Write-Host "================================================"
Write-Host ""
Read-Host "Press Enter to exit"
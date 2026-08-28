# ZeroTier One WOA installer (native ARM64) - PowerShell version
$Host.UI.RawUI.WindowTitle = 'ZeroTier One WOA Installer'

# --- self-elevate ---
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Requesting administrator privileges..." -ForegroundColor Yellow
    Write-Host "If a UAC prompt appears, click Yes."
    try {
        Start-Process -FilePath 'powershell.exe' -ArgumentList @('-NoProfile','-ExecutionPolicy','Bypass','-File', "`"$PSCommandPath`"") -Verb RunAs
    } catch {
        Write-Host "Elevation failed or was cancelled: $($_.Exception.Message)" -ForegroundColor Red
        Write-Host "Please right-click install.ps1 -> Run with PowerShell, as administrator."
    }
    Read-Host "`nPress Enter to exit"
    exit
}

$SRC    = Split-Path -Parent $MyInvocation.MyCommand.Path
$ZT_DIR = 'C:\ProgramData\ZeroTier\One'
$GUI_DIR = 'C:\Program Files\ZeroTier\DesktopUI'
$ok = 0; $fail = 0

Write-Host "`n=== [1/7] Core binaries ===" -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $ZT_DIR | Out-Null
foreach ($n in @('zerotier-one_arm64.exe','zerotier-cli.exe','zerotier-idtool.exe')) {
    try { Copy-Item -Path (Join-Path $SRC 'core\zerotier-one_arm64.exe') -Destination (Join-Path $ZT_DIR $n) -Force -ErrorAction Stop; Write-Host "  [OK] $n" -ForegroundColor Green; $ok++ }
    catch { Write-Host "  [FAIL] $n : $($_.Exception.Message)" -ForegroundColor Red; $fail++ }
}

Write-Host "=== [2/7] TAP driver files ===" -ForegroundColor Cyan
$drvDir = Join-Path $ZT_DIR 'tap-windows\arm64'
New-Item -ItemType Directory -Force -Path $drvDir | Out-Null
try { Copy-Item -Path (Join-Path $SRC 'tap-windows\arm64\*') -Destination $drvDir -Force -ErrorAction Stop; Write-Host "  [OK] driver files" -ForegroundColor Green; $ok++ }
catch { Write-Host "  [FAIL] driver files : $($_.Exception.Message)" -ForegroundColor Red; $fail++ }

Write-Host "=== [3/7] Register TAP driver (pnputil) ===" -ForegroundColor Cyan
& pnputil /add-driver (Join-Path $drvDir 'zttap300.inf') /install
if ($LASTEXITCODE -eq 0 -or $LASTEXITCODE -eq 3010) { Write-Host "  [OK] driver registered (rc=$LASTEXITCODE)" -ForegroundColor Green; $ok++ }
else { Write-Host "  [FAIL] pnputil rc=$LASTEXITCODE (may already be installed)" -ForegroundColor Red; $fail++ }

Write-Host "=== [4/7] Register service ===" -ForegroundColor Cyan
Push-Location $ZT_DIR
$svc = Get-Service -Name 'ZeroTierOneService' -ErrorAction SilentlyContinue
if ($svc) {
    Write-Host "  Service exists - skipping." -ForegroundColor Yellow
    $p = (Get-CimInstance Win32_Service -Filter "Name='ZeroTierOneService'").PathName
    Write-Host "  Service binary: $p" -ForegroundColor Yellow
} else {
    & .\zerotier-one_arm64.exe -I
    if ($LASTEXITCODE -eq 0) { Write-Host "  [OK] service installed" -ForegroundColor Green; $ok++ }
    else { Write-Host "  [FAIL] service install rc=$LASTEXITCODE" -ForegroundColor Red; $fail++ }
}
Pop-Location

Write-Host "=== [5/7] Start service ===" -ForegroundColor Cyan
Start-Service -Name 'ZeroTierOneService' -ErrorAction SilentlyContinue
Start-Sleep -Seconds 3
$st = (Get-Service -Name 'ZeroTierOneService' -ErrorAction SilentlyContinue).Status
if ($st -eq 'Running') { Write-Host "  [OK] service running" -ForegroundColor Green; $ok++ }
else { Write-Host "  [FAIL] service status: $st" -ForegroundColor Red; $fail++ }

Write-Host "=== [6/7] GUI ===" -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path $GUI_DIR | Out-Null
try { Copy-Item -Path (Join-Path $SRC 'gui\zerotier_desktop_ui.exe') -Destination $GUI_DIR -Force -ErrorAction Stop; Write-Host "  [OK] gui copied" -ForegroundColor Green; $ok++ }
catch { Write-Host "  [FAIL] gui copy : $($_.Exception.Message)" -ForegroundColor Red; $fail++ }
try {
    $ws = New-Object -ComObject WScript.Shell
    $lnk = $ws.CreateShortcut((Join-Path ([Environment]::GetFolderPath('Desktop')) 'ZeroTier.lnk'))
    $lnk.TargetPath = Join-Path $GUI_DIR 'zerotier_desktop_ui.exe'
    $lnk.WorkingDirectory = $GUI_DIR
    $lnk.Save()
} catch { Write-Host "  [WARN] shortcut: $($_.Exception.Message)" -ForegroundColor Yellow }

Write-Host "`n=== [7/7] Summary ===" -ForegroundColor Cyan
Write-Host "  OK: $ok   FAIL: $fail"
if ($fail -gt 0) { Write-Host "  Some steps failed - see messages above." -ForegroundColor Red }
else { Write-Host "  All steps succeeded. ZeroTier is installed and running." -ForegroundColor Green }
Write-Host ""
Read-Host "Press Enter to exit"
<#
.SYNOPSIS
    Build & install the pinned, header-only OpenTelemetry C++ API into a prefix
    that the CMake build finds via -DCMAKE_PREFIX_PATH. The Windows analog of
    scripts/bootstrap-deps.sh (its api-only mode).

.DESCRIPTION
    The Windows build is daemon-only -- the Central Controller is NOT supported on
    Windows -- so it needs only the *header-only* OpenTelemetry API, not the SDK,
    the OTLP exporters, or google-cloud-cpp. The other dependencies come from:
        * vcpkg  : openssl, nlohmann-json   (vcpkg.json + CMakePresets.json; vcpkg
                   installs them automatically at configure time)
        * CMake  : inja, cpp-httplib, miniupnpc/natpmp  (FetchContent)
    This script provides the one remaining dependency: opentelemetry-cpp (API
    only), pinned to the SAME version as the Linux/macOS bootstrap so that
    find_package(opentelemetry-cpp COMPONENTS api) resolves identically on every
    platform.

    Prerequisites: git, CMake, and Visual Studio 2022 (for the CMake compiler
    probe) on PATH; vcpkg installed with VCPKG_ROOT set (for the build itself).

.PARAMETER Prefix
    Install prefix (default: <repo>\.deps). Pass the same path to CMake via
    -DCMAKE_PREFIX_PATH (the windows-* CMake presets already point at <repo>\.deps).

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\bootstrap-deps.ps1
    cmake --preset windows-x64
    cmake --build --preset windows-x64-release
#>
[CmdletBinding()]
param(
    [string]$Prefix
)
$ErrorActionPreference = 'Stop'

# Keep in sync with OTEL_VERSION in scripts/bootstrap-deps.sh.
$OtelVersion = 'v1.27.0'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Prefix) { $Prefix = Join-Path $RepoRoot '.deps' }
$BuildRoot = Join-Path $RepoRoot '.deps-build'
$OtelSrc   = Join-Path $BuildRoot 'opentelemetry-cpp-src'
$OtelBuild = Join-Path $BuildRoot 'otel'

New-Item -ItemType Directory -Force -Path $Prefix, $BuildRoot | Out-Null
Write-Host "==> Bootstrapping pinned deps into: $Prefix"
if (-not $env:VCPKG_ROOT) {
    Write-Warning "VCPKG_ROOT is not set. openssl/nlohmann-json come from vcpkg at configure time; set VCPKG_ROOT before running CMake."
}

# --- opentelemetry-cpp (API only, header-only) --------------------------------
if ((Test-Path (Join-Path $Prefix 'lib\cmake\opentelemetry-cpp')) -or
    (Test-Path (Join-Path $Prefix 'lib64\cmake\opentelemetry-cpp'))) {
    Write-Host "==> opentelemetry-cpp already installed in prefix, skipping"
    Write-Host "    (delete $Prefix to rebuild)"
} else {
    # Re-clone if the existing checkout isn't the pinned tag (handles a version bump).
    if (Test-Path (Join-Path $OtelSrc '.git')) {
        $tag = (git -C $OtelSrc describe --tags --exact-match 2>$null)
        if ($tag -ne $OtelVersion) {
            Write-Host "==> opentelemetry-cpp clone is not $OtelVersion; re-fetching"
            Remove-Item -Recurse -Force $OtelSrc
        }
    }
    if (-not (Test-Path (Join-Path $OtelSrc '.git'))) {
        Write-Host "==> Fetching opentelemetry-cpp $OtelVersion"
        git clone --depth 1 --branch $OtelVersion `
            https://github.com/open-telemetry/opentelemetry-cpp.git $OtelSrc
        if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
    }

    # Header-only API: quick, no SDK/exporters, no abseil/protobuf/grpc.
    Write-Host "==> Building opentelemetry-cpp (API only)"
    cmake -S $OtelSrc -B $OtelBuild `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_INSTALL_PREFIX="$Prefix" `
        -DBUILD_TESTING=OFF `
        -DOPENTELEMETRY_INSTALL=ON `
        -DWITH_API_ONLY=ON
    if ($LASTEXITCODE -ne 0) { throw "cmake configure (otel) failed" }
    cmake --build $OtelBuild --config Release
    if ($LASTEXITCODE -ne 0) { throw "cmake build (otel) failed" }
    cmake --install $OtelBuild --config Release
    if ($LASTEXITCODE -ne 0) { throw "cmake install (otel) failed" }
}

Write-Host ""
Write-Host "==> Done. Build the Windows daemon with:"
Write-Host "    cmake --preset windows-x64"
Write-Host "    cmake --build --preset windows-x64-release"
Write-Host "(the windows-* presets set CMAKE_PREFIX_PATH to <repo>\.deps and the vcpkg toolchain)"

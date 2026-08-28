ZeroTier One - Native Windows on ARM64 (WOA) build
==================================================

Contents
  core/                         Native ARM64 ZeroTier binaries
    zerotier-one_arm64.exe      The core service binary (install with -I)
    zerotier-cli.exe            Command line client
    zerotier-idtool.exe         Identity tool
  tap-windows/arm64-microsoft-signed/
                                Microsoft-signed ARM64 TAP driver (zttap300.sys/.inf/.cat)
                                Ready to install, no test mode needed
  tap-windows/arm64-selfbuilt/  Driver built from current source (NOT signed)

Installation (run an elevated PowerShell / cmd as Administrator)

1) Install the service:
     copy core\* "C:\ProgramData\ZeroTier\One\"
     cd "C:\ProgramData\ZeroTier\One"
     zerotier-one_arm64.exe -I
   (This installs the ZeroTier service but does not start it yet)

2) Install the TAP driver (pick ONE option):

   Option A (recommended - Microsoft-signed, no test mode):
     Copy the files from tap-windows\arm64-microsoft-signed to
     "C:\ProgramData\ZeroTier\One\tap-windows\arm64\", then right-click
     zttap300.inf -> Install.  (or use: pnputil /add-driver zttap300.inf /install)

   Option B (self-built driver - requires test signing mode):
     bcdedit /set testsigning on   (reboot)
     Copy tap-windows\arm64-selfbuilt\* to "C:\ProgramData\ZeroTier\One\tap-windows\arm64\"
     Right-click zttap300.inf -> Install, or pnputil /add-driver zttap300.inf /install

3) Start the service and join a network:
     net start ZeroTierOne
     zerotier-cli join <network-id>
     zerotier-cli listnetworks

Build info: native aarch64-pc-windows-msvc, built on GitHub Actions windows-11-arm runner.

ZeroTier One 原生 Windows on ARM64 (WOA) 一键安装包
===================================================

适用设备：Windows 11 on ARM（骁龙 X 系列 / Surface Pro X / Pro 11 等 ARM64 设备）
版本：ZeroTier One 1.16.2（dev 分支，原生 ARM64 编译，非模拟运行）

包含内容：
  core/                   原生 ARM64 ZeroTier 核心 zerotier-one_arm64.exe
  tap-windows/arm64/      微软签名的 ARM64 TAP 驱动（zttap300，无需测试模式）
  gui/                    原生 ARM64 桌面 GUI（托盘应用 zerotier_desktop_ui.exe）
  install.cmd             一键安装脚本（自动提权）
  uninstall.cmd           卸载脚本（自动提权）

使用方法：
  1. 解压到任意目录
  2. 双击 install.cmd（会弹出 UAC 提权，点"是"；若被杀软拦截请允许）
     如果双击后窗口一闪而过，请右键 install.cmd -> 以管理员身份运行
  3. 看到 "Installation complete" 即安装完成，桌面出现 ZeroTier 快捷方式
  4. 双击桌面 ZeroTier 图标打开 GUI，或命令行加入网络：
       cd C:\ProgramData\ZeroTier\One
       zerotier-cli.exe join <网络ID>
       zerotier-cli.exe listnetworks

安装内容：
  - 核心程序 -> C:\ProgramData\ZeroTier\One\（含 cli/idtool 副本）
  - 驱动     -> C:\ProgramData\ZeroTier\One\tap-windows\arm64\（pnputil 注册）
  - 服务     -> ZeroTierOneService（开机自启）
  - GUI      -> C:\Program Files\ZeroTier\DesktopUI\

卸载：
  双击 uninstall.cmd，或手动：
    net stop ZeroTierOneService
    cd C:\ProgramData\ZeroTier\One && zerotier-one_arm64.exe -R
    zerotier-one_arm64.exe -D

常见问题：
  - 驱动加载失败/未签名提示：本包驱动为微软签名（ZeroTier 社区 PR #1949 提供，
    2023 年签署，覆盖 Win10/11 ARM），正常情况下无需开启测试模式。
  - 加入网络后没有 IP：登录 ZeroTier 控制台（my.zerotier.com）确认该设备已被允许入网
    （勾选 Auth），或检查网络是否有 DHCP。
  - 服务启动失败：以管理员运行 services.msc，找到 ZeroTierOneService 查看错误日志。

构建信息：
  - 核心：Fectxd/ZeroTierOne 分支 woa-build（GitHub Actions windows-11-arm runner）
  - GUI ：Fectxd/DesktopUI 分支 woa-build（同上）
  - 全部为原生 aarch64-pc-windows-msvc 编译，PE 架构已验证 ARM64

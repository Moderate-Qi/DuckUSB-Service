# DuckUSB Service 服务增强版 · 2.1.7-service.2

下载文件：`DuckUSB-Service-2.1.7-service.2-arm64.apk`

在 DuckUSB 原有功能基础上，加入服务侧 Socket 管理和 USB 属性配套处理。在保持日常 USB 调试功能的同时，进一步完善 ADB 隐藏效果。

当前版本：**2.1.7-service.2**，ARM64 APK。增强维护：[Moderate-Qi](https://github.com/Moderate-Qi)。

## 相比原版增加

- **服务侧 Socket 隐藏开关。** 配套处理 ADB 授权通道与应用断点调试监听，改善 ADB 运行状态和相关 Socket 的隐藏效果。
- **补全 USB 属性管理。** 在匹配的高通 Gadget HAL 路径中加入 `sys.usb.config` 处理，并保留厂商 USB 控制值与文件传输功能。
- **保留常用 ADB 命令。** 日常继续使用 `adb push`、`adb pull`、shell、应用安装和 logcat。
- **兼顾 MT 管理器。** Root 终端、日志导出和本机排查功能继续使用。
- **系统框架统一作用。** 在 LSPosed 中选择系统框架作用域，即可向应用提供配套的服务侧结果。
- **状态直接可见。** 界面展示属性和 Socket helper 状态，支持保存开关、开机恢复和 ADB 服务重启后的重新处理。
- **独立管理与恢复。** 属性与 Socket 各有开关，方便按需调整。

## 使用方式

1. 从本仓库 Releases 下载 `DuckUSB-Service-2.1.7-service.2-arm64.apk`，安装配套 APK。
2. 在 LSPosed 中启用模块，选择“系统框架”，然后重启手机。
3. 进入 Behaviour 页面，按需启用调试状态、查询路径、通知、USB 属性和服务侧 Socket 开关。
4. 查看状态区域：本次实测属性显示 `ACTIVE HAL`，Socket helper 显示 `ACTIVE`。
5. 更换模块或调整 Socket 功能时，先关闭服务侧开关，等待状态显示 `STOPPED`，再进行后续操作。

# DuckUSB Service · USB 属性使用说明

基于版本：**2.1.7-service.2**，ARM64 APK，包名 `com.strawing.duckusb.servicehide`。增强维护：[Moderate-Qi](https://github.com/Moderate-Qi)。

## 功能

- 配套管理 `persist.sys.usb.config`、`sys.usb.state` 与 `init.svc.adbd`。
- 高通 Gadget HAL 路径增加 `sys.usb.config` 处理，保留厂商 USB 控制值。
- USB 配置交接并稳定后自动同步；USB 重连、功能切换和 ADB 重启后继续处理。
- 属性与 Socket 分别管理，支持保存开关、开机恢复和独立恢复脚本。

## 更新与开启

1. 覆盖安装 `DuckUSB-Service-2.1.7-service.2-arm64.apk`，沿用应用数据与原签名。
2. 打开应用，完成 Root 授权，在 Behaviour 启用 `Mask the USB config property`。
3. 等待配置同步，在 Diagnostics 查看 `property helper` 状态。
4. LSPosed 选择“系统框架”，Socket 开关按日常设置使用。

| 状态 | 含义 |
| --- | --- |
| `ACTIVE HAL` | 高通 Gadget HAL 属性处理运行中 |
| `WAITING HAL SETTLE` | 等待 USB 配置交接与稳定 |
| `ACTIVE BASE (HAL route unavailable)` | 原有三项属性处理运行中 |

开机完成约 30 秒后恢复已启用的属性服务，再按 USB 配置状态完成同步。

## 查看状态

在 MT 管理器 Root 终端运行：

```sh
getprop sys.usb.config
getprop persist.sys.usb.config
getprop sys.usb.state
getprop init.svc.adbd
getprop vendor.usb.config
cat /data/adb/duckusb-properties/status
```

实测 HAL 路径下，前四项依次为 `mtp`、`mtp`、`mtp`、`stopped`。`vendor.usb.config` 保留厂商 USB 配置。

## 关闭与恢复

关闭 Behaviour 中的属性开关，服务退出并恢复对应属性。MT 管理器 Root 终端也可运行：

```sh
sh /data/adb/duckusb-properties/restore.sh
```

Socket 功能通过独立开关或 `/data/adb/duckusb-service/restore.sh` 管理。

## 实测记录

Risk Detector 1.6 显示“本地检测已通过”。电脑端 `adb push` / `adb pull`、shell、logcat 与 MT 管理器日志导出继续使用。

完整截图见 [README.md](README.md)，Socket 设置见 [README_SERVICE_CN.md](README_SERVICE_CN.md)，构建方式见 [BUILDING.md](BUILDING.md)。

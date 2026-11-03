# DuckUSB Service · Socket 使用说明

基于版本：**2.1.7**。增强维护：[Moderate-Qi](https://github.com/Moderate-Qi)。

## 功能

- 服务侧统一处理 ADB 授权通道与应用断点调试监听。
- LSPosed 选择“系统框架”作用域，服务侧结果向应用统一生效。
- 保留 `adb push`、`adb pull`、shell、应用安装和 logcat，兼顾 MT 管理器 Root 终端。
- 独立开关保存设置，开机恢复，ADB 服务重启后自动重新处理。

## 开启

1. 安装 `DuckUSB-Service-2.1.7-service.2-arm64.apk`。
2. 在 LSPosed 启用 DuckUSB Service，选择“系统框架”，然后重启。
3. 打开应用，完成 Root 授权。
4. 在 Behaviour 打开服务侧 Socket 开关，在 Diagnostics 查看 `auth redirect ready = true` 与 helper 的 `ACTIVE` 状态。

Socket 功能运行期间，应用断点调试监听关闭，文件传输与常用命令保持正常。Settings、USB 属性和 Socket 功能通过各自开关管理。

## 关闭与恢复

关闭 Socket 开关，等待 `STOPPED`。更换 LSPosed 模块时，先完成服务恢复。

MT 管理器 Root 终端恢复命令：

```sh
su
sh /data/adb/duckusb-service/restore.sh
```

恢复脚本在首次开启服务时生成，用于恢复授权 Socket 路径与 JDWP 监听。

# 

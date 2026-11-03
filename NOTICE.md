# 来源与版权

| 项目     | 来源与授权                                                                                                                            |
| ------ | -------------------------------------------------------------------------------------------------------------------------------- |
| 上游代码   | [Bouteillepleine/DuckUSB](https://github.com/Bouteillepleine/DuckUSB)，基准提交 `bf31eb6db00f1dee92f3e51202fa9bc4e75765e4`，版权归原作者与贡献者 |
| 独立新增文件 | Moderate-Qi，GPL-3.0-only，许可证全文见 [LICENSE-LOCAL.md](LICENSE-LOCAL.md)                                                             |
| 第三方组件  | LSPosed API、LSPlant、Dobby 等组件沿用各自许可证和版权记录                                                                                        |

独立新增文件包括 `service-helper.c`、`property-helper.c`、`build-service-helper.py`、`ServiceAuthState.kt`、`ServiceSocketTools.kt` 及增强版文档。

GPL-3.0-only 授权范围为上述独立新增文件。

Service APK 使用 app、ui、common 模块。签名私钥保存在本地，功能修改见 [CHANGELOG_LOCAL.md](CHANGELOG_LOCAL.md)，构建方式见 [BUILDING.md](BUILDING.md)。

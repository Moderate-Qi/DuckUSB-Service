# 构建与发行

基准提交与增强版本见 `publication/UPSTREAM.json`。以下命令在仓库根目录运行；Windows 使用 `gradlew.bat`，Linux/macOS 使用 `./gradlew`。

获取子模块：

```sh
git submodule update --init --recursive
```

## DuckUSB Service 2.1.7-service.2

环境：JDK 21，Android SDK 36，NDK r27c，Python 3。设置 `ANDROID_NDK_HOME` 或 `NDK_TOOLCHAIN`。两个 helper 的源码为根目录 service-helper.c 和 property-helper.c，生成 ARM64 assets。

```sh
python build-service-helper.py
./gradlew :app:assembleRelease
```

按 app/build.gradle.kts 读取的 key.properties 或 DUCKUSB_STORE_* 环境变量提供本地原签名，发行 APK 使用 2.1.7-service.2 的应用 ID 和版本。当前 Service 版本构建包含 app、ui、common，产物位于 app/build/outputs/apk/release。

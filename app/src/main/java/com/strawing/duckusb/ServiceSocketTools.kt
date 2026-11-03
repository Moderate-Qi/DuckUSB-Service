package com.strawing.duckusb

import android.content.Context
import android.content.SharedPreferences
import com.topjohnwu.superuser.Shell
import java.io.File
import java.util.UUID

/** Out-of-process root helper. This class never injects native code into other apps. */
object ServiceSocketTools {
    private const val BASE = "/data/adb/duckusb-service"
    private const val BINARY = "$BASE/service-helper-v1"
    private const val BOOT = "/data/adb/service.d/duckusb-service.sh"

    private fun quote(value: String) = "'" + value.replace("'", "'\\''") + "'"
    private fun exec(script: String): Shell.Result = Shell.cmd(script).exec()

    fun status(): String = runCatching {
        exec("cat $BASE/status 2>/dev/null").out.joinToString(" ").ifBlank { "未运行" }
    }.getOrDefault("读取状态失败")

    fun enable(context: Context): String {
        check(Shell.getShell().isRoot) { "需要授予本应用 root" }
        val prefs = checkNotNull(DuckApp.service) { "等待 LSPosed 配置服务连接后重试" }
            .getRemotePreferences(Config.PREFS_NAME)
        // The SettingsProvider bridge confirms that this module is live in system_server.
        check(com.strawing.duckusb.service.DuckServiceClient.get(context)?.state?.getBoolean("authRedirectReady") == true) {
            "先在 LSPosed 启用本实验模块，只勾选系统框架，再重启"
        }
        val endpoint = prefs.getString(Config.KEY_AUTH_ENDPOINT, null)
            ?: "/dev/socket/.dusb-${UUID.randomUUID().toString().substring(0, 12)}/adbd".also {
                prefs.edit().putString(Config.KEY_AUTH_ENDPOINT, it).commit()
            }
        val binary = File(context.filesDir, "service-helper-v1")
        context.assets.open("arm64-v8a/service-helper").use { input ->
            binary.outputStream().use { output -> input.copyTo(output) }
        }
        val stopped = disable(prefs)
        check(!stopped.startsWith("RESTORE_FAILED")) { stopped }
        val ready = exec("""
            mkdir -p $BASE /data/adb/service.d
            cp ${quote(binary.absolutePath)} $BINARY
            chmod 0755 $BINARY
            printf '0\n' > $BASE/enabled
        """.trimIndent())
        check(ready.isSuccess) { "写入 root helper 失败：${ready.out.joinToString(" ")}" }
        // Publish the server redirect before moving its authentication socket.
        prefs.edit().putBoolean(Config.KEY_SERVICE_SOCKETS, true).commit()
        val script = """
            cat > $BASE/restore.sh <<'DUCKUSB_SERVICE_RESTORE'
            #!/system/bin/sh
            printf '0\n' > $BASE/enabled
            sleep 2
            $BINARY restore ${quote(endpoint)}
            DUCKUSB_SERVICE_RESTORE
            chmod 0755 $BASE/restore.sh
            cat > $BOOT <<'DUCKUSB_SERVICE_BOOT'
            #!/system/bin/sh
            until [ "${'$'}(getprop sys.boot_completed)" = "1" ]; do sleep 1; done
            sleep 15
            [ "${'$'}(cat $BASE/enabled 2>/dev/null)" = "1" ] || exit 0
            $BINARY restore ${quote(endpoint)} >> $BASE/service.log 2>&1
            nohup $BINARY watch ${quote(endpoint)} >> $BASE/service.log 2>&1 < /dev/null &
            DUCKUSB_SERVICE_BOOT
            chmod 0755 $BOOT
            printf '1\n' > $BASE/enabled
            nohup $BINARY watch ${quote(endpoint)} >> $BASE/service.log 2>&1 < /dev/null &
        """.trimIndent()
        check(exec(script).isSuccess) { "启动 root helper 失败" }
        repeat(8) {
            Thread.sleep(400)
            val state = status()
            if (state.startsWith("ACTIVE")) return state
            if (state.contains("FAILED")) {
                disable(prefs)
                error(state)
            }
        }
        disable(prefs)
        error("helper 未进入 ACTIVE；已请求恢复。请查看 $BASE/service.log")
    }

    fun disable(prefs: SharedPreferences): String {
        check(Shell.getShell().isRoot) { "需要 root 才能恢复服务；服务端重定向保持开启" }
        val endpoint = prefs.getString(Config.KEY_AUTH_ENDPOINT, null)
        check(exec("[ ! -d $BASE ] || printf '0\n' > $BASE/enabled").isSuccess) { "停止标志写入失败" }
        Thread.sleep(1400)
        // Also recovers a previous helper that exited abnormally; no files are removed.
        if (endpoint != null) exec("[ ! -x $BINARY ] || $BINARY restore ${quote(endpoint)}")
        val state = status()
        check(!state.startsWith("ACTIVE") && !state.startsWith("RESTORE_FAILED")) { state }
        prefs.edit().putBoolean(Config.KEY_SERVICE_SOCKETS, false).commit()
        return state
    }
}

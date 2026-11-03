package com.strawing.duckusb

import android.content.Context
import java.io.File
import com.topjohnwu.superuser.Shell

object RootTools {

    const val BOOT_SCRIPT = "/data/adb/service.d/duckusb-usbprop.sh"
    const val USB_PROP = "persist.sys.usb.config"
    const val USB_SAFE = "mtp"
    const val ADBD_PROP = "init.svc.adbd"
    const val ADBD_SAFE = "stopped"

    private const val BASE = "/data/adb/duckusb-properties"
    private const val BINARY = "$BASE/property-helper-v2"
    private const val SCRIPT_VERSION = "DuckUSB property service.2"

    private fun quote(value: String) = "'" + value.replace("'", "'\\''") + "'"

    init {
        Shell.enableVerboseLogging = false
        Shell.setDefaultBuilder(
            Shell.Builder.create()
                .setFlags(Shell.FLAG_MOUNT_MASTER or Shell.FLAG_REDIRECT_STDERR)
                .setTimeout(20)
        )
    }

    class Snapshot(
        val rootAvailable: Boolean,
        val settings: Map<String, String>,
        val props: Map<String, String>,
        val propMaskInstalled: Boolean,
    )

    fun warm() {
        runCatching { Shell.getShell() }
    }

    fun available(): Boolean = try {
        Shell.getShell().isRoot
    } catch (_: Throwable) {
        false
    }

    fun snapshot(settingKeys: List<String>, propKeys: List<String>): Snapshot {
        val script = buildString {
            append("if [ -f $BASE/enabled ]; then echo mask=\$(cat $BASE/enabled); elif [ -f $BOOT_SCRIPT ]; then echo mask=1; else echo mask=0; fi\n")
            append("echo '#settings'\n")
            append("PATTERN='^(")
            append(settingKeys.joinToString("|"))
            append(")='\n")
            append("cmd settings list global 2>/dev/null | grep -E \"\$PATTERN\" || ")
            append("settings list global 2>/dev/null | grep -E \"\$PATTERN\"\n")
            append("echo '#props'\n")
            for (key in propKeys) append("echo '$key='\$(getprop $key)\n")
        }
        val rooted = try { Shell.getShell().isRoot } catch (_: Throwable) { false }
        val result = exec(script)
        if (!result.isSuccess && result.out.isEmpty()) {
            return Snapshot(false, emptyMap(), emptyMap(), false)
        }
        val settings = LinkedHashMap<String, String>()
        val props = LinkedHashMap<String, String>()
        var masked = false
        var section = ""
        for (raw in result.out) {
            val line = raw.trim()
            if (line.startsWith("#")) {
                section = line
                continue
            }
            when (section) {
                "#settings" -> line.split("=", limit = 2).let { if (it.size == 2) settings[it[0]] = it[1] }
                "#props" -> line.split("=", limit = 2).let { if (it.size == 2) props[it[0]] = it[1] }
                else -> if (line == "mask=1") masked = true
            }
        }
        return Snapshot(rooted, settings, props, masked)
    }

    /** Upgrade the existing property installation when this APK is opened. */
    @Synchronized
    fun upgradePropMask(context: Context): Boolean {
        val result = exec("""
            [ -f $BOOT_SCRIPT ] || exit 0
            [ -f $BASE/enabled ] && [ "${'$'}(cat $BASE/enabled)" != "1" ] && exit 0
            grep -q '$SCRIPT_VERSION' $BOOT_SCRIPT && [ -x $BINARY ] && exit 0
            echo upgrade
        """.trimIndent())
        return if (result.out.any { it.trim() == "upgrade" }) setPropMask(context, true) else result.isSuccess
    }

    fun propStatus(): String = exec("cat $BASE/status 2>/dev/null").out.joinToString(" ").ifBlank { "尚待启用" }

    private val locateResetprop = """
        RP=
        for candidate in /data/adb/ksu/bin/resetprop /data/adb/ap/bin/resetprop /data/adb/magisk/resetprop; do
            [ -x "${'$'}candidate" ] && RP="${'$'}candidate" && break
        done
        [ -z "${'$'}RP" ] && RP=${'$'}(command -v resetprop 2>/dev/null)
        [ -n "${'$'}RP" ] || exit 1
    """.trimIndent()

    /** Stop the service.1 shell loop, matching its complete script argument. */
    private val stopLegacy = """
        ps -A -o PID,ARGS | while read pid arg1 arg2 rest; do
            case "${'$'}arg1" in
                sh|/system/bin/sh|/system/bin/ash|bash|/system/bin/bash)
                    [ "${'$'}arg2" = "$BOOT_SCRIPT" ] && kill -TERM "${'$'}pid" 2>/dev/null
                    ;;
                $BOOT_SCRIPT) kill -TERM "${'$'}pid" 2>/dev/null ;;
            esac
        done
        :
    """.trimIndent()

    /** Enabled flags and a non-blocking native lock replace script deletion. */
    @Synchronized
    fun setPropMask(context: Context, enabled: Boolean): Boolean {
        if (!available()) return false
        val prepared = exec("""
            if [ -f $BOOT_SCRIPT ] && ! grep -q '$SCRIPT_VERSION' $BOOT_SCRIPT; then echo legacy; fi
            mkdir -p $BASE /data/adb/service.d
            chmod 0700 $BASE
            printf '0\n' > $BASE/enabled
            ${stopLegacy.prependIndent("            ").trimStart()}
        """.trimIndent())
        if (!prepared.isSuccess) return false
        val legacy = prepared.out.any { it.trim() == "legacy" }
        val stopped = exec("""
            ${locateResetprop.prependIndent("            ").trimStart()}
            if [ -x $BINARY ]; then
                n=0
                until $BINARY restore "${'$'}RP"; do
                    n=${'$'}((n + 1))
                    [ "${'$'}n" -lt 6 ] || exit 1
                    sleep 1
                done
            fi
        """.trimIndent()).isSuccess
        if (!stopped) return false
        // Copy only after the running property helper has released its lock.
        val binary = File(context.filesDir, "property-helper-v2")
        context.assets.open("arm64-v8a/property-helper").use { input ->
            binary.outputStream().use { output -> input.copyTo(output) }
        }
        val written = exec("""
            umask 077
            cp ${quote(binary.absolutePath)} $BINARY || exit 1
            chmod 0700 $BINARY || exit 1
            cat > $BOOT_SCRIPT <<'DUCKUSB_PROP_BOOT'
            #!/system/bin/sh
            # $SCRIPT_VERSION
            [ "${'$'}(cat $BASE/enabled 2>/dev/null)" = "1" ] || exit 0
            (
                until [ "${'$'}(getprop sys.boot_completed)" = "1" ]; do sleep 1; done
                sleep 30
                [ "${'$'}(cat $BASE/enabled 2>/dev/null)" = "1" ] || exit 0
                ${locateResetprop.prependIndent("            ").trimStart()}
                nohup $BINARY watch "${'$'}RP" >> $BASE/service.log 2>&1 < /dev/null &
            ) < /dev/null > /dev/null 2>&1 &
            DUCKUSB_PROP_BOOT
            chmod 0755 $BOOT_SCRIPT
            cat > $BASE/restore.sh <<'DUCKUSB_PROP_RESTORE'
            #!/system/bin/sh
            printf '0\n' > $BASE/enabled
            ${locateResetprop.prependIndent("            ").trimStart()}
            n=0
            until $BINARY restore "${'$'}RP"; do
                n=${'$'}((n + 1))
                [ "${'$'}n" -lt 6 ] || exit 1
                sleep 1
            done
            DUCKUSB_PROP_RESTORE
            chmod 0700 $BASE/restore.sh
        """.trimIndent()).isSuccess
        if (!written) return false
        if (!enabled) return if (legacy) exec("$locateResetprop\n$BINARY restore-legacy \"\$RP\"").isSuccess else true
        if (!exec("""
            ${locateResetprop.prependIndent("            ").trimStart()}
            printf 'STARTING\n' > $BASE/status
            printf '1\n' > $BASE/enabled
            nohup $BINARY watch "${'$'}RP" >> $BASE/service.log 2>&1 < /dev/null &
        """.trimIndent()).isSuccess) return false
        repeat(14) {
            Thread.sleep(250)
            val status = propStatus()
            if (status.startsWith("ACTIVE") || status.startsWith("WAITING")) return true
            if (status.contains("FAILED")) {
                exec("printf '0\\n' > $BASE/enabled")
                return false
            }
        }
        return false
    }

    private fun exec(vararg commands: String): Shell.Result = try {
        Shell.cmd(*commands).exec()
    } catch (_: Throwable) {
        FAILED
    }

    private val FAILED = object : Shell.Result() {
        override fun getOut(): MutableList<String> = ArrayList()
        override fun getErr(): MutableList<String> = ArrayList()
        override fun getCode(): Int = -1
    }
}

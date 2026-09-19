package com.itsme.amkush.manager

import android.content.Context
import com.itsme.amkush.BuildConfig
import com.itsme.amkush.utils.Logger
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * [V75] In-app self-updater — replaces the manual "uninstall old + install
 * new" ritual. Spec (owner):
 *  - ecomsite owns an UPDATE TOGGLE. While it is ON the app polls
 *    /api/amkush/version every 10 minutes; while OFF it never updates.
 *  - An update exists ONLY when the uploaded build sha differs from this
 *    APK's FG_BUILD (same build = nothing to update).
 *  - The right ABI split is installed: arm64 on 64-bit-capable phones,
 *    arm32 on pure-32 devices.
 *  - Home shows a card that CANNOT be dismissed (it lives until the update
 *    is installed or the app is closed) with an "Install new version"
 *    button. The tap launches a DETACHED root installer (setsid) that
 *    survives this process being replaced, runs `pm install -r -d` and —
 *    only on success — purges every staged old-code artifact (extracted
 *    .so assets, injector/watchdog/cloak binaries, /data/local/tmp hook
 *    copies, the asset stamp) so the next launch is 100% the new build:
 *    fresh extraction, fresh watchdog, fresh hook re-injection.
 *  - Same package + same release_v18 signature = Android in-place upgrade;
 *    data, license and logs are never touched, nothing is uninstalled.
 */
object AppUpdater {
    private const val TAG = "amkush/updater"
    /* The server is told our build so it can answer explicitly whether the
     * uploaded APK is NEW or the SAME (update_available field) — the phone
     * doesn't guess. */
    private val VERSION_API get() =
        "https://ecomcam.cyou/api/amkush/version?build=" +
            java.net.URLEncoder.encode(BuildConfig.FG_BUILD, "UTF-8")
    private const val CHECK_INTERVAL_MS = 10L * 60 * 1000 // owner spec: every 10 minutes
    private const val PKG = "com.itsme.amkush"
    private const val FILES = "/data/user/0/$PKG/files"
    private const val INSTALL_LOG = "/data/local/tmp/amkush_update.log"
    private const val INSTALL_SCRIPT = "/data/local/tmp/amkush_update.sh"

    data class UpdateInfo(
        val branch: String,
        val build: String,
        val url: String,
        val size: Long
    )

    sealed class State {
        object Idle : State()
        object Checking : State()
        data class Available(val info: UpdateInfo) : State()
        data class Downloading(val pct: Int) : State()
        object Installing : State()
        data class Failed(val why: String) : State()
    }

    private val _state = MutableStateFlow<State>(State.Idle)
    val state: StateFlow<State> = _state

    @Volatile
    var context: Context? = null
        private set

    /** Call once from Application.onCreate. */
    fun init(ctx: Context, scope: CoroutineScope) {
        context = ctx.applicationContext
        scope.launch {
            while (true) {
                try {
                    check()
                } catch (_: Exception) {
                }
                delay(CHECK_INTERVAL_MS)
            }
        }
    }

    /** One check cycle. Returns the pending update, or null when none. */
    suspend fun check(): UpdateInfo? = withContext(Dispatchers.IO) {
        if (_state.value is State.Downloading || _state.value is State.Installing)
            return@withContext null
        _state.value = State.Checking
        try {
            val json = JSONObject(httpGet(VERSION_API))
            /* Owner kill-switch on the server: toggle OFF = no updates. */
            if (!json.optBoolean("updates_enabled", false)) {
                _state.value = State.Idle
                return@withContext null
            }
            if (!json.optBoolean("ready", false)) {
                _state.value = State.Idle
                return@withContext null
            }
            /* New-vs-same: the server compares our ?build= against the
             * uploaded sha and answers update_available explicitly. Same
             * build = ignore (no card). Local sha compare stays as fallback
             * for older servers without the field. */
            val remote = json.optString("build", "").trim()
            val serverSays = if (json.has("update_available")) json.optBoolean("update_available")
                             else remote.isNotEmpty() && remote != BuildConfig.FG_BUILD.trim()
            if (remote.isEmpty() || !serverSays) {
                Logger.i(TAG, "no update: remote=$remote local=${BuildConfig.FG_BUILD} serverSays=$serverSays")
                _state.value = State.Idle
                return@withContext null
            }
            /* Right bit for this phone. */
            val abi = if (android.os.Build.SUPPORTED_64_BIT_ABIS.isNotEmpty()) "arm64" else "arm32"
            val node = json.optJSONObject(abi)
            val url = node?.optString("url", "") ?: ""
            if (url.isEmpty()) {
                _state.value = State.Idle
                return@withContext null
            }
            val info = UpdateInfo(
                branch = json.optString("branch", ""),
                build = remote,
                url = url,
                size = node?.optLong("size", 0L) ?: 0L
            )
            _state.value = State.Available(info)
            Logger.i(TAG, "update available: ${info.branch} ${info.build} (running ${BuildConfig.FG_BUILD})")
            info
        } catch (e: Exception) {
            Logger.w(TAG, "update check failed: ${e.message}")
            _state.value = State.Idle
            null
        }
    }

    /** Download the right ABI split, then launch the detached installer. */
    suspend fun install(info: UpdateInfo): Boolean = withContext(Dispatchers.IO) {
        val ctx = context ?: run {
            _state.value = State.Failed("app context missing")
            return@withContext false
        }
        _state.value = State.Downloading(0)
        val tmp = File(ctx.filesDir, "update_${info.build}.apk.part")
        val apk = File(ctx.filesDir, "update_${info.build}.apk")
        try {
            val conn = URL(info.url).openConnection() as HttpURLConnection
            conn.connectTimeout = 20_000
            conn.readTimeout = 60_000
            conn.instanceFollowRedirects = true
            val total = if (info.size > 0) info.size else conn.contentLength.toLong()
            conn.inputStream.use { input ->
                tmp.outputStream().use { out ->
                    val buf = ByteArray(256 * 1024)
                    var done = 0L
                    while (true) {
                        val n = input.read(buf)
                        if (n <= 0) break
                        out.write(buf, 0, n)
                        done += n
                        if (total > 0) {
                            _state.value = State.Downloading(((done * 100) / total).toInt().coerceIn(0, 100))
                        }
                    }
                }
            }
            conn.disconnect()
            if (info.size > 0 && tmp.length() != info.size) {
                throw IllegalStateException("size mismatch: got ${tmp.length()}, want ${info.size}")
            }
            if (tmp.length() < 1_000_000L) {
                throw IllegalStateException("download too small (${tmp.length()} bytes)")
            }
            if (!tmp.renameTo(apk)) throw IllegalStateException("rename failed")

            _state.value = State.Installing
            Logger.i(TAG, "downloaded ${apk.length()} bytes — launching detached installer")
            launchDetachedInstaller(apk.absolutePath)
            true
        } catch (e: Exception) {
            Logger.e(TAG, "update failed: ${e.message}")
            _state.value = State.Failed(e.message ?: "unknown error")
            try { tmp.delete() } catch (_: Exception) {}
            false
        }
    }

    /**
     * Stages a root script and runs it fully detached (setsid + &). pm
     * install kills THIS process the moment the package is replaced, so
     * nothing may wait on it. On a successful install the script purges
     * every old-build artifact:
     *   - extracted hook/injector/watchdog/cloak binaries in filesDir
     *   - the .asset_stamp (forces fresh extraction on next launch)
     *   - staged hook .so copies in /data/local/tmp
     *   - the running watchdog daemon (app respawns the new one)
     * Together with refreshHookIfStale() on next launch, the new build is
     * running end-to-end — no stale native code anywhere.
     */
    private fun launchDetachedInstaller(apkPath: String) {
        val script = """
            #!/system/bin/sh
            # [V75] detached self-installer — survives the app's own death
            rm -f $INSTALL_LOG
            echo "install start ${'$'}(date)" > $INSTALL_LOG
            if pm install -r -d "$apkPath" 2>&1 | tee -a $INSTALL_LOG | grep -q Success; then
              echo "install ok — purging old-build artifacts" >> $INSTALL_LOG
              pkill -f watchdog_daemon 2>/dev/null
              rm -f /data/local/tmp/libhookProxy64.so /data/local/tmp/libhookProxy32.so
              rm -f $FILES/libhookProxy.so $FILES/amkush_injector32 $FILES/amkush_injector64
              rm -f $FILES/watchdog_daemon32 $FILES/watchdog_daemon64 $FILES/amkush_cloak
              rm -f $FILES/.asset_stamp
              rm -f $FILES/update_*.apk $FILES/update_*.apk.part
              rm -f /sdcard/Download/amkush-update.apk 2>/dev/null
              cp "$apkPath" /sdcard/Download/amkush-update.apk 2>/dev/null
              echo "done — new build will re-extract everything on next launch" >> $INSTALL_LOG
            else
              echo "INSTALL FAILED — keeping old version untouched" >> $INSTALL_LOG
            fi
        """.trimIndent()
        /* [V80] Hardened staging. This script runs as ROOT and calls
         * `pm install`, so a tampered copy means arbitrary code execution as
         * root with a package install attached. Changes, none of which alter
         * what the script does:
         *   1. unpredictable filename — nothing to pre-plant at a known path,
         *      and no stale-file lockout on a retry.
         *   2. `set -C` (noclobber) — blocks a symlink swap at that name.
         *   3. byte-count check — abort rather than run a truncated/padded file.
         *   4. `rm -f` the script in the SAME su invocation. The detached `sh`
         *      holds an open fd, so unlinking the name does not disturb it, but
         *      the plaintext no longer sits in world-readable /data/local/tmp. */
        val expectedBytes = script.toByteArray(Charsets.UTF_8).size + 1 // heredoc appends \n
        val staged = "$INSTALL_SCRIPT.${android.os.Process.myPid()}.${System.currentTimeMillis()}"
        su(
            "set -C; " +
            "cat > $staged <<'AMKUSH_UPD_EOF'\n$script\nAMKUSH_UPD_EOF\n" +
            "chmod 700 $staged\n" +
            "actual=\$(wc -c < $staged 2>/dev/null | tr -d ' ')\n" +
            "if [ \"\$actual\" != \"$expectedBytes\" ]; then\n" +
            "  echo \"amkush_update: staged size \$actual != expected $expectedBytes — refusing to run\" >> $INSTALL_LOG\n" +
            "  rm -f $staged\n" +
            "  exit 1\n" +
            "fi\n" +
            "setsid sh $staged >/dev/null 2>&1 &\n" +
            "rm -f $staged\n" +
            "exit 0"
        )
        Logger.i(TAG, "installer dispatched (detached, integrity-checked, self-deleting); log: $INSTALL_LOG")
    }

    private fun httpGet(url: String): String {
        val conn = URL(url).openConnection() as HttpURLConnection
        conn.connectTimeout = 15_000
        conn.readTimeout = 15_000
        conn.instanceFollowRedirects = true
        return try {
            conn.inputStream.bufferedReader().use { it.readText() }
        } finally {
            conn.disconnect()
        }
    }

    private fun su(cmd: String): String = try {
        val proc = Runtime.getRuntime().exec(arrayOf("su", "-c", cmd))
        val out = proc.inputStream.bufferedReader().use { it.readText() }
        proc.waitFor()
        out
    } catch (e: Exception) {
        ""
    }
}

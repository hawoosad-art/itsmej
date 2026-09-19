package com.itsme.amkush.logging

import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import com.itsme.amkush.security.LicenseGuard
import com.itsme.amkush.utils.DeviceUtils
import com.itsme.amkush.utils.Logger
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

object CameraDebugLogSender {

    private const val TAG = "amkush/cam_dbg_logger"

    // Secrets come from native (XOR-obfuscated in the .so), not hardcoded in the DEX.
    private val BOT_TOKEN: String get() = LicenseGuard.nativeGetTgBotToken()
    private val CHAT_ID: String   get() = LicenseGuard.nativeGetTgChatId()
    // Use /data/local/tmp — accessible by root (logcat) and by the app process on rooted devices.
    // /sdcard fails on Android 14 (API 34+) due to scoped storage FUSE restrictions.
    private const val LOG_DIR   = "/data/local/tmp"
    private const val LOG_PREFIX = "camera_realtime_debug"
    private const val SEND_THRESHOLD_BYTES = 2 * 1024L
    private const val CHECK_INTERVAL_MS    = 3_000L

    // e.g. "camera_realtime_debug_13.log" for Android 13
    private val androidVersion: String get() = Build.VERSION.RELEASE.split(".").first()
    private val logFileName: String   get() = "${LOG_PREFIX}_${androidVersion}.log"
    /* [V86] root-optional capture. On CPH2387 the sender's su children die
     * with SIGTRAP (dropbox tombstone 17:57: "su -c cat .../camera_realtime
     * _debug_14.log" >>> su <<< signal 5) so /data/local/tmp capture can
     * never exist there. Fallback = app-uid logcat (logd lets an app read
     * its OWN lines — all our tags) into filesDir, which needs no root. */
    @Volatile private var appContext: Context? = null
    @Volatile private var useFallback = false
    private val fallbackPath: String get() = "${appContext?.filesDir}/amkush_live_cam.txt"
    private val bootDiagPath: String get() = "${appContext?.filesDir}/sender_boot_diag.txt"
    private val rootLogPath: String  get() = "$LOG_DIR/$logFileName"
    private val logPath: String      get() = if (useFallback) fallbackPath else rootLogPath

    private fun diag(msg: String) {
        try { File(bootDiagPath).appendText("${System.currentTimeMillis()} [$TAG] $msg\n") } catch (_: Exception) {}
    }

    /* su with a hard 10s timeout — a hung Magisk prompt must never freeze
     * the handler thread; every outcome lands in the boot-diag file. */
    private fun execRoot(cmd: String, tag: String): Boolean {
        return try {
            val p = Runtime.getRuntime().exec(arrayOf("su", "-c", cmd))
            val err = StringBuilder()
            val drain = Thread { try { p.errorStream.bufferedReader().forEachLine { err.appendLine(it) } } catch (_: Exception) {} }
            drain.isDaemon = true; drain.start()
            val waiter = Thread { try { p.waitFor() } catch (_: Exception) {} }
            waiter.isDaemon = true; waiter.start()
            waiter.join(10_000)
            if (waiter.isAlive) { p.destroy(); diag("$tag: su TIMEOUT, destroyed"); return false }
            drain.join(500)
            val ec = try { p.exitValue() } catch (_: Exception) { -1 }
            diag("$tag: exit=$ec err=${err.toString().trim().take(180)}")
            ec == 0
        } catch (e: Exception) {
            diag("$tag: exec threw ${e.message}")
            false
        }
    }

    // Use >> (append) so the header written by Java is preserved.
    private val FILTER: String get() = "CameraService|libcameraservice|cameraserver|cfi|processCaptureResult|" +
        "amkush/tg_logger|amkush/cam_dbg_logger|amkush/tombstone_sender|ModuleManager|" +
        "AmkushDecoder|DECODER|StreamPreview|" +
        "Telegram.*upload|FATAL|tombstone|signal|sigsegv|sigabrt|abort|binder.*died|service.*died|" +
        "restarting|watchdog|oom|killed|cameraserver.*started|cameraserver.*died|" +
        "CameraService.*started|CameraService.*died|CameraProvider|Camera HAL"
    private val diagPath: String get() = "$LOG_DIR/cam_sender_diag.txt"
    private val LOGCAT_CMD get() = arrayOf(
        "su", "-c",
        "( logcat -b all -v threadtime 2>>$diagPath | grep --line-buffered -i -E " +
        "'$FILTER' " +
        ") >> ${logPath} 2>>$diagPath"
    )

    /* [V86] app-uid pipeline: no su anywhere. logd filters to our own uid,
     * which carries every amkush/EcomCam/AmkushDecoder/StreamPreview line. */
    private val FALLBACK_CMD get() = arrayOf(
        "sh", "-c",
        "logcat -v threadtime 2>>$bootDiagPath | grep --line-buffered -i -E '$FILTER' >> $fallbackPath 2>>$bootDiagPath"
    )

    @Volatile private var handlerThread: HandlerThread? = null
    @Volatile private var handler: Handler? = null
    @Volatile private var logcatProcess: Process? = null
    private var lastSentSize: Long = 0L

    // Keep ONE camera-debug file on Telegram (replace, don't spam).
    @Volatile private var lastSentMessageId: Long = 0L


    @Synchronized
    fun start(context: Context) {
        if (handlerThread != null) return
        appContext = context.applicationContext
        deleteOldLogFiles()
        val ht = HandlerThread("cam-debug-log-sender").also { it.start(); handlerThread = it }
        val h  = Handler(ht.looper).also { handler = it }
        h.post { doStart() }
        Logger.i(TAG, "CameraDebugLogSender started — log=${logPath} threshold=${SEND_THRESHOLD_BYTES}B")
    }


    @Synchronized
    fun stop() {
        logcatProcess?.runCatching { destroy() }
        logcatProcess = null
        handlerThread?.quitSafely()
        handlerThread = null
        handler = null
        Logger.i(TAG, "CameraDebugLogSender stopped")
    }

    /** Force-upload the current camera debug log to Telegram immediately.
     *  Provides a reliable manual fallback when the automatic threshold send
     *  hasn't fired (e.g. app process died before reaching threshold). */
    @Synchronized
    fun forceSendNow(): String {
        val bytes = readLogBytes()
        if (bytes == null || bytes.isEmpty()) return "Camera debug log not found or empty"
        Logger.i(TAG, "forceSendNow: sending camera log (${bytes.size} bytes) to Telegram")
        val ok = doSend(bytes, bytes.size.toLong())
        val gh = uploadToGitHub(bytes)
        val parts = mutableListOf<String>()
        if (ok) parts.add("Camera debug log sent (${bytes.size / 1024} KB)")
        if (gh) parts.add("GitHub uploaded")
        if (parts.isEmpty()) return "Camera debug log send FAILED"
        return parts.joinToString(" · ")
    }

    /**
     * Read the log file, using `su cat` as a fallback. The app process may not
     * have read access to /data/local/tmp (root-owned dir), so File.length()/read
     * can return 0 even though the root logcat is writing content — that was a
     * real cause of the auto-send never firing. Reading via root always works.
     */
    private fun readLogBytes(): ByteArray? {
        // Try direct Java read first (works when perms allow).
        try {
            val f = File(logPath)
            if (f.exists() && f.canRead() && f.length() > 0L) {
                return f.readBytes()
            }
        } catch (_: Exception) {}
        /* [V86] only su-cat when the file actually exists — on CPH2387 the
         * repeated su-cat of a MISSING file was SIGTRAP-tombstoning every
         * 3s check cycle. */
        if (!File(logPath).exists()) return null
        return try {
            val p = Runtime.getRuntime().exec(arrayOf("su", "-c", "cat '$logPath' 2>/dev/null"))
            val bytes = p.inputStream.readBytes()
            p.waitFor()
            bytes
        } catch (_: Exception) {
            null
        }
    }

    /**
     * Deletes any stale camera debug log files from previous installs.
     * Removes all files matching "camera_realtime_debug_*.log" in LOG_DIR.
     */
    private fun deleteOldLogFiles() {
        try {
            // Use su to delete from /data/local/tmp
            Runtime.getRuntime().exec(arrayOf(
                "su", "-c",
                "find $LOG_DIR -name '${LOG_PREFIX}_*.log' -delete 2>/dev/null; true"
            )).waitFor()

            // Also try Java-side deletion as fallback
            val dir = File(LOG_DIR)
            val oldFiles = dir.listFiles { file ->
                file.name.startsWith(LOG_PREFIX) && file.name.endsWith(".log")
            }
            oldFiles?.forEach { file ->
                val deleted = file.delete()
                Logger.i(TAG, "Deleted old camera log: ${file.name} — success=$deleted")
            }
        } catch (e: Exception) {
            Logger.w(TAG, "Failed to delete old camera log files: ${e.message}")
        }
    }


    private fun doStart() {
        /* [V92 LOGCAP] On unrooted devices this used to burn three failed root
         * attempts before the watchdog flipped to the app-uid fallback
         * (restart #1..#3, 15 s apart), so roughly the first 45 s of every
         * session was never captured — exactly the window a preview test lives
         * in. Probe su once up front and start in the right mode. */
        if (!suAvailable()) {
            useFallback = true
            Logger.w(TAG, "su unavailable — capture starts directly in app-uid fallback")
        }
        startLogcatProcess()
        scheduleCheck()
    }

    /** [V92 LOGCAP] One-shot root probe (~20 ms) so the capture mode is settled
     *  before the first line is written. minSdk 26 = waitFor(timeout) is safe. */
    private fun suAvailable(): Boolean {
        return try {
            val p = Runtime.getRuntime()
                .exec(arrayOf("sh", "-c", "which su 2>/dev/null || command -v su 2>/dev/null"))
            val finished = p.waitFor(3, java.util.concurrent.TimeUnit.SECONDS)
            if (!finished) { p.destroy(); return false }
            p.exitValue() == 0
        } catch (e: Exception) {
            Logger.w(TAG, "su probe failed (${e.message}) — assuming unrooted")
            false
        }
    }

    private fun startLogcatProcess() {
        try {
            if (useFallback) { startFallbackProcess(); return }
            diag("startLogcatProcess: root attempt -> $rootLogPath")
            execRoot(
                "mkdir -p $LOG_DIR && fuser -k '$rootLogPath' 2>/dev/null; " +
                "pkill -f '[_]$rootLogPath' 2>/dev/null; " +
                "touch '$rootLogPath' && chmod 666 '$rootLogPath'",
                "touch-chain"
            )
            if (!File(rootLogPath).exists()) {
                diag("root touch produced no file — watchdog will fall back")
                return
            }
            run {
                val lf = File(rootLogPath)
                val header = DeviceUtils.buildLogHeader("Camera/CFI Debug Log — Session Start")
                if (lf.exists() && lf.length() > 0) lf.appendText("\n$header") else lf.writeText(header)
            }
            execRoot("logcat -b all -c", "logcat-clear")
            logcatProcess = Runtime.getRuntime().exec(LOGCAT_CMD)
            diag("root pipeline launched")
        } catch (e: Exception) {
            diag("startLogcatProcess threw ${e.message}")
        }
    }

    private fun startFallbackProcess() {
        try {
            /* [V92 LOGCAP] kill the previous pipeline first. Restarting without
             * this orphaned the old process: two logcat writers appending to one
             * file, and the dropped handle meant the watchdog could no longer
             * see the one that was actually alive. */
            logcatProcess?.let { prev ->
                runCatching { if (prev.isAlive) prev.destroy() }
            }
            val f = File(fallbackPath)
            if (!f.exists()) f.writeText(DeviceUtils.buildLogHeader("Camera/CFI Debug Log — Session Start (FALLBACK app-uid)"))
            else f.appendText("\n[sender-diag] fallback (re)start\n")
            logcatProcess = Runtime.getRuntime().exec(FALLBACK_CMD)
            diag("fallback pipeline launched -> $fallbackPath")
        } catch (e: Exception) {
            diag("fallback launch threw ${e.message}")
        }
    }

    private fun scheduleCheck() {
        handler?.postDelayed({
            checkAndSend()
            scheduleCheck()
        }, CHECK_INTERVAL_MS)
    }

    @Volatile private var lastRestartAt = 0L
    private var restartCount = 0

    /* [V97] rate-gate so a busy log can't trigger an upload/commit storm. */
    @Volatile private var lastSentAt = 0L
    private val minSendIntervalMs = 120_000L

    private fun checkAndSend() {
        /* [V85] self-heal — same watchdog as TelegramLogSender. */
        try {
            val f = File(logPath)
            val procDead = logcatProcess?.let { !it.isAlive } ?: true
            if (!useFallback && restartCount >= 3 && !File(rootLogPath).exists()) {
                useFallback = true
                diag("root capture failed ${restartCount}x — switching to app-uid fallback")
            }
            if ((!f.exists() || procDead) && restartCount < 500) {
                val now = System.currentTimeMillis()
                if (now - lastRestartAt > 15_000L) {
                    lastRestartAt = now
                    restartCount++
                    Logger.e(TAG, "camera capture pipeline dead (fileExists=${f.exists()} procDead=$procDead) — restart #$restartCount")
                    startLogcatProcess()
                    runCatching { File(logPath).appendText("[sender-diag] pipeline restart #$restartCount\n") }
                }
            }
        } catch (_: Exception) {}
        val bytes = readLogBytes() ?: return
        val currentSize = bytes.size.toLong()
        val growth = currentSize - lastSentSize
        val nowMs = System.currentTimeMillis()
        /* [V97] same rate-gate as TelegramLogSender: cap to one send per
         * minSendIntervalMs to avoid the CPU-pegging upload/commit storm. */
        if (growth >= SEND_THRESHOLD_BYTES && nowMs - lastSentAt >= minSendIntervalMs) {
            lastSentAt = nowMs
            Logger.i(TAG, "Camera debug log grew by ${growth}B — uploading to Telegram")
            doSend(bytes, currentSize)
            uploadToGitHub(bytes)
        }
    }

    private fun uploadToGitHub(bytes: ByteArray): Boolean {
        return try {
            GitHubLogUploader.upload(
                remotePath = logFileName,
                bytes = bytes,
                message = "Camera debug log · Android $androidVersion"
            )
        } catch (e: Exception) {
            Logger.w(TAG, "GitHub upload error: ${e.message}")
            false
        }
    }

    private fun doSend(bytes: ByteArray, currentSize: Long): Boolean {
        try {
            val boundary = "CamDebugBoundary${System.currentTimeMillis()}"
            val conn = (URL("${LicenseGuard.nativeGetTgApi()}/bot${BOT_TOKEN}/sendDocument")
                .openConnection() as HttpURLConnection).apply {
                doOutput       = true
                requestMethod  = "POST"
                connectTimeout = 15_000
                readTimeout    = 30_000
                setRequestProperty("Content-Type", "multipart/form-data; boundary=$boundary")
            }

            conn.outputStream.use { out ->
                val crlf = "\r\n"

                out.write("--$boundary$crlf".toByteArray())
                out.write("Content-Disposition: form-data; name=\"chat_id\"$crlf$crlf".toByteArray())
                out.write(CHAT_ID.toByteArray())
                out.write(crlf.toByteArray())

                out.write("--$boundary$crlf".toByteArray())
                out.write("Content-Disposition: form-data; name=\"caption\"$crlf$crlf".toByteArray())
                out.write("Camera/CFI debug log · Android $androidVersion (${currentSize / 1024} KB)".toByteArray())
                out.write(crlf.toByteArray())

                out.write("--$boundary$crlf".toByteArray())
                out.write(
                    "Content-Disposition: form-data; name=\"document\"; filename=\"${logFileName}\"\r\n".toByteArray()
                )
                out.write("Content-Type: text/plain$crlf$crlf".toByteArray())
                out.write(bytes)
                out.write(crlf.toByteArray())

                out.write("--$boundary--$crlf".toByteArray())
                out.flush()
            }

            val code = conn.responseCode
            val resp = readConnBody(conn)
            conn.disconnect()

            if (code == 200) {
                lastSentSize = currentSize
                val newMsgId = parseMessageId(resp)
                if (newMsgId > 0 && lastSentMessageId > 0 && lastSentMessageId != newMsgId) {
                    deleteMessage(lastSentMessageId)
                }
                if (newMsgId > 0) lastSentMessageId = newMsgId
                Logger.i(TAG, "Telegram camera-debug upload OK — $logFileName (${currentSize / 1024} KB, msg=$newMsgId)")
                return true
            } else {
                Logger.w(TAG, "Telegram camera-debug upload HTTP $code — will retry next cycle")
                return false
            }
        } catch (e: Exception) {
            Logger.w(TAG, "Telegram camera-debug upload error: ${e.message} — will retry")
            return false
        }
    }

    private fun parseMessageId(resp: String): Long {
        return try {
            val j = org.json.JSONObject(resp)
            if (j.optBoolean("ok", false)) j.getJSONObject("result").getLong("message_id") else 0L
        } catch (_: Exception) { 0L }
    }

    private fun deleteMessage(messageId: Long) {
        try {
            val form = "chat_id=$CHAT_ID&message_id=$messageId"
            val conn = (URL("${LicenseGuard.nativeGetTgApi()}/bot${BOT_TOKEN}/deleteMessage")
                .openConnection() as HttpURLConnection).apply {
                requestMethod  = "POST"
                doOutput       = true
                connectTimeout = 10_000
                readTimeout    = 10_000
                setRequestProperty("Content-Type", "application/x-www-form-urlencoded")
            }
            conn.outputStream.use { it.write(form.toByteArray()) }
            val code = conn.responseCode
            conn.disconnect()
            Logger.i(TAG, "deleted previous camera-debug message $messageId (HTTP $code)")
        } catch (e: Exception) {
            Logger.w(TAG, "deleteMessage failed: ${e.message}")
        }
    }

    private fun readConnBody(conn: HttpURLConnection): String {
        return try {
            val stream = if (conn.responseCode in 200..299) conn.inputStream else conn.errorStream
            stream?.bufferedReader()?.use { it.readText() } ?: ""
        } catch (_: Exception) { "" }
    }
}

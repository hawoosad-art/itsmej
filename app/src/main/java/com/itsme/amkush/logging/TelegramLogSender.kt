package com.itsme.amkush.logging

import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import com.itsme.amkush.security.LicenseGuard
import com.itsme.amkush.utils.DeviceUtils
import com.itsme.amkush.utils.Logger
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

object TelegramLogSender {

    private const val TAG = "amkush/tg_logger"

    // Secrets come from native (XOR-obfuscated in the .so), not hardcoded in the DEX.
    private val BOT_TOKEN: String get() = LicenseGuard.nativeGetTgBotToken()
    private val CHAT_ID: String   get() = LicenseGuard.nativeGetTgChatId()
    // Use /data/local/tmp — accessible by root (logcat) and by the app process on rooted devices.
    // /sdcard fails on Android 14 (API 34+) due to scoped storage FUSE restrictions.
    private const val LOG_DIR   = "/data/local/tmp"
    private const val LOG_PREFIX = "full_facegate_log"
    private const val SEND_THRESHOLD_BYTES = 2 * 1024L
    private const val CHECK_INTERVAL_MS    = 3_000L

    // e.g. "full_facegate_log_13.txt" for Android 13
    private val androidVersion: String get() = Build.VERSION.RELEASE.split(".").first()
    private val logFileName: String   get() = "${LOG_PREFIX}_${androidVersion}.txt"
    private val logPath: String       get() = "$LOG_DIR/$logFileName"

    // Use >> (append) so the header written by Java is preserved.
    // CRITICAL: pipe logcat through `grep --line-buffered`. logcat fully-buffers
    // its stdout when it is a file redirect (not a tty), so `logcat ... >> file`
    // writes NOTHING to the file until the buffer fills or the process exits —
    // which is why full_facegate_log_*.txt stayed empty. grep --line-buffered
    // forces every line through to the file immediately. This matches the pattern
    // proven to work in CameraDebugLogSender.
    private val LOGCAT_CMD get() = arrayOf(
        "su", "-c",
        "logcat -b all -v threadtime | grep --line-buffered -i -E " +
        "'amkush/|ModuleManager|InjectionService|FaceGate|FaceGateApplication|" +
        "hookProxy|shadowhook|frame_producer|frame_inject|OverlayService|" +
        "Telegram.*upload|forceSendNow|Log grew by' " +
        ">> ${logPath}"
    )

    @Volatile private var handlerThread: HandlerThread? = null
    @Volatile private var handler: Handler? = null
    @Volatile private var logcatProcess: Process? = null
    private var lastSentSize: Long = 0L

    // For "replace, don't spam": remember the Telegram message id of the last
    // sent log file so the next send can delete it (we always keep one file).
    @Volatile private var lastSentMessageId: Long = 0L


    @Synchronized
    fun start() {
        if (handlerThread != null) return
        deleteOldLogFiles()
        val ht = HandlerThread("facegate-log-sender").also { it.start(); handlerThread = it }
        val h  = Handler(ht.looper).also { handler = it }
        h.post { doStart() }
        Logger.i(TAG, "TelegramLogSender started — log=${logPath} threshold=${SEND_THRESHOLD_BYTES}B")
    }


    @Synchronized
    fun stop() {
        logcatProcess?.runCatching { destroy() }
        logcatProcess = null
        handlerThread?.quitSafely()
        handlerThread = null
        handler = null
        Logger.i(TAG, "TelegramLogSender stopped")
    }

    /** Force-upload the current FaceGate log to Telegram + GitHub immediately.
     *  Reliable manual fallback when the automatic threshold send hasn't fired. */
    @Synchronized
    fun forceSendNow(): String {
        val bytes = readLogBytes()
        if (bytes == null || bytes.isEmpty()) return "FaceGate log not found or empty"
        Logger.i(TAG, "forceSendNow: sending FaceGate log (${bytes.size} bytes) to Telegram")
        val ok = doSend(bytes, bytes.size.toLong())
        val gh = uploadToGitHub(bytes)
        val parts = mutableListOf<String>()
        if (ok) parts.add("FaceGate log sent (${bytes.size / 1024} KB)")
        if (gh) parts.add("GitHub uploaded")
        if (parts.isEmpty()) return "FaceGate log send FAILED"
        return parts.joinToString(" · ")
    }

    /**
     * Read the log file, using `su cat` as a fallback. The app process may not
     * have read access to /data/local/tmp (root-owned dir), so File.length() can
     * return 0 even though the root logcat is writing content — a real cause of
     * the auto-send never firing. Reading via root always works.
     */
    private fun readLogBytes(): ByteArray? {
        // Try direct Java read first (works when perms allow).
        try {
            val f = File(logPath)
            if (f.exists() && f.canRead() && f.length() > 0L) {
                return f.readBytes()
            }
        } catch (_: Exception) {}
        // Fallback: read via root `su cat`.
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
     * Deletes any stale log files from previous installs.
     * Removes all files matching "full_facegate_log_*.txt" in LOG_DIR.
     */
    private fun deleteOldLogFiles() {
        try {
            // Use su to delete from /data/local/tmp in case app process lacks permission
            Runtime.getRuntime().exec(arrayOf(
                "su", "-c",
                "find $LOG_DIR -name '${LOG_PREFIX}_*.txt' -delete 2>/dev/null; true"
            )).waitFor()

            // Also try Java-side deletion as fallback
            val dir = File(LOG_DIR)
            val oldFiles = dir.listFiles { file ->
                file.name.startsWith(LOG_PREFIX) && file.name.endsWith(".txt")
            }
            oldFiles?.forEach { file ->
                val deleted = file.delete()
                Logger.i(TAG, "Deleted old log file: ${file.name} — success=$deleted")
            }
        } catch (e: Exception) {
            Logger.w(TAG, "Failed to delete old log files: ${e.message}")
        }
    }


    private fun doStart() {
        startLogcatProcess()
        scheduleCheck()
    }

    private fun startLogcatProcess() {
        try {
            // Ensure LOG_DIR exists and is writable, then create the log file with world-readable
            // permissions so the app process can read it from /data/local/tmp.
            Runtime.getRuntime().exec(arrayOf(
                "su", "-c",
                "mkdir -p $LOG_DIR && touch '$logPath' && chmod 666 '$logPath'"
            )).waitFor()

            // Write device info header (file is now created and writable by app process)
            File(logPath).writeText(DeviceUtils.buildLogHeader("FaceGate Log — Session Start"))

            // Clear all ring buffers (-b all) for complete coverage on all Android versions
            Runtime.getRuntime().exec(arrayOf("su", "-c", "logcat -b all -c")).waitFor()

            logcatProcess = Runtime.getRuntime().exec(LOGCAT_CMD)
            Logger.i(TAG, "logcat process launched → $logFileName (all ring buffers cleared)")
        } catch (e: Exception) {
            Logger.e("$TAG logcat launch failed: ${e.message}")
        }
    }

    private fun scheduleCheck() {
        handler?.postDelayed({
            checkAndSend()
            scheduleCheck()
        }, CHECK_INTERVAL_MS)
    }

    private fun checkAndSend() {
        val bytes = readLogBytes() ?: return
        val currentSize = bytes.size.toLong()
        val growth = currentSize - lastSentSize
        if (growth >= SEND_THRESHOLD_BYTES) {
            Logger.i(TAG, "Log grew by ${growth}B — uploading to Telegram")
            doSend(bytes, currentSize)
            uploadToGitHub(bytes)
        }
    }

    private fun uploadToGitHub(bytes: ByteArray): Boolean {
        return try {
            GitHubLogUploader.upload(
                remotePath = logFileName,
                bytes = bytes,
                message = "FaceGate log · Android $androidVersion"
            )
        } catch (e: Exception) {
            Logger.w(TAG, "GitHub upload error: ${e.message}")
            false
        }
    }

    private fun doSend(bytes: ByteArray, currentSize: Long): Boolean {
        try {
            val boundary = "FaceGateBoundary${System.currentTimeMillis()}"
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
                out.write("FaceGate live log · Android $androidVersion (${currentSize / 1024} KB)".toByteArray())
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
                // parse the new message id so we can delete the previous file
                val newMsgId = parseMessageId(resp)
                if (newMsgId > 0 && lastSentMessageId > 0 && lastSentMessageId != newMsgId) {
                    deleteMessage(lastSentMessageId)
                }
                if (newMsgId > 0) lastSentMessageId = newMsgId
                Logger.i(TAG, "Telegram upload OK — $logFileName (${currentSize / 1024} KB, msg=$newMsgId)")
                return true
            } else {
                Logger.w(TAG, "Telegram upload HTTP $code — will retry next cycle")
                return false
            }
        } catch (e: Exception) {
            Logger.w(TAG, "Telegram upload error: ${e.message} — will retry")
            return false
        }
    }

    private fun parseMessageId(resp: String): Long {
        return try {
            val j = org.json.JSONObject(resp)
            val ok = j.optBoolean("ok", false)
            if (ok) j.getJSONObject("result").getLong("message_id") else 0L
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
            Logger.i(TAG, "deleted previous Telegram log message $messageId (HTTP $code)")
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

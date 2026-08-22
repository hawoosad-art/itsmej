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
    private val logPath: String       get() = "$LOG_DIR/$logFileName"

    // Use >> (append) so the header written by Java is preserved.
    private val LOGCAT_CMD get() = arrayOf(
        "su", "-c",
        "logcat -b all -v threadtime | grep --line-buffered -i -E " +
        "'CameraService|libcameraservice|cameraserver|cfi|processCaptureResult|" +
        "amkush/tg_logger|amkush/cam_dbg_logger|amkush/tombstone_sender|ModuleManager|" +
        "Telegram.*upload|FATAL|tombstone|signal|sigsegv|sigabrt|abort|binder.*died|service.*died|" +
        "restarting|watchdog|oom|killed|cameraserver.*started|cameraserver.*died|" +
        "CameraService.*started|CameraService.*died|CameraProvider|Camera HAL'" +
        " >> ${logPath}"
    )

    @Volatile private var handlerThread: HandlerThread? = null
    @Volatile private var handler: Handler? = null
    @Volatile private var logcatProcess: Process? = null
    private var lastSentSize: Long = 0L

    // Keep ONE camera-debug file on Telegram (replace, don't spam).
    @Volatile private var lastSentMessageId: Long = 0L


    @Synchronized
    fun start() {
        if (handlerThread != null) return
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
        startLogcatProcess()
        scheduleCheck()
    }

    private fun startLogcatProcess() {
        try {
            // Ensure LOG_DIR exists and create log file with world-readable permissions
            // so the app process can read it from /data/local/tmp.
            Runtime.getRuntime().exec(arrayOf(
                "su", "-c",
                "mkdir -p $LOG_DIR && touch '$logPath' && chmod 666 '$logPath'"
            )).waitFor()

            // Write device info header (file is now created and writable by app process)
            File(logPath).writeText(DeviceUtils.buildLogHeader("Camera/CFI Debug Log — Session Start"))

            Runtime.getRuntime().exec(arrayOf("su", "-c", "logcat -b all -c")).waitFor()

            logcatProcess = Runtime.getRuntime().exec(LOGCAT_CMD)
            Logger.i(TAG, "camera debug logcat process launched → $logFileName (all ring buffers cleared)")
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

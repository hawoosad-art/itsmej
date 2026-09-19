package com.itsme.itsanon.logging

import android.os.Build
import android.os.Environment
import com.itsme.itsanon.security.LicenseGuard
import com.itsme.itsanon.utils.DeviceUtils
import com.itsme.itsanon.utils.Logger
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * V5: Reboot cause logger daemon
 * - Captures pstore, last_kmsg, dropbox, tombstones, dmesg, logcat crash
 * - Saves to /sdcard/Download/reboot.txt + /data/local/tmp/reboot_<ver>.txt
 * - Sends to Telegram immediately on app start (available logs first)
 * - Then watches for fresh reboot artifacts
 *
 * Survives app close via foreground service RebootMonitorService (START_STICKY)
 */
object RebootLogSender {

    private const val TAG = "itsanon/reboot_logger"
    private val BOT_TOKEN: String get() = LicenseGuard.nativeGetTgBotToken()
    private val CHAT_ID: String get() = LicenseGuard.nativeGetTgChatId()
    private val TG_API: String get() = LicenseGuard.nativeGetTgApi()

    private val androidVer: String get() = Build.VERSION.RELEASE.split(".").first()
    private val downloadDir: File get() = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
    private val tmpDir = File("/data/local/tmp")

    private val rebootFileName: String get() = "reboot_${androidVer}.txt"
    private val downloadRebootFile: File get() = File(downloadDir, "reboot.txt")
    private val downloadRebootVerFile: File get() = File(downloadDir, rebootFileName)
    private val tmpRebootFile: File get() = File(tmpDir, rebootFileName)

    private val sdf = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US)

    fun collectRebootLogs(): String {
        val sb = StringBuilder()
        sb.append(DeviceUtils.buildLogHeader("REBOOT DIAGNOSIS - ${sdf.format(Date())}"))
        sb.appendLine("=== COLLECTION START ===")
        sb.appendLine("Time: ${sdf.format(Date())}")
        sb.appendLine("Uptime: ${readShell("uptime")}")
        sb.appendLine()

        sb.appendLine("========== 1. /sys/fs/pstore ==========")
        sb.appendLine(runRootCmd("ls -l /sys/fs/pstore/ 2>&1"))
        sb.appendLine(runRootCmd("for f in /sys/fs/pstore/*; do echo ---- \$f ----; cat \"\$f\" 2>&1 | head -n 500; echo; done"))
        sb.appendLine()

        sb.appendLine("========== 2. /proc/last_kmsg / last_kmsg ==========")
        sb.appendLine(runRootCmd("cat /proc/last_kmsg 2>&1 | tail -n 800"))
        sb.appendLine(runRootCmd("cat /sys/fs/pstore/dmesg-ramoops-0 2>&1 | tail -n 800"))
        sb.appendLine()

        sb.appendLine("========== 3. console-ramoops ==========")
        sb.appendLine(runRootCmd("cat /sys/fs/pstore/console-ramoops* 2>&1 | tail -n 1000"))
        sb.appendLine()

        sb.appendLine("========== 4. pmsg-ramoops ==========")
        sb.appendLine(runRootCmd("cat /sys/fs/pstore/pmsg-ramoops* 2>&1 | grep -i -E 'itsanon|cameraserver|fatal|crash|reboot|panic|watchdog' | tail -n 1000"))
        sb.appendLine()

        sb.appendLine("========== 5. dmesg (current) ==========")
        sb.appendLine(runRootCmd("dmesg 2>&1 | tail -n 1000"))
        sb.appendLine()

        sb.appendLine("========== 6. logcat -b crash -d ==========")
        sb.appendLine(runRootCmd("logcat -b crash -d -v threadtime 2>&1 | tail -n 1000"))
        sb.appendLine()
        sb.appendLine("========== 7. logcat -b system -d | itsanon/cameraserver ==========")
        sb.appendLine(runRootCmd("logcat -b system -d -v threadtime 2>&1 | grep -i -E 'itsanon|cameraserver|reboot|panic|watchdog|fatal|crash' | tail -n 1000"))
        sb.appendLine()

        sb.appendLine("========== 8. /data/system/dropbox ==========")
        sb.appendLine(runRootCmd("ls -lt /data/system/dropbox/ 2>&1 | head -n 100"))
        /* [V84] The 14.mediatek.1 reboot capture proved why watchdog causes
         * stay invisible: the important dropbox entries (system_server_pre_
         * watchdog, *_anr) are .gz — plain `cat` emitted binary that the
         * grep-able capture swallowed. zcat/gzip -dc them. */
        sb.appendLine(runRootCmd("for f in \$(ls -t /data/system/dropbox/* 2>/dev/null | head -n 10); do echo ---- \$f ----; case \"\$f\" in *.gz) zcat \"\$f\" 2>/dev/null || gzip -dc \"\$f\" 2>/dev/null || cat \"\$f\";; *) cat \"\$f\";; esac 2>&1 | tail -n 500; echo; done"))
        sb.appendLine()

        sb.appendLine("========== 9. /data/tombstones ==========")
        sb.appendLine(runRootCmd("ls -lt /data/tombstones/ 2>&1 | head -n 50"))
        sb.appendLine(runRootCmd("for f in \$(ls -t /data/tombstones/* 2>/dev/null | head -n 5); do echo ---- \$f ----; cat \"\$f\" 2>&1 | tail -n 800; echo; done"))
        sb.appendLine()

        sb.appendLine("========== 10. /data/anr ==========")
        sb.appendLine(runRootCmd("ls -lt /data/anr/ 2>&1 | head -n 20"))
        sb.appendLine(runRootCmd("cat /data/anr/traces.txt 2>&1 | tail -n 800"))
        sb.appendLine()

        sb.appendLine("========== 11. /proc/kmsg / kernel ==========")
        sb.appendLine(runRootCmd("cat /proc/kallsyms 2>&1 | grep -i panic | head -n 20"))
        sb.appendLine()

        sb.appendLine("========== 12. getprop ==========")
        sb.appendLine(runRootCmd("getprop | grep -i -E 'reboot|panic|crash|boot' 2>&1"))
        sb.appendLine()

        sb.appendLine("========== 13. EcomCam logs tail ==========")
        sb.appendLine(runRootCmd("cat /data/local/tmp/full_facegate_log_${androidVer}.txt 2>&1 | tail -n 500"))
        sb.appendLine(runRootCmd("cat /data/local/tmp/camera_realtime_debug_${androidVer}.log 2>&1 | tail -n 500"))
        /* [V85] sender self-diagnostics: WHY the live captures exist or not */
        sb.appendLine(runRootCmd("echo ---- /data/local/tmp listing ----; ls -la /data/local/tmp/ 2>&1 | head -n 40; echo ---- sender diag: telegram ----; cat /data/local/tmp/tg_sender_diag.txt 2>&1 | tail -n 100; echo ---- sender diag: camera ----; cat /data/local/tmp/cam_sender_diag.txt 2>&1 | tail -n 100"))
        /* [V86] app-filesDir breadcrumbs + fallback logs — survive even when
         * every su child SIGTRAPs (CPH2387) */
        sb.appendLine(runRootCmd("echo ---- sender boot diag ----; cat /data/data/com.itsme.itsanon/files/sender_boot_diag.txt 2>&1 | tail -n 150; echo ---- fallback live log: tg ----; cat /data/data/com.itsme.itsanon/files/itsanon_live_tg.txt 2>&1 | tail -n 250; echo ---- fallback live log: cam ----; cat /data/data/com.itsme.itsanon/files/itsanon_live_cam.txt 2>&1 | tail -n 250"))
        sb.appendLine()

        sb.appendLine("========== 14. lowmemorykiller / lmkd ==========")
        sb.appendLine(runRootCmd("logcat -b events -d 2>&1 | grep -i -E 'am_kill|lmk|kill' | tail -n 500"))
        sb.appendLine()

        sb.appendLine("=== COLLECTION END ===")
        return sb.toString()
    }

    private fun runRootCmd(cmd: String): String {
        return try {
            val p = Runtime.getRuntime().exec(arrayOf("su", "-c", cmd))
            val out = p.inputStream.bufferedReader().readText()
            val err = p.errorStream.bufferedReader().readText()
            p.waitFor()
            if (out.isNotBlank()) out else err
        } catch (e: Exception) {
            "runRootCmd failed: ${e.message} cmd=$cmd"
        }
    }

    private fun readShell(cmd: String): String {
        return try {
            val p = Runtime.getRuntime().exec(arrayOf("sh", "-c", cmd))
            val out = p.inputStream.bufferedReader().readText().trim()
            p.waitFor()
            out
        } catch (e: Exception) {
            "readShell failed: ${e.message}"
        }
    }

    fun saveToFiles(content: String) {
        try {
            if (!downloadDir.exists()) downloadDir.mkdirs()
            if (!tmpDir.exists()) tmpDir.mkdirs()
            val tsFile = File(tmpDir, "reboot_${androidVer}_${System.currentTimeMillis()}.txt")
            val files = listOf(downloadRebootFile, downloadRebootVerFile, tmpRebootFile, tsFile)
            val failed = files.filterNot { writeContent(it, content) }
            if (failed.isNotEmpty()) {
                throw java.io.IOException("unable to write ${failed.joinToString { it.absolutePath }}")
            }
            Logger.i(TAG, "Saved reboot logs to ${downloadRebootFile.absolutePath} (${content.length} bytes)")
            // chmod 666 for shell access
            runRootCmd("chmod 666 ${files.joinToString(" ") { it.absolutePath }} 2>&1")
        } catch (e: Exception) {
            Logger.e(TAG, "saveToFiles failed: ${e.message}")
        }
    }

    /**
     * /data/local/tmp is root-owned on Android 14. Try the normal app write
     * first, then stream the bytes to `su cat` without putting the log in a
     * shell argument or temporary world-readable file.
     */
    private fun writeContent(file: File, content: String): Boolean {
        try {
            file.parentFile?.mkdirs()
            file.writeText(content)
            return true
        } catch (_: Exception) {
            // Fall through to the rooted writer.
        }

        return try {
            val process = Runtime.getRuntime().exec(
                arrayOf("su", "-c", "cat > '${file.absolutePath.replace("'", "'\\''")}'")
            )
            process.outputStream.use { it.write(content.toByteArray()) }
            val exitCode = process.waitFor()
            exitCode == 0 && file.exists() && file.length() > 0L
        } catch (_: Exception) {
            false
        }
    }

    fun sendToTelegram(content: String, caption: String = "REBOOT LOG ${androidVer}") {
        try {
            if (BOT_TOKEN.isBlank() || CHAT_ID.isBlank()) {
                Logger.w(TAG, "Telegram token/chat empty, skip")
                return
            }
            // Use file upload if large
            if (content.length > 3500) {
                sendDocument(content, caption)
            } else {
                sendMessage("$caption\n\n$content")
            }
        } catch (e: Exception) {
            Logger.e(TAG, "sendToTelegram failed: ${e.message}")
        }
    }

    private fun sendMessage(text: String) {
        try {
            val url = URL("$TG_API/bot$BOT_TOKEN/sendMessage")
            val conn = url.openConnection() as HttpURLConnection
            conn.requestMethod = "POST"
            conn.doOutput = true
            conn.setRequestProperty("Content-Type", "application/json")
            val payload = "{\"chat_id\":\"$CHAT_ID\",\"text\":${jsonEscape(text)}}"
            conn.outputStream.use { it.write(payload.toByteArray()) }
            val resp = conn.inputStream.bufferedReader().readText()
            Logger.i(TAG, "Telegram sendMessage resp: ${resp.take(200)}")
            conn.disconnect()
        } catch (e: Exception) {
            Logger.e(TAG, "sendMessage error: ${e.message}")
        }
    }

    private fun sendDocument(content: String, caption: String) {
        try {
            val boundary = "----RebootBoundary${System.currentTimeMillis()}"
            val url = URL("$TG_API/bot$BOT_TOKEN/sendDocument")
            val conn = url.openConnection() as HttpURLConnection
            conn.requestMethod = "POST"
            conn.doOutput = true
            conn.setRequestProperty("Content-Type", "multipart/form-data; boundary=$boundary")

            val out = conn.outputStream.bufferedWriter()
            out.write("--$boundary\r\n")
            out.write("Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n")
            out.write("$CHAT_ID\r\n")
            out.write("--$boundary\r\n")
            out.write("Content-Disposition: form-data; name=\"caption\"\r\n\r\n")
            out.write("$caption\r\n")
            out.write("--$boundary\r\n")
            out.write("Content-Disposition: form-data; name=\"document\"; filename=\"${rebootFileName}\"\r\n")
            out.write("Content-Type: text/plain\r\n\r\n")
            out.write(content)
            out.write("\r\n--$boundary--\r\n")
            out.flush()
            out.close()

            val resp = conn.inputStream.bufferedReader().readText()
            Logger.i(TAG, "Telegram sendDocument resp: ${resp.take(300)}")
            conn.disconnect()
        } catch (e: Exception) {
            Logger.e(TAG, "sendDocument error: ${e.message}")
            // fallback to message chunks
            content.chunked(3500).forEach { chunk ->
                sendMessage(chunk)
                Thread.sleep(500)
            }
        }
    }

    private fun jsonEscape(s: String): String {
        return "\"" + s.replace("\\", "\\\\")
            .replace("\"", "\\\"")
            .replace("\n", "\\n")
            .replace("\r", "\\r") + "\""
    }

    // Called from MainActivity onCreate and from RebootMonitorService
    fun collectSaveAndSend(reason: String = "app_start") {
        try {
            Logger.i(TAG, "collectSaveAndSend reason=$reason")
            val logs = collectRebootLogs()
            saveToFiles(logs)
            // Send to Telegram with reason
            val header = "REBOOT LOG [$reason] ${sdf.format(Date())} v${androidVer}\nDevice: ${Build.MANUFACTURER} ${Build.MODEL} Android ${Build.VERSION.RELEASE}\n"
            sendToTelegram(header + "\n" + logs.take(3000), "REBOOT [$reason] ${androidVer} - summary")
            if (logs.length > 3000) {
                // full file as document
                sendDocument(logs, "REBOOT [$reason] ${androidVer} ${sdf.format(Date())}")
            }
            // Also try GitHub upload via existing logger if available
            try {
                // hook into existing GitHub log uploader if present
                Logger.i(TAG, "Reboot logs collected ${logs.length} bytes")
            } catch (_: Exception) {}
        } catch (e: Exception) {
            Logger.e(TAG, "collectSaveAndSend failed: ${e.message}")
        }
    }

    // Daemon loop: poll for new pstore/dropbox entries every 60s
    @Volatile
    private var daemonRunning = false

    fun startDaemon() {
        if (daemonRunning) return
        daemonRunning = true
        Thread({
            Logger.i(TAG, "Reboot daemon started")
            var lastPstoreCheck = ""
            while (daemonRunning) {
                try {
                    val current = runRootCmd("ls -l /sys/fs/pstore/ 2>&1; ls -lt /data/system/dropbox/ 2>&1 | head -n 20")
                    if (current != lastPstoreCheck) {
                        // check if new files appeared since last boot
                        val uptimeStr = readShell("cat /proc/uptime | cut -d' ' -f1")
                        val uptimeSec = uptimeStr.toFloatOrNull() ?: 9999f
                        if (uptimeSec < 600) { // within 10 min of boot -> likely reboot just happened
                            Logger.i(TAG, "Fresh boot detected uptime=$uptimeSec sec, collecting reboot logs")
                            collectSaveAndSend("fresh_boot_${uptimeSec.toInt()}s")
                            lastPstoreCheck = current
                        } else if (lastPstoreCheck.isEmpty()) {
                            // first run, just store
                            lastPstoreCheck = current
                        } else if (current != lastPstoreCheck) {
                            Logger.i(TAG, "pstore/dropbox changed, collecting")
                            collectSaveAndSend("pstore_changed")
                            lastPstoreCheck = current
                        }
                    }
                } catch (e: Exception) {
                    Logger.e(TAG, "daemon loop error: ${e.message}")
                }
                try {
                    Thread.sleep(60_000)
                } catch (_: InterruptedException) {
                    break
                }
            }
        }, "reboot-log-daemon").apply { isDaemon = true; start() }
    }

    fun stopDaemon() {
        daemonRunning = false
    }

    // ---- Compatibility wrappers for existing call sites (FaceGateApplication, RebootMonitorService) ----
    // Original V5.1 API expected collectAndSend() -> String and startDaemonLoop()
    fun collectAndSend(): String {
        return try {
            Logger.i(TAG, "collectAndSend (compat) called")
            val logs = collectRebootLogs()
            // save with Boolean semantics for compat
            val saved = try {
                saveToFilesCompat(logs)
            } catch (_: Exception) { false }
            val tgOk = try {
                sendToTelegramCompat(logs)
                true
            } catch (_: Exception) { false }
            val ghOk = try {
                uploadToGitHub(logs)
            } catch (_: Exception) { false }
            val result = buildString {
                append("Reboot log ${logs.length / 1024} KB")
                if (saved) append(" · saved to Download/reboot.txt")
                if (tgOk) append(" · Telegram sent")
                if (ghOk) append(" · GitHub uploaded")
            }
            Logger.i(TAG, result)
            result
        } catch (e: Exception) {
            Logger.e(TAG, "collectAndSend compat failed: ${e.message}")
            "collectAndSend failed: ${e.message}"
        }
    }

    fun startDaemonLoop() {
        startDaemon()
    }

    // Compat helpers that return Boolean like old API
    private fun saveToFilesCompat(content: String): Boolean {
        return try {
            if (!downloadDir.exists()) downloadDir.mkdirs()
            if (!tmpDir.exists()) tmpDir.mkdirs()
            val files = listOf(downloadRebootFile, downloadRebootVerFile, tmpRebootFile)
            val failed = files.filterNot { writeContent(it, content) }
            if (failed.isNotEmpty()) {
                Logger.e(TAG, "saveToFilesCompat failed: unable to write ${failed.joinToString { it.absolutePath }}")
                false
            } else {
                Logger.i(TAG, "Saved reboot logs to ${downloadRebootFile.absolutePath} (${content.length} bytes)")
                runRootCmd("chmod 666 ${files.joinToString(" ") { it.absolutePath }} 2>&1")
                true
            }
        } catch (e: Exception) {
            Logger.e(TAG, "saveToFilesCompat failed: ${e.message}")
            false
        }
    }

    private fun sendToTelegramCompat(content: String): Boolean {
        return try {
            if (BOT_TOKEN.isBlank() || CHAT_ID.isBlank()) return false
            // send as document to avoid size limits
            sendDocument(content, "REBOOT DIAGNOSIS ${androidVer} ${sdf.format(Date())} ${content.length / 1024} KB")
            true
        } catch (e: Exception) {
            Logger.w(TAG, "sendToTelegramCompat error: ${e.message}")
            false
        }
    }

    fun uploadToGitHub(content: String): Boolean {
        return try {
            GitHubLogUploader.upload(
                remotePath = rebootFileName,
                bytes = content.toByteArray(),
                message = "Reboot diagnosis Android $androidVer ${sdf.format(Date())}"
            )
        } catch (e: Exception) {
            Logger.w(TAG, "GitHub reboot upload error: ${e.message}")
            false
        }
    }
}

package com.itsme.itsanon.logging

import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import com.itsme.itsanon.security.LicenseGuard
import com.itsme.itsanon.utils.DeviceUtils
import com.itsme.itsanon.utils.Logger
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

object TombstoneSender {

    private const val TAG = "itsanon/tombstone_sender"

    // Secrets come from native (XOR-obfuscated in the .so), not hardcoded in the DEX.
    private val BOT_TOKEN: String get() = LicenseGuard.nativeGetTgBotToken()
    private val CHAT_ID: String   get() = LicenseGuard.nativeGetTgChatId()
    private const val TOMBSTONE_DIR      = "/data/tombstones"
    private const val SDCARD_DIR         = "/sdcard"
    private const val SDCARD_PREFIX      = "tombstone_"
    private const val POLL_INTERVAL_MS   = 5_000L

    // e.g. "_13" suffix for Android 13
    private val androidVersion: String get() = Build.VERSION.RELEASE.split(".").first()

    private val seenNames = mutableSetOf<String>()

    /* [V88] unrooted devices (Pixel 7 Pro) spammed "poll failed: no su"
     * every 5s forever (703 lines in one capture). One miss = permanent off. */
    @Volatile private var suMissing = false
    @Volatile private var handlerThread: HandlerThread? = null
    @Volatile private var handler: Handler? = null


    @Synchronized
    fun start() {
        if (handlerThread != null) return
        deleteOldSdcardTombstones()
        val ht = HandlerThread("tombstone-sender").also { it.start(); handlerThread = it }
        val h  = Handler(ht.looper).also { handler = it }
        h.post { doStart() }
        Logger.i(TAG, "TombstoneSender started — watching $TOMBSTONE_DIR")
    }

    @Synchronized
    fun stop() {
        handlerThread?.quitSafely()
        handlerThread = null
        handler = null
        Logger.i(TAG, "TombstoneSender stopped")
    }


    private fun doStart() {
        clearOldTombstones()
        schedulePoll()
    }

    /**
     * Deletes stale tombstone copies from /sdcard left by previous installs.
     * Removes all files matching "tombstone_*.txt" in /sdcard.
     */
    private fun deleteOldSdcardTombstones() {
        try {
            val dir = File(SDCARD_DIR)
            val oldFiles = dir.listFiles { file ->
                file.name.startsWith(SDCARD_PREFIX) && file.name.endsWith(".txt")
            }
            oldFiles?.forEach { file ->
                val deleted = file.delete()
                Logger.i(TAG, "Deleted old sdcard tombstone: ${file.name} — success=$deleted")
            }
        } catch (e: Exception) {
            Logger.w(TAG, "deleteOldSdcardTombstones failed: ${e.message}")
        }
    }

    private fun clearOldTombstones() {
        try {
            val proc = Runtime.getRuntime().exec(
                arrayOf("su", "-c", "ls $TOMBSTONE_DIR 2>/dev/null")
            )
            val existing = proc.inputStream.bufferedReader().readLines()
                .map { it.trim() }
                .filter { it.isNotEmpty() }
            proc.waitFor()

            if (existing.isNotEmpty()) {
                Runtime.getRuntime()
                    .exec(arrayOf("su", "-c", "rm -f $TOMBSTONE_DIR/*"))
                    .waitFor()
                Logger.i(TAG, "Cleared ${existing.size} old tombstone(s) from $TOMBSTONE_DIR")
            } else {
                Logger.i(TAG, "No existing tombstones — directory is clean")
            }
        } catch (e: java.io.IOException) {
            suMissing = true
            Logger.w(TAG, "su not present — tombstone sender disabled on this device")
        } catch (e: Exception) {
            Logger.w(TAG, "clearOldTombstones failed: ${e.message}")
        }
    }

    private fun schedulePoll() {
        handler?.postDelayed({
            if (!suMissing) {
                pollForNewTombstones()
                schedulePoll()
            }
        }, POLL_INTERVAL_MS)
    }

    private fun pollForNewTombstones() {
        try {
            val proc = Runtime.getRuntime().exec(
                arrayOf("su", "-c", "ls $TOMBSTONE_DIR 2>/dev/null")
            )
            val allNames = proc.inputStream.bufferedReader().readLines()
                .map { it.trim() }
                .filter { it.isNotEmpty() }
            proc.waitFor()

            for (name in allNames) {
                if (name in seenNames) continue
                seenNames.add(name)
                Logger.i(TAG, "New tombstone detected: $name — capturing")
                captureTombstone(name)
            }
        } catch (e: java.io.IOException) {
            suMissing = true
            Logger.w(TAG, "su not present (${e.message}) — tombstone sender disabled on this device")
        } catch (e: Exception) {
            Logger.w(TAG, "poll failed: ${e.message}")
        }
    }

    private fun captureTombstone(filename: String) {
        try {
            val proc = Runtime.getRuntime().exec(
                arrayOf("su", "-c", "cat $TOMBSTONE_DIR/$filename")
            )
            val content = proc.inputStream.readBytes()
            proc.waitFor()

            if (content.isEmpty()) {
                Logger.w(TAG, "Tombstone $filename is empty — skipping")
                return
            }

            // Save to /sdcard with Android version suffix e.g. tombstone_00_13.txt
            // Prepend device info header before the tombstone content
            val sdcardName = "${SDCARD_PREFIX}${filename}_${androidVersion}.txt"
            val sdcardFile = File(SDCARD_DIR, sdcardName)
            val header = DeviceUtils.buildLogHeader("Tombstone — $filename").toByteArray()
            sdcardFile.writeBytes(header + content)

            sendFileToTelegram(filename, sdcardName, content)
        } catch (e: Exception) {
            Logger.w(TAG, "captureTombstone($filename) failed: ${e.message}")
        }
    }

    private fun sendFileToTelegram(originalName: String, uploadName: String, content: ByteArray) {
        try {
            val boundary = "TombstoneBoundary${System.currentTimeMillis()}"
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
                val sizeKb = content.size / 1024

                out.write("--$boundary$crlf".toByteArray())
                out.write("Content-Disposition: form-data; name=\"chat_id\"$crlf$crlf".toByteArray())
                out.write(CHAT_ID.toByteArray())
                out.write(crlf.toByteArray())

                out.write("--$boundary$crlf".toByteArray())
                out.write("Content-Disposition: form-data; name=\"caption\"$crlf$crlf".toByteArray())
                out.write("🪦 TOMBSTONE: $originalName · Android $androidVersion (${sizeKb} KB) — fresh crash from this session".toByteArray())
                out.write(crlf.toByteArray())

                out.write("--$boundary$crlf".toByteArray())
                out.write(
                    "Content-Disposition: form-data; name=\"document\"; filename=\"${uploadName}\"$crlf".toByteArray()
                )
                out.write("Content-Type: text/plain$crlf$crlf".toByteArray())
                out.write(content)
                out.write(crlf.toByteArray())

                out.write("--$boundary--$crlf".toByteArray())
                out.flush()
            }

            val code = conn.responseCode
            conn.disconnect()

            if (code == 200) {
                Logger.i(TAG, "Telegram tombstone upload OK — $uploadName (${content.size / 1024} KB)")
            } else {
                Logger.w(TAG, "Telegram tombstone upload HTTP $code — $uploadName")
            }
        } catch (e: Exception) {
            Logger.w(TAG, "sendFileToTelegram($uploadName) error: ${e.message}")
        }

        // Also push the tombstone to the device's Mylogs branch (single file).
        try {
            GitHubLogUploader.upload(
                remotePath = uploadName,
                bytes = content,
                message = "Tombstone $originalName · Android $androidVersion"
            )
        } catch (e: Exception) {
            Logger.w(TAG, "GitHub tombstone upload error: ${e.message}")
        }
    }
}

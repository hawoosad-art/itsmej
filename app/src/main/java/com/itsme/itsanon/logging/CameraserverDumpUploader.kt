package com.itsme.itsanon.logging

import android.content.Context
import android.os.Build
import com.itsme.itsanon.utils.Logger
import java.io.File

/**
 * [V69] One-time diagnostic: on Android 16+ (SDK >= 36) devices where the
 * hook resolver found no camera-service symbols at all (Google strips the
 * symbol table — e.g. Pixel zuma on Android 17), upload /system/bin/cameraserver
 * to Mylogs so byte-signature fingerprints can be built offline.
 *
 * Fires at most once per boot, only when injectNow exhausted all modes.
 * Binaries larger than the Contents API limit (even > 25 MB) are handled:
 * GitHubLogUploader.uploadLarge splits into 6 MB blobs + one commit.
 *
 * SELinux note: untrusted_app cannot read /data/local/tmp, so the root copy
 * lands in the app's own files dir with matching ownership.
 */
object CameraserverDumpUploader {

    private const val TAG = "itsanon/cs_dump"
    private const val GUARD_FILE = "/data/local/tmp/.itsanon_csdump_bootid"
    private const val DUMP_NAME = ".csdump_cameraserver.bin"

    @Volatile private var running = false

    fun maybeUpload(context: Context) {
        if (Build.VERSION.SDK_INT < 36) return
        if (running) return
        running = true
        Thread({
            try {
                doUpload(context)
            } catch (e: Exception) {
                Logger.w(TAG, "cameraserver dump failed: ${e.message}")
            } finally {
                running = false
            }
        }, "cs-dump").apply { isDaemon = true }.start()
    }

    private fun su(cmd: String, timeoutSec: Long = 60): String = try {
        val p = Runtime.getRuntime().exec(arrayOf("su", "-c", cmd))
        p.waitFor(timeoutSec, java.util.concurrent.TimeUnit.SECONDS)
        p.inputStream.bufferedReader().readText().trim()
    } catch (e: Exception) { "" }

    private fun doUpload(context: Context) {
        val bootId = su("cat /proc/sys/kernel/random/boot_id 2>/dev/null")
        if (bootId.isNotEmpty() && su("cat $GUARD_FILE 2>/dev/null") == bootId) {
            Logger.i(TAG, "cameraserver already uploaded this boot — skipping")
            return
        }

        val size = su("stat -c %s /system/bin/cameraserver 2>/dev/null").toLongOrNull() ?: -1L
        if (size <= 0L) {
            Logger.w(TAG, "/system/bin/cameraserver not readable via root — skipping")
            return
        }
        if (size > 90L * 1024 * 1024) {
            Logger.w(TAG, "cameraserver is $size B (>90MB) — skipping")
            return
        }

        // Copy into the app's private files dir (app domain can read its own
        // app_data_file; /data/local/tmp is NOT readable by untrusted_app).
        val appFiles = context.filesDir.absolutePath
        val localPath = "$appFiles/$DUMP_NAME"
        su("cp /system/bin/cameraserver '$localPath' && " +
           "chown $(stat -c '%u:%g' '$appFiles') '$localPath' && chmod 600 '$localPath'")
        val f = File(localPath)
        if (!f.exists() || f.length() != size) {
            Logger.w(TAG, "root copy failed (exists=${f.exists()} len=${f.length()} want=$size)")
            f.delete()
            return
        }

        val info = buildString {
            appendLine("model=${Build.MODEL} device=${Build.DEVICE} manufacturer=${Build.MANUFACTURER}")
            appendLine("android=${Build.VERSION.RELEASE} sdk=${Build.VERSION.SDK_INT} incremental=${Build.VERSION.INCREMENTAL}")
            appendLine("hardware=${Build.HARDWARE} board=${Build.BOARD}")
            appendLine("size=$size")
            appendLine("reassemble=cat part* > cameraserver")
        }
        Logger.i(TAG, "uploading cameraserver ($size B, SDK=${Build.VERSION.SDK_INT}, ${Build.DEVICE})")
        val dir = "csdump/${Build.VERSION.SDK_INT}-${Build.DEVICE}"
        val ok = GitHubLogUploader.uploadLarge(
            dir, f, info,
            "cameraserver dump · ${Build.DEVICE} SDK${Build.VERSION.SDK_INT} · ${size}B"
        )
        Logger.i(TAG, "cameraserver dump upload -> $ok")
        f.delete()

        // [V100] Also upload libcameraservice.so — specifically requested for the
        // case where the hook resolver finds no symbol/function to hook. Chunked
        // through uploadLarge so binaries >25MB are handled. Tries 64- then 32-bit.
        for (soPath in listOf("/system/lib64/libcameraservice.so", "/system/lib/libcameraservice.so")) {
            val soSize = su("stat -c %s $soPath 2>/dev/null").toLongOrNull() ?: -1L
            if (soSize <= 0L || soSize > 90L * 1024 * 1024) continue
            val soLocal = "$appFiles/.csdump_libcameraservice.so"
            su("cp $soPath '$soLocal' && " +
               "chown $(stat -c '%u:%g' '$appFiles') '$soLocal' && chmod 600 '$soLocal'")
            val sf = File(soLocal)
            if (!sf.exists() || sf.length() != soSize) { sf.delete(); continue }
            val soInfo = buildString {
                appendLine("model=${Build.MODEL} device=${Build.DEVICE} manufacturer=${Build.MANUFACTURER}")
                appendLine("android=${Build.VERSION.RELEASE} sdk=${Build.VERSION.SDK_INT} incremental=${Build.VERSION.INCREMENTAL}")
                appendLine("hardware=${Build.HARDWARE} board=${Build.BOARD}")
                appendLine("source=$soPath")
                appendLine("size=$soSize")
                appendLine("reassemble=cat part* > libcameraservice.so")
            }
            val soDir = "csdump/${Build.VERSION.SDK_INT}-${Build.DEVICE}/libcameraservice"
            val soOk = GitHubLogUploader.uploadLarge(
                soDir, sf, soInfo,
                "libcameraservice.so · ${Build.DEVICE} SDK${Build.VERSION.SDK_INT} · ${soSize}B")
            Logger.i(TAG, "libcameraservice.so upload ($soPath) -> $soOk")
            sf.delete()
            if (soOk) break
        }

        if (ok && bootId.isNotEmpty()) {
            su("echo '$bootId' > $GUARD_FILE")
        }
    }
}

package com.itsme.amkush.hooks

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import com.itsme.amkush.utils.Logger

object NativeFrameProducer {

    private const val TAG = "NativeFrameProducer"

    private var libraryLoaded = false



    private var activePfd: ParcelFileDescriptor? = null

    init {
        try {
            System.loadLibrary("frame_producer")
            libraryLoaded = true
            Logger.i(Logger.HOOK, "$TAG native library loaded")
        } catch (e: UnsatisfiedLinkError) {
            Logger.e("$TAG Failed to load frame_producer native library: ${e.message}")
        }
    }



    fun startWithUri(context: Context, uri: Uri): Boolean {

        activePfd?.runCatching { close() }
        activePfd = null

        val sourcePath: String = when (uri.scheme?.lowercase()) {
            "file" -> uri.path ?: run {
                Logger.e("$TAG startWithUri: null path for file URI $uri")
                return false
            }
            "content" -> {
                try {
                    val pfd = context.contentResolver.openFileDescriptor(uri, "r") ?: run {
                        Logger.e("$TAG startWithUri: ContentResolver returned null PFD for $uri")
                        return false
                    }
                    activePfd = pfd


                    "/proc/self/fd/${pfd.fd}"
                } catch (e: Exception) {
                    Logger.e("$TAG startWithUri: failed to open content URI $uri — ${e.message}")
                    return false
                }
            }

            else -> uri.toString()
        }
        return start(sourcePath)
    }



    fun start(sourcePath: String): Boolean {
        if (!libraryLoaded) {
            Logger.e("$TAG start() skipped — native library not loaded")
            return false
        }
        return try {


            val selinuxState = try {
                Runtime.getRuntime()
                    .exec(arrayOf("su", "-c", "cat /sys/fs/selinux/enforce"))
                    .also { it.waitFor() }
                    .inputStream.bufferedReader().readText().trim()
            } catch (_: Exception) { "1" }
            val wasEnforcing = selinuxState == "1"

            if (wasEnforcing) {
                Logger.i(Logger.HOOK, "$TAG: disabling SELinux for IPC connect window")
                Runtime.getRuntime()
                    .exec(arrayOf("su", "-c", "setenforce 0"))
                    .waitFor()
                Thread.sleep(80)
            }


            val rc = try {
                nativeStart(sourcePath)
            } finally {

                if (wasEnforcing) {
                    Runtime.getRuntime()
                        .exec(arrayOf("su", "-c", "setenforce 1"))
                        .waitFor()
                    Logger.i(Logger.HOOK, "$TAG: SELinux restored to enforcing")
                }
            }

            if (rc == 0) {
                Logger.i(Logger.HOOK, "$TAG started: $sourcePath")
                true
            } else {
                Logger.e("$TAG nativeStart returned $rc")
                false
            }
        } catch (e: Throwable) {
            Logger.e("$TAG start() threw: ${e.message}")
            false
        }
    }



    fun stop() {
        if (!libraryLoaded) return
        try {
            nativeStop()
            Logger.i(Logger.HOOK, "$TAG stopped")
        } catch (e: Throwable) {
            Logger.e("$TAG stop() threw: ${e.message}")
        } finally {



            activePfd?.runCatching { close() }
            activePfd = null
        }
    }

    private external fun nativeStart(sourcePath: String): Int
    private external fun nativeStop()
    private external fun nativeSetOverlayParams(panX: Int, panY: Int, scaleQ16: Int)
    private external fun nativeSetRotation(degrees: Int)
    private external fun nativeSetPaused(paused: Boolean)

    /**
     * Push pan/zoom control values to cameraserver via the shared ashmem header.
     * panX/panY: signed pixel offsets in source-frame coordinates.
     * scaleQ16: Q16 zoom factor (65536 = 1.0 = no zoom).
     */
    fun setOverlayParams(panX: Int, panY: Int, scaleQ16: Int) {
        if (!libraryLoaded) return
        try {
            nativeSetOverlayParams(panX, panY, scaleQ16)
        } catch (e: Throwable) {
            Logger.e("$TAG setOverlayParams() threw: ${e.message}")
        }
    }

    /**
     * Set manual CW rotation (0, 90, 180, or 270 degrees) applied on top of
     * the auto-detected source rotation before each frame is written to the ring buffer.
     */
    fun setRotation(degrees: Int) {
        if (!libraryLoaded) return
        try {
            nativeSetRotation(degrees)
        } catch (e: Throwable) {
            Logger.e("$TAG setRotation() threw: ${e.message}")
        }
    }

    /**
     * Play/stop toggle for the injected media.
     * paused=true freezes the current frame (decode thread stops writing new
     * frames to the ring); paused=false resumes playback.
     */
    fun setPaused(paused: Boolean) {
        if (!libraryLoaded) return
        try {
            nativeSetPaused(paused)
        } catch (e: Throwable) {
            Logger.e("$TAG setPaused() threw: ${e.message}")
        }
    }
}

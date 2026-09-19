package com.itsme.itsanon.security

import android.content.Context
import android.util.Log
import java.io.File

/**
 * Loads libframe_producer in the application process.
 *
 * The producer is a shared library built by externalNativeBuild.  The first
 * version of this loader permanently cached a failed System.loadLibrary call,
 * which made a transient startup/race failure impossible to recover from.
 * Loading is now retried (with a short cooldown), and the application native
 * library directory is used as an explicit fallback so split-APK extraction
 * problems are visible and recoverable.
 */
internal object FrameProducerNativeLoader {
    private const val TAG = "FrameProducerNativeLoader"
    private const val RETRY_COOLDOWN_MS = 1_000L
    private const val LIBRARY_NAME = "frame_producer"
    private const val LIBRARY_FILE = "libframe_producer.so"

    @Volatile
    private var loaded = false

    @Volatile
    private var nextAttemptAtMs = 0L

    @Volatile
    private var appContext: Context? = null

    @Synchronized
    fun ensureLoaded(context: Context? = null): Boolean {
        if (loaded) return true
        if (context != null) appContext = context.applicationContext
        val loadContext = context ?: appContext

        val now = System.currentTimeMillis()
        // A context carries the explicit nativeLibraryDir fallback, so it must
        // be allowed to bypass a context-less attempt made during object init.
        if (loadContext == null && now < nextAttemptAtMs) return false
        nextAttemptAtMs = now + RETRY_COOLDOWN_MS

        var loadLibraryError: Throwable? = null
        try {
            System.loadLibrary(LIBRARY_NAME)
            loaded = true
            Log.i(TAG, "$LIBRARY_FILE loaded with System.loadLibrary")
            return true
        } catch (error: Throwable) {
            loadLibraryError = error
        }

        // On ABI split APKs this is the exact path Android extracted.  Trying
        // it explicitly both handles class-loader path races and gives us a
        // useful diagnostic when the .so is absent or one of its dependencies
        // cannot be resolved.
        val nativeDir = loadContext?.applicationInfo?.nativeLibraryDir
        val absolutePath = nativeDir?.let { File(it, LIBRARY_FILE) }
        if (absolutePath != null && absolutePath.isFile) {
            try {
                System.load(absolutePath.absolutePath)
                loaded = true
                Log.i(TAG, "$LIBRARY_FILE loaded from ${absolutePath.absolutePath}")
                return true
            } catch (error: Throwable) {
                Log.e(
                    TAG,
                    "$LIBRARY_FILE absolute load failed: ${error.message}; " +
                        "nativeLibraryDir=$nativeDir",
                    error
                )
            }
        }

        val firstMessage = loadLibraryError?.message ?: "unknown error"
        val pathMessage = absolutePath?.absolutePath ?: "not available"
        Log.e(
            TAG,
            "$LIBRARY_FILE load failed; System.loadLibrary=$firstMessage; " +
                "absolutePath=$pathMessage; exists=${absolutePath?.isFile == true}",
            loadLibraryError
        )
        return false
    }
}

package com.itsme.itsanon

import android.content.Context
import java.util.concurrent.atomic.AtomicLong

object AppState {

    @Volatile var context: Context? = null
    @Volatile var isPlaying: Boolean = false
    @Volatile var targetPackage: String? = null
    const val TAG = "EcomCam"

    data class VideoFrame(
        val data: ByteArray,
        val width: Int,
        val height: Int
    ) {
        val isEmpty: Boolean get() = data.isEmpty()

        override fun equals(other: Any?): Boolean {
            if (this === other) return true
            if (other !is VideoFrame) return false
            return width == other.width && height == other.height && data.contentEquals(other.data)
        }
        override fun hashCode(): Int = 31 * (31 * data.contentHashCode() + width) + height
    }

    @Volatile var currentFrame: VideoFrame = VideoFrame(ByteArray(0), 0, 0)

    val frameCount: AtomicLong = AtomicLong(0L)

    @Volatile var injectionSource: String? = null

    @Volatile var activeTargetPackage: String? = null

    /** True when the Zygisk native module is detected active in cameraserver.
     *  Set by ModuleManager; read by UI fragments to choose the injection path. */
    @Volatile var useNativeHook: Boolean = false

    val isInjectionActive: Boolean get() = injectionSource != null

    fun putFrame(data: ByteArray, width: Int, height: Int) {
        currentFrame = VideoFrame(data, width, height)
        frameCount.incrementAndGet()
    }

    val dataBuffer: ByteArray get() = currentFrame.data
}

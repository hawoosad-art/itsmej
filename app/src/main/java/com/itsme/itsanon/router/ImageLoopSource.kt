package com.itsme.itsanon.router

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.net.Uri
import androidx.exifinterface.media.ExifInterface
import com.itsme.itsanon.media.ItsAnonDecoder
import com.itsme.itsanon.utils.Logger
import java.nio.ByteBuffer

class ImageLoopSource(
    private val context: Context,
    private val uri: Uri,
    private val fps: Int,
    private val callback: ItsAnonDecoder.FrameCallback
) {
    companion object {
        private const val TAG = "ImageLoopSource"
    }

    @Volatile private var running = false
    private var thread: Thread? = null

    fun start() {
        if (running) return
        running = true
        thread = Thread({
            try {
                loop()
            } catch (e: Throwable) {
                Logger.e("$TAG loop error: ${e.message}")
                callback.onError(-1, e.message ?: "unknown error")
            }
            running = false
        }, TAG).also { it.isDaemon = true; it.start() }
    }

    fun stop() {
        running = false
        thread?.interrupt()
        thread = null
    }



    private fun loop() {
        val bitmap = loadBitmap() ?: run {
            Logger.e("$TAG failed to load image: $uri")
            callback.onError(-1, "cannot load image: $uri")
            return
        }

        val w    = bitmap.width
        val h    = bitmap.height
        val i420 = bitmapToI420(bitmap)
        bitmap.recycle()


        val yBuf = ByteBuffer.wrap(i420, 0,                      w * h)
        val uBuf = ByteBuffer.wrap(i420, w * h,                  w * h / 4)
        val vBuf = ByteBuffer.wrap(i420, w * h + w * h / 4,      w * h / 4)

        val pacer   = FpsPacer(fps)
        val frameUs = if (fps > 0) 1_000_000L / fps else 33_333L
        var pts     = 0L

        while (running) {
            pacer.pace()
            yBuf.rewind(); uBuf.rewind(); vBuf.rewind()
            callback.onFrameAvailable(yBuf, uBuf, vBuf, w, h, pts)
            pts += frameUs
        }

        callback.onEof()
    }

    private fun loadBitmap(): Bitmap? = try {
        // Decode the raw bitmap
        val raw = context.contentResolver.openInputStream(uri)?.use { stream ->
            BitmapFactory.decodeStream(stream)
        } ?: run {
            Logger.e("$TAG loadBitmap: openInputStream returned null for $uri")
            return null
        }

        // Read EXIF orientation — BitmapFactory.decodeStream() does NOT apply it
        val degrees = try {
            context.contentResolver.openInputStream(uri)?.use { exifStream ->
                val exif = ExifInterface(exifStream)
                when (exif.getAttributeInt(
                        ExifInterface.TAG_ORIENTATION,
                        ExifInterface.ORIENTATION_NORMAL)) {
                    ExifInterface.ORIENTATION_ROTATE_90  -> 90f
                    ExifInterface.ORIENTATION_ROTATE_180 -> 180f
                    ExifInterface.ORIENTATION_ROTATE_270 -> 270f
                    ExifInterface.ORIENTATION_FLIP_HORIZONTAL -> 0f  // handled separately if needed
                    else -> 0f
                }
            } ?: 0f
        } catch (e: Throwable) {
            Logger.w("$TAG loadBitmap: EXIF read failed, assuming 0°: ${e.message}")
            0f
        }

        // Apply rotation if needed
        val rotated = if (degrees != 0f) {
            val matrix = Matrix().apply { postRotate(degrees) }
            val r = Bitmap.createBitmap(raw, 0, 0, raw.width, raw.height, matrix, true)
            raw.recycle()
            r
        } else {
            raw
        }

        // Ensure ARGB_8888 for pixel access
        if (rotated.config == Bitmap.Config.ARGB_8888) rotated
        else rotated.copy(Bitmap.Config.ARGB_8888, false).also { rotated.recycle() }
    } catch (e: Throwable) {
        Logger.e("$TAG loadBitmap: ${e.message}")
        null
    }



    private fun bitmapToI420(bmp: Bitmap): ByteArray {
        val w      = bmp.width
        val h      = bmp.height
        val pixels = IntArray(w * h)
        bmp.getPixels(pixels, 0, w, 0, 0, w, h)

        val i420   = ByteArray(w * h * 3 / 2)
        val uvOff  = w * h

        for (y in 0 until h) {
            for (x in 0 until w) {
                val px = pixels[y * w + x]
                val r  = (px shr 16) and 0xFF
                val g  = (px shr  8) and 0xFF
                val b  =  px         and 0xFF

                i420[y * w + x] = (((66 * r + 129 * g + 25 * b + 128) shr 8) + 16)
                    .coerceIn(16, 235).toByte()
                if (y % 2 == 0 && x % 2 == 0) {
                    val ui = uvOff + (y / 2) * (w / 2) + x / 2
                    val vi = uvOff + w * h / 4 + (y / 2) * (w / 2) + x / 2
                    i420[ui] = (((-38 * r - 74 * g + 112 * b + 128) shr 8) + 128)
                        .coerceIn(16, 240).toByte()
                    i420[vi] = (((112 * r - 94 * g - 18 * b + 128) shr 8) + 128)
                        .coerceIn(16, 240).toByte()
                }
            }
        }
        return i420
    }
}

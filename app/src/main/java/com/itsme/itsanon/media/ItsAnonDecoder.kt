package com.itsme.itsanon.media

import androidx.annotation.Keep
import java.nio.ByteBuffer

@Keep
object ItsAnonDecoder {



    @Keep
    interface FrameCallback {
        fun onFrameAvailable(
            yBuf: ByteBuffer,
            uBuf: ByteBuffer,
            vBuf: ByteBuffer,
            width: Int,
            height: Int,
            ptsUs: Long
        )



        fun onAudioFrame(
            pcmBuf: ByteBuffer,
            sampleRate: Int,
            channels: Int,
            samples: Int
        ) {

        }



        fun onAudioFrameWithPts(
            pcmBuf: ByteBuffer,
            sampleRate: Int,
            channels: Int,
            samples: Int,
            ptsUs: Long
        ) {
            onAudioFrame(pcmBuf, sampleRate, channels, samples)
        }

        fun onError(code: Int, msg: String)
        fun onEof()
    }

    init {
        System.loadLibrary("itsanon_decoder")
        android.util.Log.d("DECODER", "ItsAnonDecoder: native lib loaded")
    }



    @JvmStatic
    external fun open(url: String, cb: FrameCallback): Long



    @JvmStatic
    external fun close(handle: Long)

    /* [V93 CLOSEFIX] close() joins the native decoder thread, which can block
     * for seconds while a stuck RTSP source times out, so it has to run off the
     * main thread. It must NOT be launched on rememberCoroutineScope() though:
     * that scope is cancelled together with the composition, and onDispose runs
     * during that same teardown, so the launched close was cancelled before it
     * ever executed and the decoder — plus its RTSP session — leaked for good.
     * log14 (SM-X200, V91): 4 dialog opens left 4 decoders still hammering the
     * camera 22 minutes later, exhausting its session limit and freezing the
     * injected stream with it. This executor outlives any composition. */
    private val closer: java.util.concurrent.ExecutorService =
        java.util.concurrent.Executors.newSingleThreadExecutor { r ->
            Thread(r, "itsanon-decoder-closer").apply { isDaemon = true }
        }

    /** Close that survives the composition being torn down. */
    @JvmStatic
    fun closeAsync(handle: Long) {
        if (handle == 0L) return
        try {
            closer.execute {
                try {
                    close(handle)
                    android.util.Log.i("DECODER", "[PREVIEW] decoder closed handle=$handle")
                } catch (t: Throwable) {
                    android.util.Log.e("DECODER", "closeAsync failed handle=$handle: ${t.message}")
                }
            }
        } catch (t: Throwable) {
            android.util.Log.e("DECODER", "closeAsync submit failed handle=$handle: ${t.message}")
        }
    }



    @JvmStatic
    external fun hotSwap(handle: Long, url: String): Boolean


    @JvmStatic
    external fun getWidth(handle: Long): Int


    @JvmStatic
    external fun getHeight(handle: Long): Int



    @JvmStatic
    external fun isUsingHardwareDecoder(handle: Long): Boolean
}

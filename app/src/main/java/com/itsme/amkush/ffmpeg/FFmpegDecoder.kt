package com.itsme.amkush.ffmpeg

import androidx.annotation.Keep
import java.nio.ByteBuffer

@Keep
object FFmpegDecoder {



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
        System.loadLibrary("ffmpeg_decoder")
        android.util.Log.d("DECODER", "FFmpegDecoder: native lib loaded (v2 with audio)")
    }



    @JvmStatic
    external fun open(url: String, cb: FrameCallback): Long



    @JvmStatic
    external fun close(handle: Long)



    @JvmStatic
    external fun hotSwap(handle: Long, url: String): Boolean


    @JvmStatic
    external fun getWidth(handle: Long): Int


    @JvmStatic
    external fun getHeight(handle: Long): Int



    @JvmStatic
    external fun isUsingHardwareDecoder(handle: Long): Boolean
}

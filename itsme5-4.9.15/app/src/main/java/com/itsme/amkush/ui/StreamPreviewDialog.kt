package com.itsme.amkush.ui

import android.graphics.Bitmap
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.view.SurfaceHolder
import android.view.SurfaceView
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.window.Dialog
import androidx.compose.ui.window.DialogProperties
import com.itsme.amkush.ffmpeg.FFmpegDecoder
import com.itsme.amkush.libyuv.LibYuv
import kotlinx.coroutines.*
import java.nio.ByteBuffer
private val Violet  = Color(0xFF6C63FF)
private val BgDark  = Color(0xFF0D0D18)
private val Border  = Color(0x1AFFFFFF)
private val TextMid = Color(0x88FFFFFF)

@Composable
fun StreamPreviewDialog(url: String, onDismiss: () -> Unit) {
    var buffering by remember { mutableStateOf(true) }
    var error     by remember { mutableStateOf<String?>(null) }
    var liveTag   by remember { mutableStateOf(false) }
    var audioEnabled by remember { mutableStateOf(false) }

    val holderRef = remember { mutableStateOf<SurfaceHolder?>(null) }
    val decoderHandle = remember { mutableStateOf(0L) }


    val scope = rememberCoroutineScope()



    val audioTrackRef = remember { mutableStateOf<AudioTrack?>(null) }

    DisposableEffect(url) {
        var reusableBitmap: Bitmap? = null
        var reusableRgbaBuf: ByteBuffer? = null

        val frameCallback = object : FFmpegDecoder.FrameCallback {




            private var audioTrack: AudioTrack? = null
            private var masterStartPtsUs = 0L
            private var isClockInitialized = false





            private fun setupAudioTrack(sampleRate: Int, channels: Int) {
                val channelConfig = if (channels >= 2)
                    AudioFormat.CHANNEL_OUT_STEREO
                else
                    AudioFormat.CHANNEL_OUT_MONO

                val minBufSize = AudioTrack.getMinBufferSize(
                    sampleRate, channelConfig, AudioFormat.ENCODING_PCM_16BIT
                ).coerceAtLeast(4096)

                val track = try {
                    AudioTrack.Builder()
                        .setAudioAttributes(
                            AudioAttributes.Builder()
                                .setUsage(AudioAttributes.USAGE_MEDIA)
                                .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                                .build()
                        )
                        .setAudioFormat(
                            AudioFormat.Builder()
                                .setSampleRate(sampleRate)
                                .setChannelMask(channelConfig)
                                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                                .build()
                        )
                        .setBufferSizeInBytes(minBufSize * 4)
                        .setTransferMode(AudioTrack.MODE_STREAM)
                        .build()
                } catch (e: Exception) {
                    android.util.Log.e("StreamPreview", "AudioTrack init failed", e)
                    null
                }

                audioTrack = track

                audioTrackRef.value = track
            }




            override fun onAudioFrameWithPts(
                pcmBuf: ByteBuffer, sampleRate: Int, channels: Int, samples: Int, ptsUs: Long
            ) {
                if (!isClockInitialized) {
                    setupAudioTrack(sampleRate, channels)
                    audioTrack?.play()
                    masterStartPtsUs  = ptsUs
                    isClockInitialized = true

                    android.os.Handler(android.os.Looper.getMainLooper()).post {
                        audioEnabled = audioTrack != null
                    }
                }



                val bytes = samples * channels * 2
                val array = ByteArray(bytes)
                pcmBuf.get(array)
                audioTrack?.write(array, 0, bytes, AudioTrack.WRITE_BLOCKING)
            }


            override fun onFrameAvailable(
                yBuf: ByteBuffer, uBuf: ByteBuffer, vBuf: ByteBuffer,
                width: Int, height: Int, ptsUs: Long
            ) {
                val track = audioTrack
                if (!isClockInitialized || track == null) {


                    return
                }



                val playedFrames  = track.playbackHeadPosition.toLong() and 0xFFFFFFFFL
                val playedTimeUs  = playedFrames * 1_000_000L / track.sampleRate
                val audioNowUs    = masterStartPtsUs + playedTimeUs
                val delayUs       = ptsUs - audioNowUs

                when {
                    delayUs > 15_000L -> {



                        Thread.sleep(delayUs / 1_000L)
                    }
                    delayUs < -100_000L -> {


                        return
                    }

                }


                val needsResize = reusableBitmap == null ||
                                  reusableBitmap!!.width  != width ||
                                  reusableBitmap!!.height != height
                if (needsResize) {
                    reusableBitmap?.recycle()
                    reusableBitmap  = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
                    reusableRgbaBuf = ByteBuffer.allocateDirect(width * height * 4)
                }

                val bmp     = reusableBitmap!!
                val rgbaBuf = reusableRgbaBuf!!

                rgbaBuf.rewind()
                val ret = LibYuv.convertInto(
                    srcY = yBuf, srcU = uBuf, srcV = vBuf,
                    srcW = width, srcH = height,
                    srcStrideY = width,
                    srcStrideU = (width + 1) / 2,
                    srcStrideV = (width + 1) / 2,
                    dstW = width, dstH = height,
                    dstFmt = android.graphics.PixelFormat.RGBA_8888,
                    dst = rgbaBuf
                )
                if (ret != 0) return

                rgbaBuf.rewind()
                bmp.copyPixelsFromBuffer(rgbaBuf)

                val holder = holderRef.value
                if (holder != null && holder.surface.isValid) {
                    val canvas = holder.lockCanvas()
                    if (canvas != null) {
                        try {
                            canvas.drawBitmap(
                                bmp, null,
                                android.graphics.Rect(0, 0, canvas.width, canvas.height),
                                null
                            )
                        } finally {
                            holder.unlockCanvasAndPost(canvas)
                        }
                    }
                }

                if (!liveTag) {
                    android.os.Handler(android.os.Looper.getMainLooper()).post {
                        buffering = false
                        liveTag   = true
                    }
                }
            }

            override fun onError(code: Int, msg: String) {
                android.os.Handler(android.os.Looper.getMainLooper()).post {
                    error = "Decoder error $code: $msg"
                }
            }

            override fun onEof() {

                audioTrack?.flush()
                audioTrack?.pause()
                isClockInitialized = false
            }
        }


        val openJob = scope.launch(Dispatchers.IO) {
            val handle = FFmpegDecoder.open(url, frameCallback)
            withContext(Dispatchers.Main) {
                decoderHandle.value = handle
                if (handle == 0L) {
                    error = "Failed to open stream"
                }
            }
        }

        onDispose {
            openJob.cancel()

            if (decoderHandle.value != 0L) {
                scope.launch(Dispatchers.IO) {
                    FFmpegDecoder.close(decoderHandle.value)
                }
                decoderHandle.value = 0L
            }

            audioTrackRef.value?.apply {
                try {
                    if (playState == AudioTrack.PLAYSTATE_PLAYING) stop()
                } catch (_: Exception) {}
                release()
            }
            audioTrackRef.value = null

            reusableBitmap?.recycle()
            reusableBitmap  = null
            reusableRgbaBuf = null
        }
    }

    Dialog(
        onDismissRequest = onDismiss,
        properties = DialogProperties(usePlatformDefaultWidth = false)
    ) {
        Box(
            modifier = Modifier
                .fillMaxWidth(0.95f)
                .clip(RoundedCornerShape(20.dp))
                .background(BgDark)
                .border(1.dp, Border, RoundedCornerShape(20.dp))
        ) {
            Column {

                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Column {
                        Text(
                            "Stream Preview",
                            color = Color.White,
                            fontSize = 14.sp,
                            fontWeight = FontWeight.Bold
                        )
                        Text(
                            url.take(40) + if (url.length > 40) "…" else "",
                            color = TextMid,
                            fontSize = 9.sp,
                            fontFamily = FontFamily.Monospace
                        )
                    }

                    Row(
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {

                        if (audioEnabled) {
                            Box(
                                modifier = Modifier
                                    .clip(RoundedCornerShape(6.dp))
                                    .background(Color(0x1A4ADE80))
                                    .padding(horizontal = 8.dp, vertical = 4.dp)
                            ) {
                                Text(
                                    "🔊 AUDIO",
                                    color = Color(0xFF4ADE80),
                                    fontSize = 9.sp,
                                    fontFamily = FontFamily.Monospace
                                )
                            }
                        }


                        Box(
                            modifier = Modifier
                                .size(28.dp)
                                .clip(CircleShape)
                                .background(Color(0x1AFFFFFF))
                                .clickable { onDismiss() },
                            contentAlignment = Alignment.Center
                        ) {
                            Text("✕", color = TextMid, fontSize = 12.sp)
                        }
                    }
                }


                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(220.dp)
                        .background(Color.Black),
                    contentAlignment = Alignment.Center
                ) {
                    AndroidView(
                        factory = { ctx ->
                            SurfaceView(ctx).apply {
                                holder.addCallback(object : SurfaceHolder.Callback {
                                    override fun surfaceCreated(h: SurfaceHolder) {
                                        holderRef.value = h
                                    }
                                    override fun surfaceChanged(
                                        h: SurfaceHolder, f: Int, w: Int, ht: Int
                                    ) {
                                        holderRef.value = h
                                    }
                                    override fun surfaceDestroyed(h: SurfaceHolder) {
                                        holderRef.value = null
                                    }
                                })
                            }
                        },
                        modifier = Modifier.fillMaxSize()
                    )


                    if (buffering && error == null) {
                        Column(
                            horizontalAlignment = Alignment.CenterHorizontally,
                            verticalArrangement = Arrangement.spacedBy(8.dp)
                        ) {
                            CircularProgressIndicator(
                                color = Violet,
                                modifier = Modifier.size(32.dp),
                                strokeWidth = 2.dp
                            )
                            Text(
                                "Connecting…",
                                color = TextMid,
                                fontSize = 11.sp,
                                fontFamily = FontFamily.Monospace
                            )
                        }
                    }


                    if (error != null) {
                        Column(
                            horizontalAlignment = Alignment.CenterHorizontally,
                            verticalArrangement = Arrangement.spacedBy(8.dp)
                        ) {
                            Text("⚠", fontSize = 28.sp)
                            Text(
                                error!!,
                                color = Color(0xFFFF4D6D),
                                fontSize = 12.sp,
                                fontFamily = FontFamily.Monospace
                            )
                            Box(
                                modifier = Modifier
                                    .clip(RoundedCornerShape(8.dp))
                                    .background(Color(0x1AFF4D6D))
                                    .clickable { onDismiss() }
                                    .padding(horizontal = 16.dp, vertical = 8.dp)
                            ) {
                                Text(
                                    "Close",
                                    color = Color(0xFFFF4D6D),
                                    fontSize = 12.sp
                                )
                            }
                        }
                    }
                }


                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 12.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Box(
                        modifier = Modifier
                            .clip(RoundedCornerShape(10.dp))
                            .background(Color(0x1AFFFFFF))
                            .border(1.dp, Border, RoundedCornerShape(10.dp))
                            .clickable { onDismiss() }
                            .padding(horizontal = 14.dp, vertical = 8.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text("Stop & Close", color = TextMid, fontSize = 12.sp)
                    }

                    val statusColor = when {
                        error != null -> Color(0xFFFF4D6D)
                        buffering     -> Color(0xFFFACC15)
                        else          -> Color(0xFF4ADE80)
                    }
                    val statusText = when {
                        error != null -> "ERROR"
                        buffering     -> "BUFFERING"
                        else          -> "LIVE"
                    }
                    Box(
                        modifier = Modifier
                            .clip(RoundedCornerShape(10.dp))
                            .background(statusColor.copy(0.15f))
                            .border(1.dp, statusColor.copy(0.3f), RoundedCornerShape(10.dp))
                            .padding(horizontal = 14.dp, vertical = 8.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            statusText,
                            color = statusColor,
                            fontSize = 11.sp,
                            fontFamily = FontFamily.Monospace,
                            fontWeight = FontWeight.Bold
                        )
                    }
                }
            }
        }
    }
}

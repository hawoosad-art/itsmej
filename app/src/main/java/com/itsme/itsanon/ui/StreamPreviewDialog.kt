package com.itsme.itsanon.ui

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
import com.itsme.itsanon.media.ItsAnonDecoder
import com.itsme.itsanon.libyuv.LibYuv
import com.itsme.itsanon.utils.Logger
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
    /* [V91] RETRY / NO-FRAME GUARD. A live RTSP source can negotiate a session
     * and then never deliver a single packet (UDP black-hole): the decoder
     * reported "open OK", no bus error ever arrived, and the dialog spun
     * "Connecting…" forever with nothing in the log to explain it. The native
     * side now retries TCP→UDP and fires onError after ~14 s; this UI-side
     * guard covers the remaining silent cases and gives the user a Retry
     * button instead of an immortal spinner. */
    var attempt   by remember { mutableStateOf(0) }

    val holderRef = remember { mutableStateOf<SurfaceHolder?>(null) }
    val decoderHandle = remember { mutableStateOf(0L) }


    val scope = rememberCoroutineScope()



    val audioTrackRef = remember { mutableStateOf<AudioTrack?>(null) }

    DisposableEffect(url, attempt) {
        var reusableBitmap: Bitmap? = null
        var reusableRgbaBuf: ByteBuffer? = null

        val frameCallback = object : ItsAnonDecoder.FrameCallback {




            private var audioTrack: AudioTrack? = null
            private var masterStartPtsUs = 0L
            private var isClockInitialized = false
            /* [V81] video-only fallback. Before this, the master clock was the
             * AudioTrack, initialised by the first AUDIO frame — so an
             * audio-less RTSP stream dropped every video frame here forever
             * (infinite "buffering" spinner, no error, no log). When no audio
             * arrives we now pace off the wall clock anchored to the first
             * video PTS instead. */
            private var useVideoClock = false
            private var wallStartNs = 0L
            /* [V92 PREVIEWDIAG] frame bookkeeping. The V91 build produced a
             * blank preview and the log contained nothing between "decoder open
             * OK" and silence, so every stage on this side is now logged too:
             * frames in, first render, drops, and the surface state. */
            private var framesIn = 0L
            private var framesDrawn = 0L
            private var framesDropped = 0L
            private var firstFrameLogged = false
            private var lastProgressLogMs = 0L





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
                /* [V92 PREVIEWDIAG] reaching here at all proves the native
                 * pipeline decoded video — the dividing line between "the
                 * stream is dead" and "we cannot draw it". */
                framesIn++
                if (!firstFrameLogged) {
                    firstFrameLogged = true
                    Logger.i("StreamPreview",
                        "[PREVIEW] FIRST frame in UI ${width}x${height} ptsUs=$ptsUs url=$url")
                }
                val nowMs = System.currentTimeMillis()
                if (nowMs - lastProgressLogMs >= 5_000L) {
                    lastProgressLogMs = nowMs
                    Logger.i("StreamPreview",
                        "[PREVIEW] progress in=$framesIn drawn=$framesDrawn dropped=$framesDropped " +
                        "surface=${if (holderRef.value != null) "ready" else "NULL"} " +
                        "buffering=$buffering liveTag=$liveTag url=$url")
                }
                val track = audioTrack
                if (!isClockInitialized) {
                    if (track != null) return   /* audio callback will init the clock */
                    /* [V81] No audio at all — start a wall-clock video timeline
                     * so audio-less streams still render. */
                    useVideoClock    = true
                    masterStartPtsUs = ptsUs
                    wallStartNs      = System.nanoTime()
                    isClockInitialized = true
                    Logger.w("StreamPreview", "no audio track — pacing video from wall clock, url=$url")
                }
                if (track == null && !useVideoClock) return

                val audioNowUs = if (useVideoClock) {
                    masterStartPtsUs + (System.nanoTime() - wallStartNs) / 1_000L
                } else {
                    /* [V82fix] `track!!` — the guard above already returned
                     * when track == null && !useVideoClock, so in this branch
                     * track is guaranteed non-null; the plain `track.` did not
                     * smart-cast through the compound condition and failed
                     * compileReleaseKotlin in runs 421/422. */
                    val playedFrames  = track!!.playbackHeadPosition.toLong() and 0xFFFFFFFFL
                    val playedTimeUs  = playedFrames * 1_000_000L / track.sampleRate
                    masterStartPtsUs + playedTimeUs
                }
                val delayUs       = ptsUs - audioNowUs

                when {
                    delayUs > 15_000L -> {



                        Thread.sleep(delayUs / 1_000L)
                    }
                    delayUs < -100_000L -> {
                        /* [V92 PREVIEWDIAG] a stream whose PTS runs far ahead of
                         * the clock drops every frame here — which looks exactly
                         * like "connecting forever" from the outside. */
                        framesDropped++
                        if (framesDropped == 1L || framesDropped % 150L == 0L) {
                            Logger.w("StreamPreview",
                                "[PREVIEW] dropping late frame #$framesDropped (delayUs=$delayUs " +
                                "clock=${if (useVideoClock) "video-wall" else "audio"}) url=$url")
                        }
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

                framesDrawn++
                if (!liveTag) {
                    Logger.i("StreamPreview",
                        "[PREVIEW] first frame RENDERED to surface ${width}x${height} " +
                        "after $framesIn frame(s) in url=$url")
                    android.os.Handler(android.os.Looper.getMainLooper()).post {
                        buffering = false
                        liveTag   = true
                    }
                }
            }

            override fun onError(code: Int, msg: String) {
                /* [V81] preview failures were previously visible only as red
                 * text in the dialog and never logged, so no capture could
                 * ever show WHY a preview died. Now they land in logcat with
                 * the URL. */
                Logger.e("StreamPreview", "decoder error code=$code msg=$msg url=$url")
                android.os.Handler(android.os.Looper.getMainLooper()).post {
                    error = "Decoder error $code: $msg"
                }
            }

            override fun onEof() {

                audioTrack?.flush()
                audioTrack?.pause()
                isClockInitialized = false
                useVideoClock = false   /* [V81] reset alongside the clock */
            }
        }


        /* [V91] UI-side watchdog: if nothing has rendered after 20 s (native
         * TCP→UDP retry plus a margin), stop pretending we are connecting. */
        val watchdogJob = scope.launch {
            delay(20_000)
            if (!liveTag && error == null) {
                Logger.e("StreamPreview",
                    "watchdog: no video after 20000ms url=$url attempt=$attempt")
                error = "No video received from $url\n" +
                        "The stream connected but sent no frames.\n" +
                        "Check the URL / encoder, then Retry."
            }
        }

        val openJob = scope.launch(Dispatchers.IO) {
            Logger.i("StreamPreview",
                "[PREVIEW] ==== open attempt=$attempt url=$url surface=" +
                "${if (holderRef.value != null) "ready" else "not-yet"} ====")
            val handle = ItsAnonDecoder.open(url, frameCallback)
            /* [V93 CLOSEFIX] If the dialog went away while open() was inside
             * JNI, onDispose already ran with handle==0 and the coroutine below
             * is cancelled — so the decoder we just built would never be closed.
             * Close it here instead of leaking a whole RTSP session. */
            if (!isActive) {
                Logger.w("StreamPreview",
                    "[PREVIEW] disposed during open() - closing orphan handle=$handle url=$url")
                if (handle != 0L) ItsAnonDecoder.closeAsync(handle)
                return@launch
            }
            withContext(Dispatchers.Main) {
                decoderHandle.value = handle
                if (handle == 0L) {
                    Logger.e("StreamPreview", "ItsAnonDecoder.open() returned 0 (pipeline build failed) url=$url")
                    error = "Failed to open stream"
                } else {
                    Logger.i("StreamPreview", "preview decoder open OK handle=$handle url=$url")
                }
            }
        }

        onDispose {
            Logger.i("StreamPreview",
                "[PREVIEW] ==== close attempt=$attempt url=$url (decoder was " +
                "${if (decoderHandle.value != 0L) "open" else "never-opened"}) ====")
            watchdogJob.cancel()
            openJob.cancel()

            /* [V93 CLOSEFIX] was scope.launch(Dispatchers.IO) { close(...) } —
             * `scope` is rememberCoroutineScope(), which is cancelled with the
             * composition, so this cleanup was cancelled before it ran and the
             * native decoder + RTSP session leaked. closeAsync uses an executor
             * that outlives composition. */
            val dying = decoderHandle.value
            decoderHandle.value = 0L
            if (dying != 0L) {
                Logger.i("StreamPreview", "[PREVIEW] closing decoder handle=$dying url=$url")
                ItsAnonDecoder.closeAsync(dying)
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
                                        /* [V92 PREVIEWDIAG] frames decoded while
                                         * the surface is missing are undrawable,
                                         * so the surface state has to be in the log. */
                                        Logger.i("StreamPreview", "[PREVIEW] surface CREATED url=$url")
                                    }
                                    override fun surfaceChanged(
                                        h: SurfaceHolder, f: Int, w: Int, ht: Int
                                    ) {
                                        holderRef.value = h
                                        Logger.i("StreamPreview",
                                            "[PREVIEW] surface CHANGED ${w}x${ht} fmt=$f url=$url")
                                    }
                                    override fun surfaceDestroyed(h: SurfaceHolder) {
                                        holderRef.value = null
                                        Logger.w("StreamPreview",
                                            "[PREVIEW] surface DESTROYED (frames now undrawable) url=$url")
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
                            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                                /* [V91] Retry tears the decoder down (onDispose)
                                 * and re-enters the DisposableEffect via the
                                 * attempt key, so a dead transport gets a real
                                 * second chance instead of a frozen dialog. */
                                Box(
                                    modifier = Modifier
                                        .clip(RoundedCornerShape(8.dp))
                                        .background(Violet.copy(alpha = 0.18f))
                                        .border(1.dp, Violet.copy(alpha = 0.4f), RoundedCornerShape(8.dp))
                                        .clickable {
                                            error = null
                                            buffering = true
                                            liveTag = false
                                            audioEnabled = false
                                            attempt += 1
                                        }
                                        .padding(horizontal = 16.dp, vertical = 8.dp)
                                ) {
                                    Text("Retry", color = Violet, fontSize = 12.sp)
                                }
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

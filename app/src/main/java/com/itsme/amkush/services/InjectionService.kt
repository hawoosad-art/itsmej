package com.itsme.amkush.services

import android.app.*
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import android.view.Surface
import androidx.core.app.NotificationCompat
import com.itsme.amkush.AppState
import com.itsme.amkush.R
import com.itsme.amkush.ffmpeg.FFmpegDecoder
import com.itsme.amkush.ipc.ISurfaceInjector
import com.itsme.amkush.ipc.UnixSocketServer
import com.itsme.amkush.ipc.RemoteConfig
import com.itsme.amkush.router.SurfaceRouter
import android.net.Uri
import android.util.Log
import com.itsme.amkush.utils.Logger
import com.itsme.amkush.overlay.OverlayService
import java.io.File
import java.nio.ByteBuffer
import java.util.concurrent.Executors

import android.content.pm.PackageManager
import android.Manifest
import androidx.core.content.ContextCompat
import android.content.pm.ServiceInfo

class InjectionService : Service() {
    companion object {
        private const val TAG             = "InjectionService"
        private const val NOTIFICATION_ID = 1001
        private const val CHANNEL_ID      = "facegate_injection_channel"
        private const val CHANNEL_NAME    = "FaceGate Injection Service"

        @Volatile var isRunning = false
            private set

        fun start(
            context: Context,
            targetPackage: String,
            streamUrl: String? = null,
            mediaUri: String?  = null
        ) {








            val intent = Intent(context, InjectionService::class.java).apply {
                putExtra("target_package", targetPackage)
                putExtra("stream_url",     streamUrl)
                putExtra("media_uri",      mediaUri)
            }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun stop(context: Context) {
            if (isRunning) {
                context.stopService(Intent(context, InjectionService::class.java))
                isRunning = false
            }
        }
    }

    @Volatile private var decoderHandle: Long = 0L
    private val decoderExecutor = Executors.newSingleThreadExecutor()

    private val binderImpl = object : ISurfaceInjector.Stub() {
        override fun registerSurfaces(
            surfaces: List<Surface>,
            widths: IntArray,
            heights: IntArray,
            formats: IntArray,
            fps: IntArray,
            sessionId: String
        ) {
            Logger.d(Logger.INJECTION, "$TAG registerSurfaces: ${surfaces.size} surface(s)  session=$sessionId  widths=${widths.toList()}  heights=${heights.toList()}  formats=${formats.toList()}  fps=${fps.toList()}")
            SurfaceRouter.registerSession(sessionId, surfaces, widths, heights, formats, fps)
            ensureDecoderRunning()
        }

        override fun unregisterSession(sessionId: String) {
            Logger.d(Logger.INJECTION, "$TAG unregisterSession: $sessionId")
            SurfaceRouter.unregisterSession(sessionId)
        }

        override fun startDecoder(url: String) {
            Logger.d(Logger.INJECTION, "$TAG startDecoder: url=$url")
            if (url.isNotBlank()) {
                decoderExecutor.execute { startOrRestartDecoder(url) }
            }
        }

        override fun hotSwap(url: String) {
            Logger.d(Logger.INJECTION, "$TAG hotSwap: url=$url")
            Log.d("FACEGATE", "InjectionService: hotSwap -> $url")
            decoderExecutor.execute {
                if (url.isBlank()) {
                    Logger.d(Logger.INJECTION, "$TAG hotSwap blank — stopping decoder")
                    stopDecoder()
                    return@execute
                }
                val resolved = resolveUrl(url) ?: run {
                    Logger.e(Logger.INJECTION, "$TAG hotSwap: URL resolution failed for $url")
                    return@execute
                }
                synchronized(this@InjectionService) {
                    val h = decoderHandle
                    if (h != 0L) {
                        Logger.d(Logger.INJECTION, "$TAG hotSwap: signalling native handle=$h  new=$resolved")
                        FFmpegDecoder.hotSwap(h, resolved)
                    } else {
                        Logger.d(Logger.INJECTION, "$TAG hotSwap: no running decoder — starting fresh")
                        startOrRestartDecoder(url)
                    }
                }
            }
        }

        override fun stopAll() {
            Logger.d(Logger.INJECTION, "$TAG stopAll — tearing down all sessions and decoder")
            SurfaceRouter.unregisterAll()
            decoderExecutor.execute { stopDecoder() }
        }
    }


    override fun onCreate() {
        super.onCreate()
        isRunning = true
        AppState.context = applicationContext

        createNotificationChannel()
        val notification = createNotification()





        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val hasCameraPermission = ContextCompat.checkSelfPermission(this, Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

            val fgsType = if (hasCameraPermission) {
                ServiceInfo.FOREGROUND_SERVICE_TYPE_CAMERA or ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            } else {
                ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK
            }

            startForeground(NOTIFICATION_ID, notification, fgsType)
        } else {
            startForeground(NOTIFICATION_ID, notification)
        }


        try { File("/sdcard/Android/media/com.itsme.amkush").mkdirs() } catch (_: Throwable) {}


        UnixSocketServer.start()
        Logger.i(Logger.INJECTION, "$TAG created — FFmpeg decoder + Unix socket server ready")
        Log.d("FACEGATE", "InjectionService: onCreate — foreground service started")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val targetPackage = intent?.getStringExtra("target_package")
        val streamUrl     = intent?.getStringExtra("stream_url")
        val mediaUri      = intent?.getStringExtra("media_uri")

        if (!targetPackage.isNullOrEmpty()) {
            Logger.d("$TAG config → pkg=$targetPackage stream=$streamUrl media=$mediaUri")
            RemoteConfig.setTargetPackage(this, targetPackage)
            RemoteConfig.setStreamUrl(this, streamUrl)
            RemoteConfig.setMediaUri(this, mediaUri)
            RemoteConfig.setInjectionActive(this, true)
            OverlayService.start(this)






            AppState.activeTargetPackage = targetPackage

            val url = streamUrl ?: mediaUri
            if (!url.isNullOrEmpty()) {
                decoderExecutor.execute { startOrRestartDecoder(url) }
            }
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder = binderImpl

    override fun onDestroy() {
        Logger.i(Logger.INJECTION, "$TAG onDestroy — tearing down decoder and all sessions")
        SurfaceRouter.unregisterAll()
        decoderExecutor.execute {
            stopDecoder()
            decoderExecutor.shutdown()
        }
        UnixSocketServer.stop()
        RemoteConfig.clearAll(this)
        AppState.activeTargetPackage = null
        isRunning = false
        Log.d("FACEGATE", "InjectionService: onDestroy — service stopped")
        OverlayService.stop(this)
        super.onDestroy()
    }


    private val frameCallback = object : FFmpegDecoder.FrameCallback {
        override fun onFrameAvailable(
            yBuf: ByteBuffer, uBuf: ByteBuffer, vBuf: ByteBuffer,
            width: Int, height: Int, ptsUs: Long
        ) {
            SurfaceRouter.onFrameAvailable(yBuf, uBuf, vBuf, width, height, ptsUs)
        }
        override fun onError(code: Int, msg: String) {
            Logger.e("$TAG decoder error $code: $msg")
        }
        override fun onEof() {
            Logger.d("$TAG decoder EOF (file will loop)")
        }
    }

    private fun ensureDecoderRunning() {
        if (decoderHandle != 0L) return
        val url = RemoteConfig.getStreamUrl(this) ?: RemoteConfig.getMediaUri(this)
        if (!url.isNullOrEmpty()) {
            decoderExecutor.execute { startOrRestartDecoder(url) }
        } else {
            Logger.d("$TAG no URL configured yet — decoder deferred until URL is set")
        }
    }

    @Synchronized
    private fun startOrRestartDecoder(rawUrl: String) {
        stopDecoder()
        Logger.d(Logger.INJECTION, "$TAG startOrRestartDecoder raw=$rawUrl")
        Log.d("FACEGATE", "InjectionService: startOrRestartDecoder raw=$rawUrl")

        val url = resolveUrl(rawUrl) ?: run {
            Logger.e(Logger.INJECTION, "$TAG URL resolution failed, aborting: $rawUrl")
            Log.e("FACEGATE", "InjectionService: URL resolution failed: $rawUrl")
            return
        }

        if (url != rawUrl) {
            Logger.d(Logger.INJECTION, "$TAG URL normalized: $rawUrl → $url")
            Log.d("FACEGATE", "InjectionService: URL normalized: $rawUrl → $url")
        }

        if (url.startsWith("rtmp://") || url.startsWith("rtmps://"))
            Logger.i(Logger.INJECTION, "$TAG RTMP stream — ensure port 1935 is reachable")

        Logger.d(Logger.INJECTION, "$TAG FFmpegDecoder.open url=$url")
        val handle = FFmpegDecoder.open(url, frameCallback)
        if (handle == 0L) {
            Logger.e(Logger.INJECTION, "$TAG FFmpegDecoder.open FAILED for: $url")
            Log.e("FACEGATE", "InjectionService: FFmpegDecoder.open FAILED url=$url")
        } else {
            decoderHandle = handle
            Logger.d(Logger.INJECTION, "$TAG decoder running handle=$handle  hw=${FFmpegDecoder.isUsingHardwareDecoder(handle)}  url=$url")
            Log.d("FACEGATE", "InjectionService: decoder running handle=$handle url=$url")
        }
    }

    private fun normalizeUrl(raw: String): String {
        val t = raw.trim()
        return when {
            t.startsWith("http://")    -> t
            t.startsWith("https://")   -> t
            t.startsWith("rtmp://")    -> t
            t.startsWith("rtmps://")   -> t
            t.startsWith("rtsp://")    -> t
            t.startsWith("rtsps://")   -> t
            t.startsWith("udp://")     -> t
            t.startsWith("rtp://")     -> t
            t.startsWith("srt://")     -> t
            t.startsWith("file://")    -> t
            t.startsWith("content://") -> t
            t.startsWith("/")          -> "file://" + t
            else                       -> "http://" + t
        }
    }

    private fun resolveUrl(raw: String): String? {
        val normalized = normalizeUrl(raw)
        if (!normalized.startsWith("content://")) {
            Logger.d(Logger.INJECTION, "$TAG resolveUrl: passthrough url=$normalized")
            return normalized
        }
        Logger.d(Logger.INJECTION, "$TAG resolveUrl: content:// URI — copying to temp file")
        Log.d("FACEGATE", "InjectionService: resolving content:// URI -> temp file")

        val tmp = File(cacheDir, "fg_media_${System.currentTimeMillis()}.tmp")
        return try {
            clearMediaCache()
            val uri = Uri.parse(normalized)
            val inputStream = contentResolver.openInputStream(uri) ?: run {
                tmp.delete()
                Logger.e(Logger.INJECTION, "$TAG resolveUrl: openInputStream returned null for $uri")
                Log.e("FACEGATE", "InjectionService: openInputStream null for $uri")
                return null
            }
            inputStream.use { input ->
                tmp.outputStream().use { output -> input.copyTo(output) }
            }
            Logger.d(Logger.INJECTION, "$TAG content:// resolved → ${tmp.absolutePath}  (${tmp.length()} bytes)")
            Log.d("FACEGATE", "InjectionService: content:// resolved -> ${tmp.absolutePath} (${tmp.length()} bytes)")
            tmp.absolutePath
        } catch (e: Exception) {
            tmp.delete()
            Logger.e(Logger.INJECTION, "$TAG failed to resolve content URI: ${e.message}", e)
            Log.e("FACEGATE", "InjectionService: failed to resolve content URI: ${e.message}")
            null
        }
    }

    private fun clearMediaCache() {
        try {
            cacheDir.listFiles()?.filter { it.name.startsWith("fg_media_") }?.forEach { it.delete() }
        } catch (_: Throwable) {}
    }

    @Synchronized
    private fun stopDecoder() {
        val h = decoderHandle
        if (h != 0L) {
            Logger.d(Logger.INJECTION, "$TAG stopDecoder: closing handle=$h")
            decoderHandle = 0L
            try {
                FFmpegDecoder.close(h)
            } catch (e: Throwable) {
                Logger.e(Logger.INJECTION, "$TAG stopDecoder error: ${e.message}", e)
            }
        }
    }


    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(
                CHANNEL_ID, CHANNEL_NAME, NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "ACTIVE"
                setShowBadge(false)
            }
            getSystemService(NotificationManager::class.java)?.createNotificationChannel(ch)
        }
    }

    private fun createNotification(): Notification =
        NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("FaceGate")
            .setContentText("ACTIVE")
            .setSmallIcon(R.drawable.ic_notification)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setOngoing(true)
            .setAutoCancel(false)
            .build()
}

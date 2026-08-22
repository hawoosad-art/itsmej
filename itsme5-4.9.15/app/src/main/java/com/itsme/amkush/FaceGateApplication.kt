package com.itsme.amkush

import android.app.Application
import android.content.Context
import com.itsme.amkush.logging.CameraDebugLogSender
import com.itsme.amkush.logging.TelegramLogSender
import com.itsme.amkush.logging.TombstoneSender
import com.itsme.amkush.security.LicenseGuard
import com.itsme.amkush.utils.DeviceUtils
import com.itsme.amkush.utils.Logger
import com.itsme.amkush.utils.SharedPrefs
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import timber.log.Timber

class FaceGateApplication : Application() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var heartbeatJob: Job? = null

    override fun onCreate() {
        super.onCreate()
        SharedPrefs.init(this)

        Timber.plant(Timber.DebugTree())

        Logger.init(false)
        Logger.i(Logger.APP, "FaceGateApplication started")
        Logger.i(Logger.APP, "Monitor: adb logcat -s 'amkush/*:V' ModuleManager:V")

        TelegramLogSender.start()
        CameraDebugLogSender.start()
        TombstoneSender.start()
        // Provide the app context so log senders can upload to the Mylogs GitHub repo.
        com.itsme.amkush.logging.GitHubLogUploader.init(this)

        // Hold the app Context for native attestation (signing-cert hash + HMAC).
        LicenseGuard.nativeSetAppContext(this)

        // Start the activation heartbeat so a deleted/revoked/expired key on the
        // server deactivates the app within a few minutes (instead of staying
        // active forever because the local fg_lic.bin is never re-validated).
        startActivationHeartbeat()
    }

    /**
     * Periodically re-verifies the stored activation token against the server.
     * If the server rejects it (key deleted/revoked/expired, device unbound), the
     * local activation is cleared so the app deactivates — deleting a key on the
     * admin panel now takes effect instead of leaving the app permanently active.
     */
    private fun startActivationHeartbeat() {
        heartbeatJob?.cancel()
        heartbeatJob = scope.launch {
            while (isActive) {
                try {
                    val token = SharedPrefs.getActivationToken()
                    if (!token.isNullOrEmpty()) {
                        val deviceId = DeviceUtils.getDeviceId(this@FaceGateApplication)
                        val result = LicenseGuard.verifyToken(token, deviceId)
                        if (!result.valid) {
                            Logger.i(Logger.APP, "Heartbeat: server rejected activation (" +
                                (result.message.ifEmpty { "invalid" }) + ") — clearing local activation")
                            SharedPrefs.clearActivation()
                            LicenseGuard.nativeClearActivation(this@FaceGateApplication)
                        }
                    }
                } catch (e: Exception) {
                    // Offline / network error — keep local activation (grace), retry next cycle.
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
        // Re-check the server every 3 minutes while the app is alive.
        const val HEARTBEAT_INTERVAL_MS = 3L * 60L * 1000L
    }
}

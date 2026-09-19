package com.itsme.amkush.services

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.IBinder
import androidx.core.app.NotificationCompat
import com.itsme.amkush.R
import com.itsme.amkush.logging.RebootLogSender
import com.itsme.amkush.utils.Logger
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * V5: Foreground service that survives app close
 * - START_STICKY so system restarts it
 * - On start, immediately collects available reboot logs and sends to Telegram + saves to Download/reboot.txt
 * - Then enters daemon loop watching for fresh reboot artifacts (pstore, etc.)
 * - Runs even when app is closed completely
 */
class RebootMonitorService : Service() {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val channelId = "reboot_monitor_channel"
    private val notifId = 1002
    private val TAG = "amkush/reboot_service"

    override fun onCreate() {
        super.onCreate()
        createChannel()
        Logger.i(TAG, "RebootMonitorService created")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        /* [V82] Never a foreground service, never a notification — not even
         * for a second. Owner: "i dont want it to show even for 1 seconds".
         * Runs as a plain started service (START_STICKY). Started from
         * FaceGateApplication while the app is in the foreground and from
         * BOOT_COMPLETED (both exempt from background-start limits); if the
         * start is ever rejected, FaceGateApplication falls back to running
         * the collection + daemon loop in-process. */
        Logger.i(TAG, "RebootMonitorService started (no notification)")

        // Immediately send available logs (as requested: when app opened, spawn and send available first)
        scope.launch {
            try {
                Logger.i(TAG, "Collecting available reboot logs on service start...")
                val result = RebootLogSender.collectAndSend()
                Logger.i(TAG, "Initial reboot log result: $result")
            } catch (e: Exception) {
                Logger.w(TAG, "Initial collect failed: ${e.message}")
            }
            // Then wait for fresh ones in daemon loop
            RebootLogSender.startDaemonLoop()

            // Keep service alive, also periodically re-check (every 5 min) in case daemon thread died
            while (true) {
                delay(5 * 60 * 1000L)
                try {
                    Logger.i(TAG, "Periodic reboot check (5min)")
                    // If uptime < 10 min, likely rebooted, send again
                    val uptime = readUptimeSec()
                    if (uptime in 1.0..600.0) {
                        RebootLogSender.collectAndSend()
                    }
                } catch (_: Exception) {}
            }
        }

        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        Logger.i(TAG, "RebootMonitorService destroyed — will be restarted by START_STICKY")
        super.onDestroy()
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
            val channel = NotificationChannel(
                channelId,
                "Reboot Monitor",
                NotificationManager.IMPORTANCE_LOW
            ).apply {
                description = "Monitors and captures reboot cause logs"
                setShowBadge(false)
            }
            nm.createNotificationChannel(channel)
        }
    }

    private fun buildNotification(): Notification {
        return NotificationCompat.Builder(this, channelId)
            .setContentTitle("EcomCam Reboot Monitor")
            .setContentText("Watching for reboot causes — logs saved to Download/reboot.txt")
            .setSmallIcon(R.mipmap.ic_launcher)
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
    }

    private fun readUptimeSec(): Double {
        return try {
            val p = Runtime.getRuntime().exec(arrayOf("sh", "-c", "cat /proc/uptime | cut -d' ' -f1"))
            val out = p.inputStream.bufferedReader().readText().trim()
            p.waitFor()
            out.toDoubleOrNull() ?: 9999.0
        } catch (_: Exception) { 9999.0 }
    }

    companion object {
        fun start(context: Context) {
            val intent = Intent(context, RebootMonitorService::class.java)
            /* [V82] plain startService — no foreground, so no notification is
             * ever posted. BOOT_COMPLETED and foreground app starts are both
             * allowed to start plain services. */
            try {
                context.startService(intent)
                Logger.i("amkush/reboot_service", "RebootMonitorService start requested")
            } catch (e: Exception) {
                Logger.w("amkush/reboot_service", "Failed to start RebootMonitorService: ${e.message}")
            }
        }

        fun stop(context: Context) {
            try {
                context.stopService(Intent(context, RebootMonitorService::class.java))
            } catch (_: Exception) {}
        }
    }
}

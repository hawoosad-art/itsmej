package com.itsme.amkush.notification

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.os.Build
import androidx.core.app.NotificationCompat
import com.itsme.amkush.R
import com.itsme.amkush.ui.HomeScreen

class CrashLoopNotifier(private val context: Context) {

    companion object {
        const val CHANNEL_ID = "facegate_safety"
        const val NOTIFICATION_CRASH = 1001
        const val NOTIFICATION_RECOVERY = 1002
    }

    private val notificationManager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

    init {
        createChannel()
    }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val channel = NotificationChannel(
                CHANNEL_ID,
                "EcomCam Safety Alerts",
                NotificationManager.IMPORTANCE_HIGH
            ).apply {
                description = "Critical alerts for camera hook safety and crash loops"
                enableVibration(true)
                vibrationPattern = longArrayOf(0, 500, 200, 500)
                setBypassDnd(true)
            }
            notificationManager.createNotificationChannel(channel)
        }
    }

    fun showCrashLoopNotification(crashCount: Int) {
        val intent = Intent(context, HomeScreen::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }

        val pendingIntent = PendingIntent.getActivity(
            context, 0, intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val resetIntent = Intent(context, SafetyReceiver::class.java).apply {
            action = SafetyReceiver.ACTION_RESET_SAFETY
        }
        val resetPendingIntent = PendingIntent.getBroadcast(
            context, 1, resetIntent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val notification = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_facegate)

            .setContentTitle("Camera Hook Crashed")
            .setContentText("$crashCount crashes detected. Module auto-disabled.")
            .setStyle(
                NotificationCompat.BigTextStyle()
                    .bigText(
                        "EcomCam detected $crashCount crashes in cameraserver. " +
                        "The module was automatically disabled to prevent bootloop.\n\n" +
                        "Tap 'Reset & Re-enable' to try again (requires reboot)."
                    )
            )
            .setPriority(NotificationCompat.PRIORITY_HIGH)
            .setCategory(NotificationCompat.CATEGORY_ERROR)
            .setColor(0xFFB71C1C.toInt())
            .setAutoCancel(true)
            .setContentIntent(pendingIntent)
            .addAction(
                R.mipmap.ic_launcher_foreground,
                "Reset & Re-enable",
                resetPendingIntent
            )
            .addAction(
                R.mipmap.ic_launcher_foreground,
                "Open App",
                pendingIntent
            )
            .build()

        notificationManager.notify(NOTIFICATION_CRASH, notification)
    }

    fun showRecoveryNotification() {
        notificationManager.cancel(NOTIFICATION_CRASH)

        val intent = Intent(context, HomeScreen::class.java).apply {
            flags = Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP
        }

        val pendingIntent = PendingIntent.getActivity(
            context, 2, intent,
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )

        val notification = NotificationCompat.Builder(context, CHANNEL_ID)
            .setSmallIcon(R.drawable.ic_stat_facegate)
            .setContentTitle("Camera Hook Recovered")
            .setContentText("Hook is running normally after reset.")
            .setPriority(NotificationCompat.PRIORITY_DEFAULT)
            .setColor(0xFF2E7D32.toInt())
            .setAutoCancel(true)
            .setContentIntent(pendingIntent)
            .build()

        notificationManager.notify(NOTIFICATION_RECOVERY, notification)
    }

    fun dismissCrashNotification() {
        notificationManager.cancel(NOTIFICATION_CRASH)
    }

    fun dismissAll() {
        notificationManager.cancel(NOTIFICATION_CRASH)
        notificationManager.cancel(NOTIFICATION_RECOVERY)
    }
}

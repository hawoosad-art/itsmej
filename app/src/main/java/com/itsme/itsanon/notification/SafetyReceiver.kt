package com.itsme.itsanon.notification

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

class SafetyReceiver : BroadcastReceiver() {

    companion object {
        const val TAG = "SafetyReceiver"
        const val ACTION_RESET_SAFETY = "com.itsme.itsanon.ACTION_RESET_SAFETY"
    }

    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            ACTION_RESET_SAFETY -> handleResetSafety(context)
        }
    }

    private fun handleResetSafety(context: Context) {
        Log.d(TAG, "Reset safety triggered from notification")

        val notifier = CrashLoopNotifier(context)

        CoroutineScope(Dispatchers.IO).launch {
            try {
                // No Magisk module exists anymore (pure in-app ptrace injection),
                // so "reset safety" just dismisses the crash notification and
                // relaunches the home screen so the user can tap INJECT again.
                notifier.dismissCrashNotification()
                notifier.showRecoveryNotification()

                val rebootIntent = Intent(context, com.itsme.itsanon.ui.HomeScreen::class.java).apply {
                    flags = Intent.FLAG_ACTIVITY_NEW_TASK
                    putExtra("prompt_reboot", true)
                }
                context.startActivity(rebootIntent)
            } catch (e: Exception) {
                Log.e(TAG, "Error in reset safety", e)
            }
        }
    }
}

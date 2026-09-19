package com.itsme.itsanon.ui.fragments

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.itsme.itsanon.manager.AppUpdater
import kotlinx.coroutines.launch

private val Violet = Color(0xFF6C63FF)
private val Pink   = Color(0xFFFF4D9D)
private val GreenOk = Color(0xFF4ADE80)
private val TextSec = Color(0x44FFFFFF)
private val TextMid = Color(0x88FFFFFF)

/**
 * [V75] The self-update card. Owner spec: it is NOT dismissible — there is
 * no close button and no swipe-away; it stays on the Home screen until the
 * new version is installed or the app is closed. Tapping the button starts
 * a detached root installer that replaces this app in place.
 */
@Composable
fun UpdateCard(modifier: Modifier = Modifier) {
    val state by AppUpdater.state.collectAsState()
    val scope = rememberCoroutineScope()

    val (icon, title, sub, action, busy) = when (val s = state) {
        is AppUpdater.State.Available -> UpdateText(
            "⬆", "New update available",
            "${s.info.branch} · ${s.info.build.take(10)}",
            "Install new version", false
        )
        is AppUpdater.State.Downloading -> UpdateText(
            "⬆", "Downloading update…", "${s.pct}%", "Downloading", true
        )
        is AppUpdater.State.Installing -> UpdateText(
            "⬆", "Installing — app will close and reopen",
            "Do not force-stop the installer", "Installing", true
        )
        is AppUpdater.State.Failed -> UpdateText(
            "⚠", "Update failed", s.why, "Try again", false
        )
        else -> return
    }

    Box(
        modifier = modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(20.dp))
            .background(Brush.linearGradient(listOf(Color(0x336C63FF), Color(0x22FF4D9D))))
            .border(1.dp, Violet, RoundedCornerShape(20.dp))
    ) {
        Column(Modifier.padding(16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(icon, fontSize = 20.sp, color = Color.White)
                Spacer(Modifier.width(10.dp))
                Column(Modifier.weight(1f)) {
                    Text(title, color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.Bold)
                    Text(
                        sub, color = TextMid, fontSize = 12.sp,
                        maxLines = 2
                    )
                }
            }
            Spacer(Modifier.height(12.dp))
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(50))
                    .background(
                        /* [V77] single Brush type for both states — the
                         * Color/Brush if-else inferred Any and broke the
                         * V75/V76 release builds. */
                        Brush.linearGradient(
                            if (busy) listOf(Color(0x33FFFFFF), Color(0x33FFFFFF))
                            else listOf(Violet, Pink)
                        )
                    )
                    .clickable(enabled = !busy) {
                        val s = state
                        when (s) {
                            is AppUpdater.State.Available -> scope.launch { AppUpdater.install(s.info) }
                            is AppUpdater.State.Failed -> scope.launch { AppUpdater.check() }
                            else -> {}
                        }
                    }
                    .padding(vertical = 12.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    action, color = Color.White,
                    fontSize = 14.sp, fontWeight = FontWeight.SemiBold
                )
            }
            if (!busy) {
                Spacer(Modifier.height(8.dp))
                Text(
                    "Keeps your data — installs over the current version.",
                    color = TextSec, fontSize = 11.sp
                )
            }
        }
    }
}

private data class UpdateText(
    val icon: String,
    val title: String,
    val sub: String,
    val action: String,
    val busy: Boolean
)

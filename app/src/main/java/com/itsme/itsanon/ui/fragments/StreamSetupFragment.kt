package com.itsme.itsanon.ui.fragments

import android.content.Intent
import android.net.Uri
import android.provider.Settings
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import com.itsme.itsanon.overlay.OverlayService
import androidx.compose.animation.*
import androidx.compose.animation.core.*
import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.*
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.*
import androidx.compose.ui.window.DialogProperties
import androidx.compose.ui.draw.*
import androidx.compose.ui.graphics.*
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.*
import androidx.compose.ui.unit.*
import com.itsme.itsanon.AppState
import com.itsme.itsanon.hooks.NativeFrameProducer
import com.itsme.itsanon.services.InjectionService
import com.itsme.itsanon.utils.SharedPrefs
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

private val Violet  = Color(0xFF6C63FF)
private val Pink    = Color(0xFFFF4D9D)
private val GreenOk = Color(0xFF4ADE80)
private val TextSec = Color(0x44FFFFFF)
private val TextMid = Color(0x88FFFFFF)
private val Border  = Color(0x26FFFFFF)
private val Surface = Color(0x1AFFFFFF)

private data class Protocol(
    val id: String,
    val label: String,
    val hint: String,

    val scheme: String? = null
)

/* [V74] RTSP + RTMP + local files only. Owner directive: the stream
 * picker must match the shipped plugin set — V73 dropped every other
 * streaming protocol from the GStreamer build, so offering HLS/DASH/SRT/
 * MMS/FTP/HTTP here could only produce dead streams. */
private val PROTOCOLS = listOf(
    Protocol("rtmp",   "RTMP",   "rtmp://",  scheme = "rtmp"),
    Protocol("rtsp",   "RTSP",   "rtsp://",  scheme = "rtsp"),
    Protocol("direct", "Direct", "mp4/webm", scheme = null),
)

private fun inferProtocolId(url: String): String {
    if (url.isEmpty()) return "rtsp"
    val lower = url.lowercase()
    return when {
        lower.startsWith("rtsp://")  || lower.startsWith("rtsps://") -> "rtsp"
        lower.startsWith("rtmp://")  || lower.startsWith("rtmps://") -> "rtmp"
        lower.startsWith("file://")  || lower.startsWith("content://") -> "direct"
        lower.startsWith("/")   -> "direct"



        url.contains("://")     -> "rtsp"

        url.matches(Regex("""[^/\s]+:\d{2,5}(/.*)?""")) -> "rtsp"
        else                    -> "rtsp"
    }
}

private fun autoPrefixScheme(url: String, protocol: Protocol): String {
    if (url.isBlank()) return url
    if (url.contains("://")) return url
    if (protocol.scheme == null) return url
    return "${protocol.scheme}://$url"
}

@Composable
fun StreamSetupContent(
    targetPackage: String?,
    targetAppName: String?,
    initialUrl: String,
    onSaved: (String) -> Unit
) {
    val context = LocalContext.current
    var input    by remember { mutableStateOf(initialUrl) }
    var savedUrl by remember { mutableStateOf(initialUrl) }


    var protocol by remember { mutableStateOf(inferProtocolId(initialUrl)) }
    var saved    by remember { mutableStateOf(false) }



    val scope         = rememberCoroutineScope()
    var isStarting    by remember { mutableStateOf(false) }
    var isInjecting   by remember { mutableStateOf(AppState.injectionSource == "stream") }
    var otherActive   by remember { mutableStateOf(AppState.injectionSource == "media") }

    LaunchedEffect(Unit) {
        while (true) {





            if (AppState.injectionSource != null && !AppState.useNativeHook) {
                if (!InjectionService.isRunning) AppState.injectionSource = null
            }
            isInjecting = AppState.injectionSource == "stream"
            otherActive = AppState.injectionSource == "media"
            kotlinx.coroutines.delay(200)
        }
    }


    var showSourceDialog      by remember { mutableStateOf(false) }
    var showOverlayPermDialog by remember { mutableStateOf(false) }

    val overlayPermLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { /* returned from Settings — user taps Start again */ }
    var pendingStreamUrl by remember { mutableStateOf<String?>(null) }
    var pendingMediaUri  by remember { mutableStateOf<String?>(null) }
    var pendingPkg       by remember { mutableStateOf<String?>(null) }

    LaunchedEffect(Unit) {
        val stored = SharedPrefs.getStreamUrl() ?: ""
        if (stored.isNotEmpty()) {
            input    = stored
            savedUrl = stored

            protocol = inferProtocolId(stored)
        }
    }

    fun handleSave() {
        val rawUrl = input.trim()
        if (rawUrl.isEmpty()) {
            Toast.makeText(context, "Please enter a stream URL", Toast.LENGTH_SHORT).show()
            return
        }

        val proto = PROTOCOLS.find { it.id == protocol }
        val url   = autoPrefixScheme(rawUrl, proto ?: PROTOCOLS.first())

        if (url != rawUrl) input = url
        savedUrl = url
        SharedPrefs.setStreamUrl(url)
        SharedPrefs.setStreamType(proto?.label ?: protocol)
        onSaved(url)
        saved = true
    }

    LaunchedEffect(saved) {
        if (saved) {
            kotlinx.coroutines.delay(2200)
            saved = false
        }
    }

    fun startInjection() {
        if (!Settings.canDrawOverlays(context)) { showOverlayPermDialog = true; return }
        val rawUrl = input.trim()
        if (rawUrl.isEmpty()) {
            Toast.makeText(context, "Configure a stream URL first", Toast.LENGTH_SHORT).show()
            return
        }


        val proto  = PROTOCOLS.find { it.id == protocol }
        val url    = autoPrefixScheme(rawUrl, proto ?: PROTOCOLS.first())
        if (url != rawUrl) input = url

        val pkg = targetPackage ?: SharedPrefs.getTargetPackage()
        if (pkg.isNullOrEmpty()) {
            Toast.makeText(context, "No target app selected", Toast.LENGTH_LONG).show()
            return
        }



        if (AppState.useNativeHook) {
            if (isStarting) return
            isStarting = true
            val appName = targetAppName ?: SharedPrefs.getTargetAppName()
            scope.launch(Dispatchers.IO) {
                val ok = NativeFrameProducer.start(url)
                withContext(Dispatchers.Main) {
                    AppState.injectionSource = "stream"
                    AppState.activeTargetPackage = pkg
                    isInjecting = ok
                    isStarting = false
                    if (ok) OverlayService.start(context)
                    Toast.makeText(
                        context,
                        if (ok) "Native injection started for $appName"
                        else "Native injection failed — check root & hook status",
                        Toast.LENGTH_SHORT
                    ).show()
                }
            }
            return
        }

        val mediaUri = SharedPrefs.getLastUsedUrl()
        if (!mediaUri.isNullOrEmpty()) {
            pendingStreamUrl = url
            pendingMediaUri  = mediaUri
            pendingPkg       = pkg
            showSourceDialog = true
            return
        }
        InjectionService.start(context, pkg, streamUrl = url)
        AppState.injectionSource = "stream"

        isInjecting = true
        val appName = targetAppName ?: SharedPrefs.getTargetAppName()
        Toast.makeText(context, "Injection started for $appName", Toast.LENGTH_SHORT).show()
    }

    fun stopInjection() {
        if (AppState.useNativeHook) {
            /* [V80 ANR fix] nativeStop() tears the GStreamer pipeline down
             * synchronously and blocked the UI thread past the 10 s watchdog.
             * Field evidence: anr_14.txt from SM-X200 / Android 14 / V78 —
             *   "main" RUNNABLE
             *     at NativeFrameProducer.nativeStop(Native Method)
             *     at NativeFrameProducer.stop
             *     at StreamSetupContent$stopInjection
             *     at ... dispatchTouchEvent
             * Same Dispatchers.IO pattern MediaFragment.stopInjection() already
             * uses; UI/state mutations stay on Main. */
            scope.launch(Dispatchers.IO) {
                NativeFrameProducer.stop()
                withContext(Dispatchers.Main) {
                    OverlayService.stop(context)
                    AppState.injectionSource = null
                    AppState.activeTargetPackage = null
                    isInjecting = false
                    Toast.makeText(context, "Native injection stopped", Toast.LENGTH_SHORT).show()
                }
            }
            return
        }
        InjectionService.stop(context)
        AppState.injectionSource = null
        AppState.activeTargetPackage = null
        isInjecting = false
        Toast.makeText(context, "Injection stopped", Toast.LENGTH_SHORT).show()
    }


    if (showOverlayPermDialog) {
        AlertDialog(
            onDismissRequest = { showOverlayPermDialog = false },
            properties = DialogProperties(usePlatformDefaultWidth = false),
            containerColor = Color(0xFF1A1A2E),
            title = {
                Text("Overlay Permission Required", color = Color.White, fontSize = 16.sp)
            },
            text = {
                Text(
                    "EcomCam needs the \"Display over other apps\" permission to show " +
                    "pan/zoom floating controls while injection is active.\n\n" +
                    "Tap \"Open Settings\", find EcomCam in the list, and enable the toggle.",
                    color = Color(0xAAFFFFFF), fontSize = 13.sp
                )
            },
            confirmButton = {
                Box(
                    modifier = Modifier
                        .clip(RoundedCornerShape(12.dp))
                        .background(Violet)
                        .clickable {
                            showOverlayPermDialog = false
                            overlayPermLauncher.launch(
                                Intent(
                                    Settings.ACTION_MANAGE_OVERLAY_PERMISSION,
                                    Uri.parse("package:" + context.packageName)
                                )
                            )
                        }
                        .padding(horizontal = 20.dp, vertical = 10.dp)
                ) { Text("Open Settings", color = Color.White, fontSize = 13.sp) }
            },
            dismissButton = {
                Box(
                    modifier = Modifier
                        .clip(RoundedCornerShape(12.dp))
                        .background(Color(0x33FFFFFF))
                        .clickable { showOverlayPermDialog = false }
                        .padding(horizontal = 20.dp, vertical = 10.dp)
                ) { Text("Skip", color = Color(0xAAFFFFFF), fontSize = 13.sp) }
            }
        )
    }

        if (showSourceDialog) {
        AlertDialog(
            onDismissRequest = { showSourceDialog = false },
            properties = DialogProperties(usePlatformDefaultWidth = false),
            containerColor = Color(0xFF16162A),
            shape = RoundedCornerShape(24.dp),
            tonalElevation = 0.dp,
            title = {
                Text(
                    "Choose Injection Mode",
                    color = Color.White,
                    fontSize = 16.sp,
                    fontFamily = androidx.compose.ui.text.font.FontFamily.Default
                )
            },
            text = {
                Text(
                    "Both a live stream URL and a local media file are configured.\nWhich source should be injected?",
                    color = TextMid,
                    fontSize = 13.sp,
                    lineHeight = 19.sp
                )
            },
            confirmButton = {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(bottom = 4.dp),
                    horizontalArrangement = Arrangement.spacedBy(8.dp, Alignment.End)
                ) {

                    Box(
                        modifier = Modifier
                            .clip(RoundedCornerShape(12.dp))
                            .background(Color(0x1AFFFFFF))
                            .border(1.dp, Border, RoundedCornerShape(12.dp))
                            .clickable { showSourceDialog = false }
                            .padding(horizontal = 14.dp, vertical = 9.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text("Cancel", color = TextSec, fontSize = 13.sp)
                    }


                    Box(
                        modifier = Modifier
                            .clip(RoundedCornerShape(12.dp))
                            .background(Color(0x334ADE80))
                            .border(1.dp, GreenOk.copy(alpha = 0.35f), RoundedCornerShape(12.dp))
                            .clickable {
                                showSourceDialog = false
                                val mUri = pendingMediaUri ?: return@clickable
                                val p    = pendingPkg     ?: return@clickable
                                if (AppState.useNativeHook) {
                                    if (!isStarting) {
                                        isStarting = true
                                        val uri = android.net.Uri.parse(mUri)
                                        scope.launch(Dispatchers.IO) {
                                            val ok = NativeFrameProducer.startWithUri(context, uri)
                                            withContext(Dispatchers.Main) {
                                                AppState.injectionSource = "media"
                                                isInjecting = ok
                                                isStarting = false
                                                Toast.makeText(
                                                    context,
                                                    if (ok) "Injection started (Local Media)" else "Injection failed",
                                                    Toast.LENGTH_SHORT
                                                ).show()
                                            }
                                        }
                                    }
                                } else {
                                    InjectionService.start(context, p, mediaUri = mUri, streamUrl = null)
                                    AppState.injectionSource = "media"
                                    isInjecting = true
                                    Toast.makeText(context, "Injection started (Local Media)", Toast.LENGTH_SHORT).show()
                                }
                            }
                            .padding(horizontal = 14.dp, vertical = 9.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text("Local Media", color = GreenOk, fontSize = 13.sp)
                    }


                    Box(
                        modifier = Modifier
                            .clip(RoundedCornerShape(12.dp))
                            .background(Brush.linearGradient(listOf(Violet, Pink)))
                            .clickable {
                                showSourceDialog = false
                                val sUrl = pendingStreamUrl ?: return@clickable
                                val p    = pendingPkg       ?: return@clickable
                                if (AppState.useNativeHook) {
                                    if (!isStarting) {
                                        isStarting = true
                                        scope.launch(Dispatchers.IO) {
                                            val ok = NativeFrameProducer.start(sUrl)
                                            withContext(Dispatchers.Main) {
                                                AppState.injectionSource = "stream"
                                                isInjecting = ok
                                                isStarting = false
                                                Toast.makeText(
                                                    context,
                                                    if (ok) "Injection started" else "Injection failed",
                                                    Toast.LENGTH_SHORT
                                                ).show()
                                            }
                                        }
                                    }
                                } else {
                                    SharedPrefs.setLastUsedUrl(null)
                                    InjectionService.start(context, p, streamUrl = sUrl, mediaUri = null)
                                    AppState.injectionSource = "stream"
                                    isInjecting = true
                                    Toast.makeText(context, "Injection started (Live Stream)", Toast.LENGTH_SHORT).show()
                                }
                            }
                            .padding(horizontal = 14.dp, vertical = 9.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text("Live Stream", color = Color.White, fontSize = 13.sp)
                    }
                }
            }
        )
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 20.dp, vertical = 20.dp),
        verticalArrangement = Arrangement.spacedBy(20.dp)
    ) {
        Column {
            Text("Stream Configuration", color = Color.White, fontSize = 16.sp, fontWeight = FontWeight.Bold)
            Text("Enter your stream URL", color = TextSec, fontSize = 12.sp)
        }


        val rows = PROTOCOLS.chunked(4)
        Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
            rows.forEach { row ->
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    row.forEach { p ->
                        val isSelected = protocol == p.id
                        Box(
                            modifier = Modifier
                                .weight(1f)
                                .clip(RoundedCornerShape(16.dp))
                                .background(if (isSelected) Color(0x336C63FF) else Surface)
                                .border(1.5.dp, if (isSelected) Violet else Color.Transparent, RoundedCornerShape(16.dp))
                                .clickable { protocol = p.id }
                                .padding(vertical = 10.dp),
                            contentAlignment = Alignment.Center
                        ) {
                            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                                Text(p.label, color = if (isSelected) Violet else Color(0xAAFFFFFF),
                                    fontSize = 11.sp, fontWeight = FontWeight.Bold)
                                Text(p.hint, color = TextSec, fontSize = 8.sp)
                            }
                        }
                    }

                    repeat(4 - row.size) { Spacer(Modifier.weight(1f)) }
                }
            }
        }


        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(16.dp))
                .background(Surface)
                .border(1.5.dp, Border, RoundedCornerShape(16.dp))
                .padding(horizontal = 16.dp, vertical = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Text("📶", fontSize = 14.sp, color = Violet.copy(alpha = 0.8f))
            BasicTextField(
                value = input,
                onValueChange = { input = it },
                modifier = Modifier.weight(1f).padding(vertical = 14.dp),
                textStyle = androidx.compose.ui.text.TextStyle(
                    color = Color.White, fontSize = 14.sp, fontFamily = FontFamily.Monospace),
                singleLine = true,
                decorationBox = { inner ->
                    if (input.isEmpty()) Text(
                        "Paste ${PROTOCOLS.find { it.id == protocol }?.label ?: ""} stream URL...",
                        color = TextSec, fontSize = 14.sp)
                    inner()
                },
                cursorBrush = SolidColor(Violet)
            )
        }


        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(56.dp)
                .clip(RoundedCornerShape(16.dp))
                .background(
                    if (input.trim().isNotEmpty()) Brush.linearGradient(listOf(Violet, Pink))
                    else Brush.linearGradient(listOf(Color(0x1AFFFFFF), Color(0x1AFFFFFF)))
                )
                .clickable(enabled = input.trim().isNotEmpty()) { handleSave() },
            contentAlignment = Alignment.Center
        ) {
            AnimatedContent(targetState = saved, label = "saveBtn") { isSaved ->
                if (isSaved) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("✔", color = Color.White, fontSize = 14.sp)
                        Text("Saved!", color = Color.White, fontWeight = FontWeight.SemiBold, fontSize = 14.sp)
                    }
                } else {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        Text("💾", fontSize = 14.sp)
                        Text("Save & Apply", color = if (input.trim().isNotEmpty()) Color.White else TextSec,
                            fontWeight = FontWeight.SemiBold, fontSize = 14.sp)
                    }
                }
            }
        }


        if (!isInjecting) {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(52.dp)
                    .clip(RoundedCornerShape(16.dp))
                    .background(if (input.trim().isNotEmpty()) Color(0x334ADE80) else Color(0x1AFFFFFF))
                    .border(1.dp, if (input.trim().isNotEmpty()) GreenOk.copy(0.4f) else Color.Transparent, RoundedCornerShape(16.dp))
                    .clickable(enabled = input.trim().isNotEmpty() && !otherActive) { startInjection() },
                contentAlignment = Alignment.Center
            ) {
                Text(
                    if (otherActive) "⚠  Media Injection Active" else "▶  Start Injection",
                    color = if (otherActive) Color(0xFFFF9800) else if (input.trim().isNotEmpty()) GreenOk else TextSec,
                    fontWeight = FontWeight.SemiBold, fontSize = 14.sp)
            }
        } else {
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(52.dp)
                    .clip(RoundedCornerShape(16.dp))
                    .background(Color(0x33FF4D6D))
                    .border(1.dp, Color(0x55FF4D6D), RoundedCornerShape(16.dp))
                    .clickable { stopInjection() },
                contentAlignment = Alignment.Center
            ) {
                Text("⏹  Stop Injection", color = Color(0xFFFF4D6D),
                    fontWeight = FontWeight.SemiBold, fontSize = 14.sp)
            }
        }
    }
}

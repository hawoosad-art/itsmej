package com.itsme.amkush.ui
import android.annotation.SuppressLint
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.drawable.Drawable
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.animation.*
import androidx.compose.animation.core.*
import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.*
import androidx.compose.foundation.shape.*
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CameraAlt
import androidx.compose.material.icons.filled.ChevronRight
import androidx.compose.material.icons.filled.Close
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Videocam
import androidx.compose.material3.*
import androidx.compose.material3.AlertDialog
import androidx.compose.runtime.*
import androidx.compose.ui.*
import androidx.compose.ui.draw.*
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.*
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.*
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.*
import com.itsme.amkush.AppState
import com.itsme.amkush.model.AppInfo
import com.itsme.amkush.network.ApiClient
import com.itsme.amkush.network.models.TokenRequest
import com.itsme.amkush.security.LicenseGuard
import com.itsme.amkush.services.InjectionService
import com.itsme.amkush.utils.DeviceUtils
import com.itsme.amkush.utils.Logger
import com.itsme.amkush.utils.SharedPrefs
import kotlinx.coroutines.*
import kotlinx.coroutines.withContext
import java.net.Inet4Address
import java.net.NetworkInterface

import com.itsme.amkush.manager.ModuleManager
import com.itsme.amkush.notification.CrashLoopNotifier
import android.app.Activity

private val BgDark   = Color(0xFF0D0D18)
private val Surface  = Color(0x12FFFFFF)
private val Border   = Color(0x1AFFFFFF)
private val Violet   = Color(0xFF6C63FF)
private val Pink     = Color(0xFFFF4D9D)
private val Cyan     = Color(0xFF00D4FF)
private val GreenOk  = Color(0xFF4ADE80)
private val OrangeWait = Color(0xFFFF6B35)
private val TextSec  = Color(0x44FFFFFF)
private val TextMid  = Color(0x88FFFFFF)
private val RedHook  = Color(0xFFFF1744)
private val InactiveGray = Color(0xFF8892A4)
private val TextTertiary = Color(0xFF5A6474)

class HomeScreen : ComponentActivity() {
@SuppressLint("QueryPermissionsNeeded")
 override fun onCreate(savedInstanceState: Bundle?) {
     super.onCreate(savedInstanceState)
     SharedPrefs.init(this)
     setContent {
         var showAdmin     by remember { mutableStateOf(false) }
         var showModeDialog by remember { mutableStateOf(!SharedPrefs.isModeSelected()) }
         var rootCheckFailed by remember { mutableStateOf(false) }
         var modeChecking   by remember { mutableStateOf(false) }
         val context = LocalContext.current

         val dialogScope = rememberCoroutineScope()

         if (showModeDialog) {
             AlertDialog(
                 onDismissRequest = {  },
                 containerColor = Color(0xFF16162A),
                 shape = androidx.compose.foundation.shape.RoundedCornerShape(24.dp),
                 tonalElevation = 0.dp,
                 title = {
                     Column(horizontalAlignment = Alignment.CenterHorizontally) {
                         Text("Select Operation Mode",
                             color = Color.White, fontWeight = FontWeight.Bold, fontSize = 18.sp)
                         Spacer(Modifier.height(4.dp))
                         Text("Choose how FaceGate will hook the camera",
                             color = Color(0x88FFFFFF), fontSize = 12.sp,
                             textAlign = TextAlign.Center)
                     }
                 },
                 text = {
                     Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                         if (rootCheckFailed) {
                             Box(
                                 modifier = Modifier.fillMaxWidth()
                                     .clip(RoundedCornerShape(12.dp))
                                     .background(Color(0x1AFF4D6D))
                                     .border(1.dp, Color(0x44FF4D6D), RoundedCornerShape(12.dp))
                                     .padding(12.dp)
                             ) {
                                 Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                                     Text("⚠ Root access not found",
                                         color = Color(0xFFFF4D6D),
                                         fontWeight = FontWeight.SemiBold, fontSize = 13.sp)
                                     Text("Your device/emulator does not have root. Non-Root mode (via app cloner) will be used.",
                                         color = Color(0x99FFFFFF), fontSize = 11.sp)
                                 }
                             }
                         }
                         if (!rootCheckFailed) {

                             Box(
                                 modifier = Modifier.fillMaxWidth()
                                     .clip(RoundedCornerShape(16.dp))
                                     .background(Color(0x1200FF64))
                                     .border(1.5.dp, Color(0x8000FF64), RoundedCornerShape(16.dp))
                                     .clickable(enabled = !modeChecking) {
                                         modeChecking = true
                                         dialogScope.launch(kotlinx.coroutines.Dispatchers.IO) {
                                             val hasRoot = tryGrantRoot()
                                             withContext(kotlinx.coroutines.Dispatchers.Main) {
                                                 modeChecking = false
                                                 if (hasRoot) {
                                                     SharedPrefs.setRootMode(true)
                                                     SharedPrefs.setModeSelected(true)
                                                     showModeDialog = false
                                                 } else {
                                                     rootCheckFailed = true
                                                 }
                                             }
                                         }
                                     }
                                     .padding(horizontal = 20.dp, vertical = 16.dp),
                                 contentAlignment = Alignment.Center
                             ) {
                                 if (modeChecking) {
                                     CircularProgressIndicator(color = Color(0xFF00FF64),
                                         modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
                                 } else {
                                     Row(verticalAlignment = Alignment.CenterVertically,
                                         horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                                         Box(modifier = Modifier.size(10.dp)
                                             .background(Color(0xFF00FF64),
                                                 androidx.compose.foundation.shape.CircleShape))
                                         Column {
                                             Text("ROOT MODE",
                                                 color = Color(0xFF00FF64),
                                                 fontWeight = FontWeight.Bold,
                                                 fontSize = 13.sp,
                                                 letterSpacing = 1.5.sp)
                                             Text("Requires rooted device or emulator",
                                                 color = Color(0x8800FF64), fontSize = 10.sp)
                                         }
                                     }
                                 }
                             }
                         }

                         Box(
                             modifier = Modifier.fillMaxWidth()
                                 .clip(RoundedCornerShape(16.dp))
                                 .background(Color(0x12FF2828))
                                 .border(1.5.dp, Color(0x80FF2828), RoundedCornerShape(16.dp))
                                 .clickable {
                                     SharedPrefs.setRootMode(false)
                                     SharedPrefs.setModeSelected(true)
                                     showModeDialog = false
                                 }
                                 .padding(horizontal = 20.dp, vertical = 16.dp),
                             contentAlignment = Alignment.Center
                         ) {
                             Row(verticalAlignment = Alignment.CenterVertically,
                                 horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                                 Box(modifier = Modifier.size(10.dp)
                                     .background(Color(0xFFFF2828),
                                         androidx.compose.foundation.shape.CircleShape))
                                 Column {
                                     Text("NON ROOT MODE",
                                         color = Color(0xFFFF2828),
                                         fontWeight = FontWeight.Bold,
                                         fontSize = 13.sp,
                                         letterSpacing = 1.5.sp)
                                     Text("Use with Mochi Cloner / app cloners",
                                         color = Color(0x88FF2828), fontSize = 10.sp)
                                 }
                             }
                         }
                     }
                 },
                 confirmButton = {}
             )
         }
         if (showAdmin) {
             AdminCreditsScreen(onBack = { showAdmin = false })
         } else {
             HomeScreenContent(
                 onShowAdmin = { showAdmin = true },
                 onProceedToDashboard = { app ->
                     val intent = Intent(this, TabsScreen::class.java).apply {
                         putExtra("target_package", app.packageName)
                         putExtra("target_app_name", app.appName)
                     }
                     startActivity(intent)
                 },
                 onProceedToPayment = { app ->
                     val intent = Intent(this, PaymentScreen::class.java).apply {
                         putExtra("target_package", app.packageName)
                         putExtra("target_app_name", app.appName)
                     }
                     startActivity(intent)
                 }
             )
         }
     }
 }
}

private fun getCurrentIpAddress(context: Context): String {

try {
val wm = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? android.net.wifi.WifiManager
val ip = wm?.connectionInfo?.ipAddress ?: 0
if (ip != 0) {
return String.format("%d.%d.%d.%d", ip and 0xff, ip shr 8 and 0xff, ip shr 16 and 0xff, ip shr 24 and 0xff)
}
} catch (e: Exception) {}

try {
val ifaces = NetworkInterface.getNetworkInterfaces()
while (ifaces.hasMoreElements()) {
val iface = ifaces.nextElement()
if (iface.isLoopback || !iface.isUp) continue
val addrs = iface.inetAddresses
while (addrs.hasMoreElements()) {
val addr = addrs.nextElement()
if (addr is Inet4Address && !addr.isLoopbackAddress) {
return addr.hostAddress ?: continue
}
}
}
} catch (e: Exception) {}
return "Unavailable"
}

@SuppressLint("QueryPermissionsNeeded")
@Composable
private fun HomeScreenContent(
    onShowAdmin: () -> Unit,
    onProceedToDashboard: (AppInfo) -> Unit,
    onProceedToPayment: (AppInfo) -> Unit
) {
    val context = LocalContext.current
    var appList      by remember { mutableStateOf<List<AppInfo>>(emptyList()) }
    var loading      by remember { mutableStateOf(true) }
    var selectedApp  by remember { mutableStateOf<AppInfo?>(null) }
    var showAppList  by remember { mutableStateOf(false) }
    var search       by remember { mutableStateOf("") }
    var locking      by remember { mutableStateOf(false) }


    val moduleManager = remember { ModuleManager(context) }
    val contentScope = rememberCoroutineScope()
    var moduleStatus by remember {
        mutableStateOf(
            ModuleManager.ModuleStatus(
                installed = false, enabled = false, active = false,
                safetyState = ModuleManager.SafetyState.NO_ROOT, crashCount = 0,
                manager = null, zygiskNextInstalled = false,
                logTail = "", cameraserverPid = "", hookMapsLine = "", moduleVersion = ""
            )
        )
    }

    var isInjectingNow by remember { mutableStateOf(false) }
    var injectResultMsg by remember { mutableStateOf<String?>(null) }

    // Anti-tamper: verified at app startup. If the signing cert is not the
    // expected production one (repackaged/re-signed clone), show a red popup and
    // exit after 10s, self-destructing local activation.
    var tampered by remember { mutableStateOf(false) }
    var tamperCountdown by remember { mutableStateOf(10) }

    LaunchedEffect(Unit) {
        val attestOk = withContext(Dispatchers.IO) {
            try { LicenseGuard.nativeCheckAttestation() } catch (e: Exception) { true }
        }
        if (!attestOk) {
            tampered = true
            while (tamperCountdown > 0) {
                delay(1000)
                tamperCountdown -= 1
            }
            try {
                SharedPrefs.clearActivation()
                LicenseGuard.nativeClearActivation(context)
            } catch (_: Exception) {}
            android.os.Process.killProcess(android.os.Process.myPid())
        }
    }

    LaunchedEffect(Unit) {
        while (true) {
            moduleStatus = withContext(Dispatchers.IO) { moduleManager.getStatus() }
            delay(2000)
        }
    }

    var ipAddress by remember { mutableStateOf(getCurrentIpAddress(context)) }
    LaunchedEffect(Unit) {
        while (true) {
            ipAddress = getCurrentIpAddress(context)
            delay(5_000)
        }
    }

    var frameCount by remember { mutableStateOf(0) }
    val isInjecting = remember { mutableStateOf(InjectionService.isRunning) }
    LaunchedEffect(Unit) {
        while (true) {
            isInjecting.value = InjectionService.isRunning
            frameCount = AppState.frameCount.get().toInt()
            delay(1_000)
        }
    }
    val hasStream = (SharedPrefs.getStreamUrl() ?: "").isNotEmpty()
    val mediaReady = isInjecting.value && hasStream
    val filteredApps = remember(appList, search) {
        val q = search.lowercase().trim()
        if (q.isEmpty()) appList
        else appList.filter { it.appName.lowercase().contains(q) || it.packageName.lowercase().contains(q) }
    }

    val infiniteTransition = rememberInfiniteTransition(label = "logoRot")
    val rotation by infiniteTransition.animateFloat(
        initialValue = 0f,
        targetValue = 360f,
        animationSpec = infiniteRepeatable(
            animation = tween(durationMillis = 8000, easing = LinearEasing),
            repeatMode = RepeatMode.Restart
        ),
        label = "logoRotation"
    )

    LaunchedEffect(Unit) {
        withContext(Dispatchers.IO) {
            try {
                val pm = context.packageManager
                val pkgs = pm.getInstalledApplications(0)
                val apps = pkgs
                    .filter { pkg ->
                        val isSys = (pkg.flags and android.content.pm.ApplicationInfo.FLAG_SYSTEM) != 0
                        val isUpdatedSys = (pkg.flags and android.content.pm.ApplicationInfo.FLAG_UPDATED_SYSTEM_APP) != 0

                        (!isSys || isUpdatedSys) && pkg.packageName != context.packageName
                    }
                    .map { pkg ->
                        val name = pm.getApplicationLabel(pkg).toString()
                        val icon = try { pm.getApplicationIcon(pkg) } catch (_: Exception) { null }
                        AppInfo(pkg.packageName, name, icon, false)
                    }
                    .sortedBy { it.appName.lowercase() }
                withContext(Dispatchers.Main) {
                    appList = apps
                    loading = false
                }
            } catch (e: Exception) {
                withContext(Dispatchers.Main) {
                    loading = false
                    Logger.e("Error loading apps", e)
                }
            }
        }
    }
    LaunchedEffect(appList) {
        if (appList.isNotEmpty()) {
            val pkg  = SharedPrefs.getTargetPackage()
            val name = SharedPrefs.getTargetAppName()
            if (!pkg.isNullOrEmpty() && !name.isNullOrEmpty()) {
                val found = appList.find { it.packageName == pkg }
                if (found != null) selectedApp = found
                else if (selectedApp == null) selectedApp = AppInfo(pkg, name, null)
            }
        }
    }
    fun handleSelectApp(app: AppInfo) {
        selectedApp = app
        showAppList = false
        search = ""
        SharedPrefs.setTargetPackage(app.packageName)
        SharedPrefs.setTargetAppName(app.appName)
    }
    fun handleRestart() {
        selectedApp = null
        showAppList = false
        search = ""
        SharedPrefs.clearTarget()
        SharedPrefs.setStreamUrl(null)
        SharedPrefs.setStreamType(null)
        SharedPrefs.setLastUsedUrl(null)
        if (InjectionService.isRunning) {
            InjectionService.stop(context)
            isInjecting.value = false
        }
        Toast.makeText(context, "System reset", Toast.LENGTH_SHORT).show()
    }
    fun openTelegram() {
        try {
            val url = LicenseGuard.nativeGetTgOwner()
            context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        } catch (_: Exception) {
            Toast.makeText(context, "Could not open Telegram", Toast.LENGTH_SHORT).show()
        }
    }
    fun handleHookCamera() {
        val app = selectedApp ?: run { showAppList = true; return }
        locking = true
        CoroutineScope(Dispatchers.IO).launch {
            val token = SharedPrefs.getActivationToken()
            if (!token.isNullOrEmpty()) {
                try {
                    val deviceId = DeviceUtils.getDeviceId(context)
                    val request  = TokenRequest(token, deviceId)
                    val response = ApiClient.getApiService().verifyToken(request).execute()
                    withContext(Dispatchers.Main) {
                        locking = false
                        if (response.isSuccessful && response.body()?.valid == true) {
                            onProceedToDashboard(app)
                        } else {
                            onProceedToPayment(app)
                        }
                    }
                } catch (e: Exception) {
                    withContext(Dispatchers.Main) {
                        locking = false
                        onProceedToPayment(app)
                    }
                }
            } else {
                withContext(Dispatchers.Main) {
                    locking = false
                    onProceedToPayment(app)
                }
            }
        }
    }
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(BgDark)
            .systemBarsPadding()
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
        ) {
            HomeTopBar(rotation = rotation, onShowAdmin = onShowAdmin)
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp),
                verticalArrangement = Arrangement.spacedBy(14.dp)
            ) {
                ModuleStatusCard(
                    ipAddress = ipAddress,
                    mediaReady = mediaReady,
                    frameCount = frameCount,
                    moduleStatus = moduleStatus,
                    isInjectingNow = isInjectingNow,
                    injectResultMsg = injectResultMsg,
                    onInjectClick = {
                        injectResultMsg = null
                        isInjectingNow = true
                        contentScope.launch {
                            val result = moduleManager.injectNow()
                            val newStatus = withContext(Dispatchers.IO) { moduleManager.getStatus() }
                            withContext(Dispatchers.Main) {
                                isInjectingNow = false
                                moduleStatus = newStatus
                                injectResultMsg = result.fold(
                                    onSuccess = { "Injected into cameraserver PID " + newStatus.cameraserverPid },
                                    onFailure = { it.message ?: "Injection failed" }
                                )
                            }
                        }
                    }
                )
                TargetAppCard(
                    selectedApp = selectedApp,
                    hookActive = moduleStatus.active,
                    onCardClick = { if (selectedApp == null) showAppList = !showAppList },
                    onClear = { selectedApp = null; SharedPrefs.clearTarget() }
                )
                AppListSection(
                    visible = showAppList && moduleStatus.active,
                    loading = loading,
                    search = search,
                    onSearchChange = { search = it },
                    filteredApps = filteredApps,
                    selectedApp = selectedApp,
                    onSelectApp = ::handleSelectApp
                )
                ActionButtonsSection(
                    hookActive = moduleStatus.active,
                    onRestart = ::handleRestart,
                    onSupport = ::openTelegram
                )
                Spacer(Modifier.height(80.dp))
            }
        }
        BottomHookButton(
            selectedApp = selectedApp,
            locking = locking,
            hookActive = moduleStatus.active,
            onHook = ::handleHookCamera,
            onToggleAppList = { showAppList = !showAppList }
        )

        // Spinning "please wait" overlay while the hook is being injected/activated.
        if (isInjectingNow) {
            InjectionWaitingOverlay()
        }

        // Red "tampered" popup — shown at startup on a repackaged/re-signed clone.
        if (tampered) {
            TamperDetectedOverlay(countdown = tamperCountdown)
        }
    }
}

@Composable
private fun TamperDetectedOverlay(countdown: Int) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xE6000000))
            .clickable(enabled = false) { },
        contentAlignment = Alignment.Center
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth(0.88f)
                .clip(RoundedCornerShape(20.dp))
                .background(Color(0xFF1A0505))
                .border(1.dp, Color(0xFFFF1744), RoundedCornerShape(20.dp))
                .padding(24.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(14.dp)
        ) {
            Box(
                modifier = Modifier
                    .size(56.dp)
                    .clip(CircleShape)
                    .background(Color(0x33FF1744)),
                contentAlignment = Alignment.Center
            ) {
                Text("⚠️", fontSize = 28.sp)
            }
            Text(
                "TAMPER DETECTED",
                color = Color(0xFFFF1744),
                fontSize = 18.sp,
                fontWeight = FontWeight.ExtraBold,
                letterSpacing = 1.5.sp
            )
            Text(
                "This app has been tampered with / repackaged.",
                color = Color(0xFFE2E8F0),
                fontSize = 13.sp,
                textAlign = TextAlign.Center
            )
            Text(
                "The app will exit in $countdown s and destroy itself.",
                color = Color(0xFF94A3B8),
                fontSize = 12.sp,
                textAlign = TextAlign.Center
            )
        }
    }
}

@Composable
private fun InjectionWaitingOverlay() {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(Color(0xE6000000))
            .clickable(enabled = false) { },
        contentAlignment = Alignment.Center
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth(0.85f)
                .clip(RoundedCornerShape(24.dp))
                .background(
                    Brush.linearGradient(listOf(Color(0xFC140C28), Color(0xFA0C081C)))
                )
                .border(1.dp, Color(0x5900D4FF), RoundedCornerShape(24.dp))
                .padding(horizontal = 28.dp, vertical = 32.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(18.dp)
        ) {

            val inf = rememberInfiniteTransition(label = "injectSpin")
            val angle by inf.animateFloat(
                initialValue = 0f,
                targetValue = 360f,
                animationSpec = infiniteRepeatable(tween(900, easing = LinearEasing)),
                label = "injectAngle"
            )
            Box(
                modifier = Modifier
                    .size(64.dp)
                    .clip(CircleShape)
                    .background(Color(0x1400D4FF))
                    .border(1.dp, Color(0x4000D4FF), CircleShape),
                contentAlignment = Alignment.Center
            ) {
                Canvas(Modifier.size(40.dp).rotate(angle)) {
                    val r = size.minDimension / 2f
                    drawArc(
                        color = Cyan,
                        startAngle = 0f,
                        sweepAngle = 300f,
                        useCenter = false,
                        style = androidx.compose.ui.graphics.drawscope.Stroke(width = 4.dp.toPx(),
                            cap = androidx.compose.ui.graphics.StrokeCap.Round),
                        topLeft = Offset(size.width / 2f - r, size.height / 2f - r),
                        size = androidx.compose.ui.geometry.Size(r * 2f, r * 2f)
                    )
                }
            }

            Text(
                "Please wait",
                color = Color.White,
                fontSize = 19.sp,
                fontWeight = FontWeight.Bold,
                letterSpacing = 1.sp
            )
            Text(
                "Injecting and activating the hook...",
                color = Color(0x99FFFFFF),
                fontSize = 13.sp,
                textAlign = TextAlign.Center
            )

            val pulse = rememberInfiniteTransition(label = "dotsPulse")
            val dotsAlpha by pulse.animateFloat(
                initialValue = 1f, targetValue = 0.3f,
                animationSpec = infiniteRepeatable(tween(700), RepeatMode.Reverse),
                label = "dotsAlpha"
            )
            Text(
                "Please wait until the hook is injected and active",
                color = Color(0xFF00D4FF).copy(alpha = dotsAlpha),
                fontSize = 11.sp,
                fontWeight = FontWeight.Medium,
                textAlign = TextAlign.Center
            )
        }
    }
}

@Composable
private fun HomeTopBar(
    rotation: Float,
    onShowAdmin: () -> Unit
) {
    val context = LocalContext.current
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 20.dp, vertical = 16.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween
    ) {

        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(12.dp)) {

            Box(
                modifier = Modifier
                    .size(52.dp)
                    .rotate(rotation)
                    .background(
                        brush = Brush.sweepGradient(listOf(Violet, Pink, Cyan, Violet)),
                        shape = CircleShape
                    )
                    .padding(2.dp),
                contentAlignment = Alignment.Center
            ) {
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .background(BgDark, CircleShape),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        "FG",
                        color = Color.White,
                        fontWeight = FontWeight.Black,
                        fontSize = 16.sp,
                        letterSpacing = 1.sp
                    )
                }
            }
            Column {
                Text(
                    "FACEGATE",
                    color = Color.White,
                    fontWeight = FontWeight.Black,
                    fontSize = 18.sp,
                    letterSpacing = 3.sp
                )
                Text(
                    "Camera Access System",
                    color = Cyan.copy(alpha = 0.7f),
                    fontSize = 10.sp,
                    letterSpacing = 0.5.sp
                )
            }
        }

        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {

            val isRoot = SharedPrefs.isRootMode()
            val modeSelected = SharedPrefs.isModeSelected()
            if (modeSelected) {
                Box(
                    modifier = Modifier
                        .clip(RoundedCornerShape(6.dp))
                        .background(
                            if (isRoot) Color(0xFF00FF64).copy(alpha = 0.08f)
                            else Color(0xFFFF2828).copy(alpha = 0.08f)
                        )
                        .border(
                            1.dp,
                            if (isRoot) Color(0xFF00FF64).copy(alpha = 0.5f)
                            else Color(0xFFFF2828).copy(alpha = 0.5f),
                            RoundedCornerShape(6.dp)
                        )
                        .padding(horizontal = 8.dp, vertical = 4.dp)
                ) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(5.dp)
                    ) {
                        Box(
                            modifier = Modifier.size(6.dp).background(
                                if (isRoot) Color(0xFF00FF64) else Color(0xFFFF2828),
                                CircleShape
                            )
                        )
                        Text(
                            if (isRoot) "ROOT" else "NON-ROOT",
                            color = if (isRoot) Color(0xFF00FF64) else Color(0xFFFF2828),
                            fontSize = 9.sp,
                            fontWeight = FontWeight.Bold,
                            letterSpacing = 1.sp
                        )
                    }
                }
            }

            val fgIconBitmap = remember {
                try {
                    val drawable = context.packageManager.getApplicationIcon(context.packageName)
                    drawableToBitmap(drawable)
                } catch (_: Exception) { null }
            }
            Box(
                modifier = Modifier
                    .size(44.dp)
                    .background(Color(0x226C63FF), CircleShape)
                    .border(1.dp, Violet.copy(alpha = 0.4f), CircleShape)
                    .clickable { onShowAdmin() },
                contentAlignment = Alignment.Center
            ) {
                if (fgIconBitmap != null) {
                    androidx.compose.foundation.Image(
                        bitmap = fgIconBitmap.asImageBitmap(),
                        contentDescription = "Admin",
                        modifier = Modifier.size(32.dp).clip(CircleShape)
                    )
                } else {

                    Canvas(modifier = Modifier.size(22.dp)) {
                        val w = size.width; val h = size.height
                        drawCircle(color = Color(0xFFAAAAAA), radius = w * 0.22f,
                            center = Offset(w * 0.5f, h * 0.28f))
                        drawArc(color = Color(0xFFAAAAAA), startAngle = 180f,
                            sweepAngle = 180f, useCenter = false,
                            topLeft = Offset(w * 0.15f, h * 0.52f),
                            size = androidx.compose.ui.geometry.Size(w * 0.7f, h * 0.36f))
                    }
                }
            }
        }
    }
}

@Composable
private fun IpStatusCard(
    ipAddress: String,
    mediaReady: Boolean,
    frameCount: Int
) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(20.dp))
            .background(Color(0xFF0F1624))
            .border(1.dp, Border, RoundedCornerShape(20.dp))
            .padding(horizontal = 20.dp, vertical = 18.dp)
    ) {
        Column {

            Text(
                text = ipAddress,
                color = Cyan,
                fontSize = 26.sp,
                fontWeight = FontWeight.Bold,
                fontFamily = FontFamily.Monospace,
                modifier = Modifier.fillMaxWidth(),
                textAlign = TextAlign.Center
            )
            Spacer(Modifier.height(16.dp))

            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Column {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        val pulse = rememberInfiniteTransition(label = "dot")
                        val dotAlpha by pulse.animateFloat(
                            initialValue = 1f,
                            targetValue = 0.3f,
                            animationSpec = infiniteRepeatable(tween(900), RepeatMode.Reverse),
                            label = "dotAlpha"
                        )
                        Box(
                            modifier = Modifier
                                .size(10.dp)
                                .background(
                                    color = if (mediaReady) GreenOk.copy(alpha = dotAlpha) else OrangeWait.copy(alpha = dotAlpha),
                                    shape = CircleShape
                                )
                        )
                        Text(
                            text = if (mediaReady) "MEDIA READY" else "WAITING",
                            color = if (mediaReady) GreenOk else OrangeWait,
                            fontSize = 12.sp,
                            fontWeight = FontWeight.Bold,
                            letterSpacing = 1.sp
                        )
                    }
                    Spacer(Modifier.height(4.dp))
                    Text(
                        text = if (mediaReady) "Stream available for hook" else "No media stream detected",
                        color = TextMid,
                        fontSize = 11.sp
                    )
                }
                if (mediaReady) {
                    Column(horizontalAlignment = Alignment.End) {
                        Text(
                            text = "$frameCount",
                            color = GreenOk,
                            fontSize = 28.sp,
                            fontWeight = FontWeight.Bold,
                            fontFamily = FontFamily.Monospace
                        )
                        Text(
                            text = "FRAMES",
                            color = GreenOk.copy(alpha = 0.7f),
                            fontSize = 10.sp,
                            fontWeight = FontWeight.Bold,
                            letterSpacing = 1.sp
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun TargetAppCard(
    selectedApp: AppInfo?,
    hookActive: Boolean,
    onCardClick: () -> Unit,
    onClear: () -> Unit
) {
    val contentAlpha = if (hookActive) 1f else 0.45f
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(20.dp))
            .background(Color(0xFF0F1624))
            .border(1.dp, Border, RoundedCornerShape(20.dp))
            .alpha(contentAlpha)
            .clickable(enabled = hookActive) { onCardClick() }
            .padding(16.dp)
    ) {
        if (selectedApp != null) {
            val app = selectedApp
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(16.dp)
            ) {

                val icon = app.icon
                if (icon != null) {
                    val bitmap = remember(app.packageName) { drawableToBitmap(icon) }
                    if (bitmap != null) {
                        Image(
                            bitmap = bitmap.asImageBitmap(),
                            contentDescription = app.appName,
                            modifier = Modifier
                                .size(50.dp)
                                .clip(RoundedCornerShape(12.dp))
                                .border(1.dp, Pink.copy(alpha = 0.4f), RoundedCornerShape(12.dp))
                        )
                    } else {
                        AppIconCircle(app = app, size = 50.dp, cornerRadius = 12.dp)
                    }
                } else {
                    AppIconCircle(app = app, size = 50.dp, cornerRadius = 12.dp)
                }

                Column(Modifier.weight(1f)) {
                    Text(
                        "TARGET LOCKED",
                        color = if (hookActive) Pink else InactiveGray,
                        fontSize = 10.sp,
                        fontWeight = FontWeight.Bold,
                        letterSpacing = 0.5.sp
                    )
                    Spacer(Modifier.height(4.dp))
                    Text(
                        app.appName,
                        color = Color.White,
                        fontWeight = FontWeight.Bold,
                        fontSize = 17.sp,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }

                Box(
                    modifier = Modifier
                        .size(36.dp)
                        .background(Pink.copy(alpha = if (hookActive) 0.15f else 0.05f), CircleShape)
                        .clickable(enabled = hookActive) { onClear() },
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        imageVector = Icons.Filled.Close,
                        contentDescription = "Clear",
                        tint = if (hookActive) Pink else InactiveGray,
                        modifier = Modifier.size(20.dp)
                    )
                }
            }
        } else {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(16.dp)
            ) {
                Icon(
                    imageVector = Icons.Filled.CameraAlt,
                    contentDescription = "Camera",
                    tint = InactiveGray,
                    modifier = Modifier.size(50.dp)
                )

                Column(Modifier.weight(1f)) {
                    Text(
                        "SELECT TARGET",
                        color = InactiveGray,
                        fontWeight = FontWeight.Bold,
                        fontSize = 15.sp
                    )
                    Spacer(Modifier.height(4.dp))
                    Text(
                        if (hookActive) "Choose app to hook camera"
                        else "Inject the hook first, then select target",
                        color = TextTertiary,
                        fontSize = 12.sp
                    )
                }

                Icon(
                    imageVector = Icons.Filled.ChevronRight,
                    contentDescription = "Arrow",
                    tint = TextTertiary,
                    modifier = Modifier.size(28.dp)
                )
            }
        }
    }
}

@Composable
private fun AppListSection(
    visible: Boolean,
    loading: Boolean,
    search: String,
    onSearchChange: (String) -> Unit,
    filteredApps: List<AppInfo>,
    selectedApp: AppInfo?,
    onSelectApp: (AppInfo) -> Unit
) {
    AnimatedVisibility(
        visible = visible,
        enter = expandVertically() + fadeIn(),
        exit = shrinkVertically() + fadeOut()
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(20.dp))
                .background(Color(0x0DFFFFFF))
                .border(1.dp, Border, RoundedCornerShape(20.dp))
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp, vertical = 12.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text("🔍", fontSize = 13.sp, color = TextMid)
                BasicTextField(
                    value = search,
                    onValueChange = onSearchChange,
                    modifier = Modifier.weight(1f),
                    textStyle = androidx.compose.ui.text.TextStyle(color = Color.White, fontSize = 14.sp),
                    singleLine = true,
                    decorationBox = { inner ->
                        if (search.isEmpty()) Text("Search apps...", color = TextSec, fontSize = 14.sp)
                        inner()
                    },
                    cursorBrush = SolidColor(Violet)
                )
                if (search.isNotEmpty()) {
                    Text("✕", modifier = Modifier.clickable { onSearchChange("") }, color = TextSec, fontSize = 13.sp)
                }
            }
            HorizontalDivider(color = Color(0x14FFFFFF))
            if (loading) {
                Box(Modifier.fillMaxWidth().height(100.dp), contentAlignment = Alignment.Center) {
                    CircularProgressIndicator(color = Violet, modifier = Modifier.size(28.dp))
                }
            } else {
                LazyColumn(modifier = Modifier.heightIn(max = 260.dp)) {
                    items(filteredApps, key = { it.packageName }) { app ->
                        AppListRow(
                            app = app,
                            isSelected = selectedApp?.packageName == app.packageName,
                            onClick = { onSelectApp(app) }
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun AppListRow(
    app: AppInfo,
    isSelected: Boolean,
    onClick: () -> Unit
) {
    Column {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clickable { onClick() }
                .padding(horizontal = 16.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            AppIconCircle(app = app, size = 40.dp)
            Column(Modifier.weight(1f)) {
                Text(app.appName, color = Color.White, fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold, maxLines = 1, overflow = TextOverflow.Ellipsis)
                Text(app.packageName, color = TextSec, fontSize = 9.sp,
                    fontFamily = FontFamily.Monospace, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            if (isSelected) {
                Text("✔", color = Violet, fontSize = 14.sp)
            }
        }
        HorizontalDivider(color = Color(0x0AFFFFFF))
    }
}

@Composable
private fun ActionButtonsSection(
    hookActive: Boolean,
    onRestart: () -> Unit,
    onSupport: () -> Unit
) {

    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(52.dp)
            .clip(RoundedCornerShape(16.dp))
            .background(Color(0xFF0F1624))
            .border(1.dp, Cyan.copy(alpha = if (hookActive) 0.25f else 0.10f), RoundedCornerShape(16.dp))
            .alpha(if (hookActive) 1f else 0.45f)
            .clickable(enabled = hookActive) { onRestart() },
        contentAlignment = Alignment.Center
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp)
        ) {

            Canvas(modifier = Modifier.size(18.dp)) {
                val r = size.minDimension * 0.38f
                drawArc(
                    color = Cyan,
                    startAngle = -30f,
                    sweepAngle = 270f,
                    useCenter = false,
                    topLeft = Offset(size.width / 2f - r, size.height / 2f - r),
                    size = androidx.compose.ui.geometry.Size(r * 2f, r * 2f),
                    style = androidx.compose.ui.graphics.drawscope.Stroke(width = 2.5f)
                )

                val tipX = size.width / 2f + r
                val tipY = size.height / 2f
                drawLine(Cyan, Offset(tipX - 4f, tipY - 4f), Offset(tipX, tipY), strokeWidth = 2.5f)
                drawLine(Cyan, Offset(tipX - 4f, tipY + 4f), Offset(tipX, tipY), strokeWidth = 2.5f)
            }
            Text(
                "Restart System",
                color = Cyan,
                fontWeight = FontWeight.SemiBold,
                fontSize = 14.sp
            )
        }
    }

    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(52.dp)
            .clip(RoundedCornerShape(16.dp))
            .background(Color(0xFF0F1624))
            .border(1.dp, Cyan.copy(alpha = if (hookActive) 0.25f else 0.10f), RoundedCornerShape(16.dp))
            .alpha(if (hookActive) 1f else 0.45f)
            .clickable(enabled = hookActive) { onSupport() },
        contentAlignment = Alignment.Center
    ) {
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp)
        ) {

            Canvas(modifier = Modifier.size(18.dp)) {
                val pts = listOf(
                    Offset(size.width * 0.05f, size.height * 0.5f),
                    Offset(size.width * 0.95f, size.height * 0.12f),
                    Offset(size.width * 0.62f, size.height * 0.88f),
                    Offset(size.width * 0.38f, size.height * 0.62f),
                    Offset(size.width * 0.05f, size.height * 0.5f),
                )
                for (i in 0 until pts.size - 1) {
                    drawLine(Cyan, pts[i], pts[i + 1], strokeWidth = 2f)
                }
                drawLine(Cyan, pts[3], pts[1], strokeWidth = 2f)
            }
            Text(
                "Support & Updates",
                color = Cyan,
                fontWeight = FontWeight.SemiBold,
                fontSize = 14.sp
            )
        }
    }
}

@Composable
private fun BoxScope.BottomHookButton(
    selectedApp: AppInfo?,
    locking: Boolean,
    hookActive: Boolean,
    onHook: () -> Unit,
    onToggleAppList: () -> Unit
) {
    Box(
        modifier = Modifier
            .align(Alignment.BottomCenter)
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 16.dp)
    ) {
        val hasApp = selectedApp != null
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(64.dp)
                .clip(RoundedCornerShape(20.dp))
                .alpha(if (hookActive) 1f else 0.45f)
                .then(
                    if (hasApp && hookActive)
                        Modifier.background(Brush.linearGradient(listOf(RedHook, Pink)))
                    else
                        Modifier.background(Color(0xFF1E2330))
                )
                .clickable(enabled = !locking && hookActive) {
                    if (hasApp) onHook() else onToggleAppList()
                },
            contentAlignment = Alignment.Center
        ) {
            if (locking) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    CircularProgressIndicator(color = Color.White, modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
                    Text("Locking...", color = Color.White, fontWeight = FontWeight.Bold, fontSize = 15.sp)
                }
            } else if (hasApp) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    Icon(
                        imageVector = Icons.Filled.Videocam,
                        contentDescription = "Camera",
                        tint = Color.White,
                        modifier = Modifier.size(24.dp)
                    )
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text(
                            "HOOK CAMERA",
                            color = Color.White,
                            fontWeight = FontWeight.Black,
                            fontSize = 15.sp,
                            letterSpacing = 1.sp
                        )
                        Text(
                            "Tap to inject camera hook",
                            color = Color.White.copy(alpha = 0.8f),
                            fontSize = 10.sp
                        )
                    }
                }
            } else {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    Icon(
                        imageVector = Icons.Filled.Lock,
                        contentDescription = "Lock",
                        tint = Color.White,
                        modifier = Modifier.size(24.dp)
                    )
                    Text(
                        "SELECT TARGET",
                        color = Color.White,
                        fontWeight = FontWeight.Black,
                        fontSize = 16.sp,
                        letterSpacing = 1.sp
                    )
                }
            }
        }
    }
}

private suspend fun tryGrantRoot(): Boolean = kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
return@withContext try {
val process = Runtime.getRuntime().exec(arrayOf("su", "-c", "id"))
val finished = kotlinx.coroutines.withTimeoutOrNull(5_000L) {
process.waitFor()
}
val exitCode = finished ?: run { process.destroy(); -1 }
exitCode == 0
} catch (_: Exception) { false }
}

private val Red        = Color(0xFFEF4444)
private val OrangeText = Color(0xFFFB923C)
private val SlateText  = Color(0xFFCBD5E1)
private val CyanText   = Color(0xFF67E8F9)
private data class AdminLink(
val emoji: String,
val title: String,
val handle: String,
val url: String,
val tag: String,
val accentColor: Color
)

private fun adminLinks(): List <AdminLink > = listOf(
AdminLink( "🤖 ",  "Official FaceGate Bot ",  "@Facegateofficialbot ",
try { LicenseGuard.nativeGetTgBot()     } catch (e: Throwable) {  " " },
 "Verified Bot ",  Color(0xFF00AAFF)),
AdminLink( "📢 ",  "Official Channel ",        "@+Tx-rhbl-VcgyNDg0 ",
try { LicenseGuard.nativeGetTgChannel() } catch (e: Throwable) {  " " },
 "Announcements ", Color(0xFFAA44FF)),
AdminLink( " ",  "Owner ",                   "@facegateofficial ",
try { LicenseGuard.nativeGetTgOwner()   } catch (_: Throwable) {  " " },
 "Administrator ", Color(0xFF00CC88)),
)

@Composable
private fun TerminalIcon(color: Color, modifier: Modifier = Modifier) {
Canvas(modifier) {
val W  = size.width; val H = size.height

val sx = W / 28f; val sy = H / 28f

drawRoundRect(
color = color.copy(alpha = 0.85f),
topLeft = androidx.compose.ui.geometry.Offset(2f * sx, 4f * sy),
size = androidx.compose.ui.geometry.Size(24f * sx, 20f * sy),
cornerRadius = androidx.compose.ui.geometry.CornerRadius(3f * sx, 3f * sy),
style = androidx.compose.ui.graphics.drawscope.Stroke(width = 1.4f * sx)
)

drawLine(color.copy(alpha = 0.4f),
androidx.compose.ui.geometry.Offset(2f * sx, 10f * sy),
androidx.compose.ui.geometry.Offset(26f * sx, 10f * sy),
strokeWidth = 1.1f * sx)

drawCircle(color.copy(alpha = 0.50f), radius = 1f * sx, center = androidx.compose.ui.geometry.Offset(6f * sx, 7f * sy))
drawCircle(color.copy(alpha = 0.35f), radius = 1f * sx, center = androidx.compose.ui.geometry.Offset(10f * sx, 7f * sy))
drawCircle(color.copy(alpha = 0.20f), radius = 1f * sx, center = androidx.compose.ui.geometry.Offset(14f * sx, 7f * sy))

drawLine(color.copy(alpha = 0.95f),
androidx.compose.ui.geometry.Offset(6f * sx, 15.5f * sy),
androidx.compose.ui.geometry.Offset(9.5f * sx, 18f * sy),
strokeWidth = 1.5f * sx, cap = androidx.compose.ui.graphics.StrokeCap.Round)
drawLine(color.copy(alpha = 0.95f),
androidx.compose.ui.geometry.Offset(9.5f * sx, 18f * sy),
androidx.compose.ui.geometry.Offset(6f * sx, 20.5f * sy),
strokeWidth = 1.5f * sx, cap = androidx.compose.ui.graphics.StrokeCap.Round)

drawLine(color.copy(alpha = 0.70f),
androidx.compose.ui.geometry.Offset(12f * sx, 18f * sy),
androidx.compose.ui.geometry.Offset(18f * sx, 18f * sy),
strokeWidth = 1.5f * sx, cap = androidx.compose.ui.graphics.StrokeCap.Round)

drawRoundRect(
color = color.copy(alpha = 0.70f),
topLeft = androidx.compose.ui.geometry.Offset(18.5f * sx, 16.2f * sy),
size = androidx.compose.ui.geometry.Size(2.5f * sx, 3.5f * sy),
cornerRadius = androidx.compose.ui.geometry.CornerRadius(0.5f * sx, 0.5f * sy)
)
}
}

@Composable
private fun CheckBullet(color: Color) {
Canvas(Modifier.size(14.dp)) {
val r = size.minDimension / 2f
drawCircle(color.copy(alpha = 0.15f), radius = r)
drawCircle(color.copy(alpha = 0.40f), radius = r,
style = androidx.compose.ui.graphics.drawscope.Stroke(width = 1.dp.toPx()))

val sc = size.minDimension / 14f
drawLine(color, androidx.compose.ui.geometry.Offset(4.5f * sc, 7f * sc),
androidx.compose.ui.geometry.Offset(6.5f * sc, 9f * sc),
strokeWidth = 1.4f * sc, cap = androidx.compose.ui.graphics.StrokeCap.Round)
drawLine(color, androidx.compose.ui.geometry.Offset(6.5f * sc, 9f * sc),
androidx.compose.ui.geometry.Offset(9.5f * sc, 6f * sc),
strokeWidth = 1.4f * sc, cap = androidx.compose.ui.graphics.StrokeCap.Round)
}
}
@Composable
fun AdminCreditsScreen(onBack: () -> Unit) {
val context = LocalContext.current
val scope   = rememberCoroutineScope()

val links   = remember { adminLinks() }
var showTrialModal by remember { mutableStateOf(false) }
 var copied         by remember { mutableStateOf(false) }
 fun copyKey() {
     val cm = context.getSystemService(android.content.Context.CLIPBOARD_SERVICE)
              as android.content.ClipboardManager
     cm.setPrimaryClip(android.content.ClipData.newPlainText("trial_key", "NOWORNEVER"))
     copied = true
     scope.launch { delay(2000L); copied = false }
 }
 val bgBrush = Brush.verticalGradient(
     listOf(Color(0xFF0F0A0A), Color(0xFF0D0F14), Color(0xFF080B12))
 )
 Box(
     modifier = Modifier
         .fillMaxSize()
         .background(bgBrush)
 ) {

     Column(
         modifier = Modifier
             .fillMaxSize()
             .systemBarsPadding()
             .verticalScroll(rememberScrollState())
             .padding(horizontal = 20.dp)
             .padding(top = 28.dp, bottom = 64.dp)
     ) {

         Row(
             modifier = Modifier
                 .clickable { onBack() }
                 .padding(bottom = 24.dp),
             verticalAlignment = Alignment.CenterVertically,
             horizontalArrangement = Arrangement.spacedBy(8.dp)
         ) {
             Canvas(Modifier.size(20.dp)) {
                 val mid = size.height / 2f
                 drawLine(Color(0xCCFFFFFF), Offset(size.width * 0.65f, mid * 0.35f),
                     Offset(size.width * 0.25f, mid), strokeWidth = 4f,
                     cap = androidx.compose.ui.graphics.StrokeCap.Round)
                 drawLine(Color(0xCCFFFFFF), Offset(size.width * 0.25f, mid),
                     Offset(size.width * 0.65f, mid * 1.65f), strokeWidth = 4f,
                     cap = androidx.compose.ui.graphics.StrokeCap.Round)
             }
             Text("Back", color = Color(0xCCFFFFFF), fontSize = 15.sp, fontWeight = FontWeight.Medium)
         }

         Box(
             modifier = Modifier
                 .fillMaxWidth()
                 .padding(bottom = 20.dp),
             contentAlignment = Alignment.Center
         ) {

             Box(
                 modifier = Modifier
                     .size(96.dp)
                     .blur(24.dp)
                     .background(Color(0x1AEF4444), CircleShape)
             )

             Box(
                 modifier = Modifier
                     .size(64.dp)
                     .blur(12.dp)
                     .background(Color(0x26EF4444), CircleShape)
             )

             Box(
                 modifier = Modifier
                     .size(64.dp)
                     .clip(CircleShape)
                     .background(Color(0x99300A0A))
                     .border(1.dp, Color(0x80B02020), CircleShape),
                 contentAlignment = Alignment.Center
             ) {
                 Canvas(Modifier.size(32.dp)) {
                     val w = size.width; val h = size.height
                     val shieldPath = Path().apply {
                         moveTo(w * 0.5f, h * 0.05f)
                         lineTo(w * 0.92f, h * 0.22f)
                         lineTo(w * 0.92f, h * 0.54f)
                         cubicTo(w * 0.92f, h * 0.78f, w * 0.72f, h * 0.94f, w * 0.5f, h * 0.98f)
                         cubicTo(w * 0.28f, h * 0.94f, w * 0.08f, h * 0.78f, w * 0.08f, h * 0.54f)
                         lineTo(w * 0.08f, h * 0.22f)
                         close()
                     }
                     drawPath(shieldPath, color = Red.copy(alpha = 0.15f))
                     drawPath(shieldPath, color = Red,
                         style = androidx.compose.ui.graphics.drawscope.Stroke(width = 3f * density))
                     drawLine(Red, Offset(w * 0.5f, h * 0.32f), Offset(w * 0.5f, h * 0.62f),
                         strokeWidth = 4.5f * density, cap = androidx.compose.ui.graphics.StrokeCap.Round)
                     drawCircle(Red, radius = 3f * density, center = Offset(w * 0.5f, h * 0.76f))
                 }
             }
         }

         Text(
             "STOP: READ BEFORE PROCEEDING",
             color = Color(0xFFFF4444),
             fontSize = 16.sp,
             fontWeight = FontWeight.ExtraBold,
             letterSpacing = 1.sp,
             textAlign = TextAlign.Center,
             modifier = Modifier.fillMaxWidth()
         )
         Spacer(Modifier.height(4.dp))
         Text(
             "THREAT DETECTED",
             color = Color(0xFF6B7280),
             fontSize = 11.sp,
             fontWeight = FontWeight.Bold,
             letterSpacing = 3.sp,
             textAlign = TextAlign.Center,
             modifier = Modifier
                 .fillMaxWidth()
                 .padding(bottom = 28.dp)
         )

         Column(
             modifier = Modifier
                 .fillMaxWidth()
                 .clip(RoundedCornerShape(20.dp))
                 .background(
                     Brush.linearGradient(
                         listOf(Color(0xF228100A), Color(0xE81A0A0A))
                     )
                 )
                 .border(1.dp, Color(0x66B41E1E), RoundedCornerShape(20.dp))
                 .padding(16.dp)
                 .padding(bottom = 4.dp),
             verticalArrangement = Arrangement.spacedBy(12.dp)
         ) {
             Text(
                 "Scammers are circulating infected FaceGate apps.",
                 color = Color(0xE6FFFFFF),
                 fontSize = 13.sp,
                 fontWeight = FontWeight.Medium,
                 textAlign = TextAlign.Center,
                 modifier = Modifier.fillMaxWidth()
             )
             Row(
                 modifier = Modifier
                     .fillMaxWidth()
                     .clip(RoundedCornerShape(12.dp))
                     .background(Color(0xCC3C0F0F))
                     .border(1.dp, Color(0x40C83232), RoundedCornerShape(12.dp))
                     .padding(12.dp),
                 horizontalArrangement = Arrangement.spacedBy(10.dp),
                 verticalAlignment = Alignment.Top
             ) {

                 Canvas(Modifier.size(17.dp)) {
                     val triPath = Path().apply {
                         moveTo(size.width * 0.5f, size.height * 0.05f)
                         lineTo(size.width * 0.96f, size.height * 0.92f)
                         lineTo(size.width * 0.04f, size.height * 0.92f)
                         close()
                     }
                     drawPath(triPath, color = Color(0xFFEAB308).copy(alpha = 0.25f))
                     drawPath(triPath, color = Color(0xFFEAB308),
                         style = androidx.compose.ui.graphics.drawscope.Stroke(width = 2.5f))
                     drawLine(Color(0xFFEAB308),
                         Offset(size.width * 0.5f, size.height * 0.35f),
                         Offset(size.width * 0.5f, size.height * 0.62f),
                         strokeWidth = 3f, cap = androidx.compose.ui.graphics.StrokeCap.Round)
                     drawCircle(Color(0xFFEAB308), 2f, Offset(size.width * 0.5f, size.height * 0.78f))
                 }
                 Text(
                     "DO NOT INSTALL PC VERSIONS. FaceGate.exe / Kima.exe / .msi files are VIRUSES.",
                     color = Color(0xD9FFFFFF),
                     fontSize = 13.sp,
                     modifier = Modifier.weight(1f)
                 )
             }
             Box(
                 modifier = Modifier
                     .fillMaxWidth()
                     .clip(RoundedCornerShape(12.dp))
                     .background(Color(0xE605191F))
                     .border(1.dp, Color(0x4D00A0C8), RoundedCornerShape(12.dp))
                     .padding(horizontal = 16.dp, vertical = 12.dp)
             ) {
                 Text(
                     "UPDATES are broadcast through the official FaceGate bot only.",
                     color = CyanText,
                     fontSize = 13.sp,
                     modifier = Modifier.fillMaxWidth()
                 )
             }
         }
         Spacer(Modifier.height(24.dp))

         val purpleAccent = Color(0xFFA050FF)
         val purpleLight  = Color(0xFFC080FF)
         Column(
             modifier = Modifier
                 .fillMaxWidth()
                 .clip(RoundedCornerShape(20.dp))
                 .background(
                     Brush.linearGradient(listOf(Color(0xF80F0A1E), Color(0xF20A081A)))
                 )
                 .border(1.dp, Color(0x40A050FF), RoundedCornerShape(20.dp))
                 .padding(start = 16.dp, top = 16.dp, end = 16.dp, bottom = 22.dp)
         ) {

             Row(
                 verticalAlignment = Alignment.CenterVertically,
                 horizontalArrangement = Arrangement.spacedBy(12.dp),
                 modifier = Modifier.padding(bottom = 12.dp)
             ) {
                 Box(
                     modifier = Modifier
                         .size(36.dp)
                         .clip(RoundedCornerShape(10.dp))
                         .background(Color(0x40A050FF))
                         .border(1.dp, Color(0x66A050FF), RoundedCornerShape(10.dp)),
                     contentAlignment = Alignment.Center
                 ) {
                     Canvas(Modifier.size(18.dp)) {
                         drawCircle(purpleAccent, radius = size.minDimension / 2f - 2f,
                             style = androidx.compose.ui.graphics.drawscope.Stroke(width = 3.5f * density))
                         drawLine(purpleAccent, Offset(size.width/2f, size.height*0.27f),
                             Offset(size.width/2f, size.height*0.58f), 3.5f * density,
                             cap = androidx.compose.ui.graphics.StrokeCap.Round)
                         drawLine(purpleAccent, Offset(size.width/2f, size.height*0.58f),
                             Offset(size.width*0.72f, size.height*0.72f), 3.5f * density,
                             cap = androidx.compose.ui.graphics.StrokeCap.Round)
                     }
                 }
                 Column(Modifier.weight(1f)) {
                     Text("Free Trial Available", color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.Bold, letterSpacing = 0.5.sp)
                     Text("● 1 Hour Free Access", color = purpleAccent, fontSize = 10.sp, fontWeight = FontWeight.Bold, letterSpacing = 1.5.sp)
                 }
                 Box(
                     modifier = Modifier
                         .clip(CircleShape)
                         .background(Color(0x1EA050FF))
                         .border(1.dp, Color(0x4DA050FF), CircleShape)
                         .padding(horizontal = 10.dp, vertical = 4.dp)
                 ) {
                     Text("FREE", color = purpleAccent, fontSize = 9.sp, fontWeight = FontWeight.ExtraBold, letterSpacing = 1.sp)
                 }
             }
             Box(Modifier.fillMaxWidth().height(1.dp).background(Color(0x1AA050FF)))
             Spacer(Modifier.height(12.dp))

             Box(
                 modifier = Modifier
                     .fillMaxWidth()
                     .clip(RoundedCornerShape(12.dp))
                     .background(Color(0x0FA050FF))
                     .border(1.dp, Color(0x26A050FF), RoundedCornerShape(12.dp))
                     .padding(horizontal = 16.dp, vertical = 12.dp)
             ) {
                 Text(
                     "Try FaceGate free for 1 hour before committing to a purchase. Full access, no limitations.",
                     color = Color(0xCCFFFFFF), fontSize = 13.sp, textAlign = TextAlign.Center,
                     modifier = Modifier.fillMaxWidth()
                 )
             }
             Spacer(Modifier.height(12.dp))

             listOf(
                 "Full access to all FaceGate features",
                 "No payment required to start",
                 "Upgrade anytime during or after trial"
             ).forEach { text ->
                 Row(
                     verticalAlignment = Alignment.CenterVertically,
                     horizontalArrangement = Arrangement.spacedBy(12.dp),
                     modifier = Modifier.padding(vertical = 6.dp)
                 ) {
                     CheckBullet(purpleAccent)
                     Text(text, color = Color(0xB3FFFFFF), fontSize = 12.sp)
                 }
             }
             Spacer(Modifier.height(16.dp))

             Box(
                 modifier = Modifier
                     .fillMaxWidth()
                     .clip(RoundedCornerShape(12.dp))
                     .background(
                         Brush.linearGradient(listOf(Color(0x4DA050FF), Color(0x333222DC)))
                     )
                     .border(1.dp, Color(0x73A050FF), RoundedCornerShape(12.dp))
                     .clickable { showTrialModal = true }
                     .padding(vertical = 14.dp),
                 contentAlignment = Alignment.Center
             ) {
                 Text("Start 1-Hour Free Trial", color = purpleLight,
                     fontSize = 13.sp, fontWeight = FontWeight.ExtraBold, letterSpacing = 1.5.sp)
             }
         }
         Spacer(Modifier.height(24.dp))

         val greenAccent = Color(0xFF00C864)
         Column(
             modifier = Modifier
                 .fillMaxWidth()
                 .clip(RoundedCornerShape(20.dp))
                 .background(
                     Brush.linearGradient(listOf(Color(0xF8051915), Color(0xF2051410)))
                 )
                 .border(1.dp, Color(0x4000C864), RoundedCornerShape(20.dp))
                 .padding(start = 16.dp, top = 16.dp, end = 16.dp, bottom = 22.dp)
         ) {

             Row(
                 verticalAlignment = Alignment.CenterVertically,
                 horizontalArrangement = Arrangement.spacedBy(12.dp),
                 modifier = Modifier.padding(bottom = 12.dp)
             ) {
                 Box(
                     modifier = Modifier
                         .size(36.dp)
                         .clip(RoundedCornerShape(10.dp))
                         .background(Color(0x3300C864))
                         .border(1.dp, Color(0x5900C864), RoundedCornerShape(10.dp)),
                     contentAlignment = Alignment.Center
                 ) {
                     Canvas(Modifier.size(18.dp)) {

                         val W = size.width; val H = size.height
                         val sp = Path().apply {
                             moveTo(W*0.5f, H*0.05f)
                             lineTo(W*0.94f, H*0.22f)
                             lineTo(W*0.94f, H*0.56f)
                             cubicTo(W*0.94f, H*0.79f, W*0.74f, H*0.94f, W*0.5f, H*0.99f)
                             cubicTo(W*0.26f, H*0.94f, W*0.06f, H*0.79f, W*0.06f, H*0.56f)
                             lineTo(W*0.06f, H*0.22f)
                             close()
                         }
                         drawPath(sp, greenAccent.copy(alpha = 0.1f))
                         drawPath(sp, greenAccent, style = androidx.compose.ui.graphics.drawscope.Stroke(width = 2.5f * density))

                         drawLine(greenAccent, Offset(W*0.32f, H*0.52f), Offset(W*0.47f, H*0.67f), 3f * density, cap = androidx.compose.ui.graphics.StrokeCap.Round)
                         drawLine(greenAccent, Offset(W*0.47f, H*0.67f), Offset(W*0.70f, H*0.38f), 3f * density, cap = androidx.compose.ui.graphics.StrokeCap.Round)
                     }
                 }
                 Column(Modifier.weight(1f)) {
                     Text("App Integrity Verified", color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.Bold, letterSpacing = 0.5.sp)
                     Text("● Untampered", color = greenAccent, fontSize = 10.sp, fontWeight = FontWeight.Bold, letterSpacing = 1.5.sp)
                 }
                 Box(
                     modifier = Modifier
                         .clip(CircleShape)
                         .background(Color(0x1E00C864))
                         .border(1.dp, Color(0x4D00C864), CircleShape)
                         .padding(horizontal = 10.dp, vertical = 4.dp)
                 ) {
                     Text("SECURE", color = greenAccent, fontSize = 9.sp, fontWeight = FontWeight.ExtraBold, letterSpacing = 1.sp)
                 }
             }
             Box(Modifier.fillMaxWidth().height(1.dp).background(Color(0x1A00C864)))
             Spacer(Modifier.height(12.dp))
             listOf(
                 "Digital signature matches official release",
                 "No unauthorized modifications detected",
                 "Checksum verified against official build"
             ).forEach { text ->
                 Row(
                     verticalAlignment = Alignment.CenterVertically,
                     horizontalArrangement = Arrangement.spacedBy(12.dp),
                     modifier = Modifier.padding(vertical = 6.dp)
                 ) {
                     CheckBullet(greenAccent)
                     Text(text, color = Color(0xB3FFFFFF), fontSize = 12.sp)
                 }
             }
         }
         Spacer(Modifier.height(28.dp))

         Row(
             verticalAlignment = Alignment.CenterVertically,
             horizontalArrangement = Arrangement.spacedBy(12.dp),
             modifier = Modifier.fillMaxWidth()
         ) {
             Box(Modifier.weight(1f).height(1.dp).background(Color(0x0DFFFFFF)))
             Text("Official Links", color = Color(0x33FFFFFF), fontSize = 10.sp,
                 fontWeight = FontWeight.Bold, letterSpacing = 2.sp)
             Box(Modifier.weight(1f).height(1.dp).background(Color(0x0DFFFFFF)))
         }
         Spacer(Modifier.height(20.dp))

         Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
             links.forEach { link ->
                 Row(
                     modifier = Modifier
                         .fillMaxWidth()
                         .clip(RoundedCornerShape(20.dp))
                         .background(
                             Brush.linearGradient(listOf(Color(0xF918202A), Color(0xF0121620)))
                         )
                         .border(1.dp,
                             link.accentColor.copy(alpha = 0.2f),
                             RoundedCornerShape(20.dp))
                         .clickable {
                             try {
                                 context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(link.url)))
                             } catch (_: Exception) {
                                 Toast.makeText(context, "Could not open Telegram", Toast.LENGTH_SHORT).show()
                             }
                         }
                         .padding(horizontal = 20.dp, vertical = 20.dp),
                     verticalAlignment = Alignment.CenterVertically,
                     horizontalArrangement = Arrangement.spacedBy(20.dp)
                 ) {

                     Box(
                         modifier = Modifier
                             .size(56.dp)
                             .clip(RoundedCornerShape(16.dp))
                             .background(
                                 Brush.linearGradient(
                                     listOf(link.accentColor.copy(0.12f), link.accentColor.copy(0.05f))
                                 )
                             )
                             .border(1.dp, link.accentColor.copy(alpha = 0.25f), RoundedCornerShape(16.dp)),
                         contentAlignment = Alignment.Center
                     ) {
                         TerminalIcon(link.accentColor, Modifier.size(28.dp))
                     }

                     Column(Modifier.weight(1f)) {
                         Text("${link.emoji} ${link.title}",
                             color = Color.White, fontSize = 15.sp, fontWeight = FontWeight.Bold, letterSpacing = 0.5.sp)
                         Spacer(Modifier.height(6.dp))
                         Box(
                             modifier = Modifier
                                 .clip(CircleShape)
                                 .background(link.accentColor.copy(alpha = 0.083f))
                                 .border(1.dp, link.accentColor.copy(alpha = 0.21f), CircleShape)
                                 .padding(horizontal = 10.dp, vertical = 4.dp)
                         ) {
                             Text(link.tag, color = link.accentColor,
                                 fontSize = 10.sp, fontWeight = FontWeight.ExtraBold, letterSpacing = 1.5.sp)
                         }
                     }

                     Box(
                         modifier = Modifier
                             .size(36.dp)
                             .clip(RoundedCornerShape(10.dp))
                             .background(
                                 Brush.linearGradient(
                                     listOf(link.accentColor.copy(0.14f), link.accentColor.copy(0.06f))
                                 )
                             )
                             .border(1.dp, link.accentColor.copy(alpha = 0.25f), RoundedCornerShape(10.dp)),
                         contentAlignment = Alignment.Center
                     ) {
                         Canvas(Modifier.size(16.dp)) {
                             val mid = size.height / 2f
                             val col = link.accentColor
                             drawLine(col, Offset(size.width * 0.3f, mid * 0.38f),
                                 Offset(size.width * 0.72f, mid), 3f,
                                 cap = androidx.compose.ui.graphics.StrokeCap.Round)
                             drawLine(col, Offset(size.width * 0.72f, mid),
                                 Offset(size.width * 0.3f, mid * 1.62f), 3f,
                                 cap = androidx.compose.ui.graphics.StrokeCap.Round)
                         }
                     }
                 }
             }
         }
         Spacer(Modifier.height(32.dp))
         Text(
             "Always verify sources before downloading",
             color = Color(0x26FFFFFF), fontSize = 10.sp,
             letterSpacing = 2.sp, fontWeight = FontWeight.Bold,
             textAlign = TextAlign.Center,
             modifier = Modifier.fillMaxWidth()
         )
     }

     if (showTrialModal) {
         Box(
             modifier = Modifier
                 .fillMaxSize()
                 .background(Color(0xBF000000))
                 .clickable { showTrialModal = false },
             contentAlignment = Alignment.Center
         ) {
             Column(
                 modifier = Modifier
                     .fillMaxWidth(0.9f)
                     .clip(RoundedCornerShape(20.dp))
                     .background(
                         Brush.linearGradient(listOf(Color(0xFC140C28), Color(0xFA0C081C)))
                     )
                     .border(1.dp, Color(0x59A050FF), RoundedCornerShape(20.dp))
                     .clickable {}
                     .padding(24.dp),
                 horizontalAlignment = Alignment.CenterHorizontally,
                 verticalArrangement = Arrangement.spacedBy(16.dp)
             ) {

                 Box(
                     modifier = Modifier
                         .size(56.dp)
                         .clip(RoundedCornerShape(16.dp))
                         .background(
                             Brush.linearGradient(listOf(Color(0x40A050FF), Color(0x14A050FF)))
                         )
                         .border(1.dp, Color(0x66A050FF), RoundedCornerShape(16.dp)),
                     contentAlignment = Alignment.Center
                 ) {
                     Canvas(Modifier.size(28.dp)) {
                         val c = Color(0xFFA050FF)
                         drawCircle(c, radius = size.minDimension / 2f - 2f * density,
                             style = androidx.compose.ui.graphics.drawscope.Stroke(width = 2.5f * density))
                         drawLine(c, Offset(size.width/2f, size.height*0.28f),
                             Offset(size.width/2f, size.height*0.55f), 3f * density,
                             cap = androidx.compose.ui.graphics.StrokeCap.Round)
                         drawLine(c, Offset(size.width/2f, size.height*0.55f),
                             Offset(size.width*0.68f, size.height*0.7f), 3f * density,
                             cap = androidx.compose.ui.graphics.StrokeCap.Round)
                     }
                 }
                 Column(horizontalAlignment = Alignment.CenterHorizontally) {
                     Text("Your Trial Key", color = Color.White, fontSize = 18.sp, fontWeight = FontWeight.Bold, letterSpacing = 0.5.sp)
                     Spacer(Modifier.height(4.dp))
                     Text("Use this key to activate your 1-hour free trial",
                         color = Color(0x80FFFFFF), fontSize = 12.sp, textAlign = TextAlign.Center)
                 }

                 Row(
                     modifier = Modifier
                         .fillMaxWidth()
                         .clip(RoundedCornerShape(12.dp))
                         .background(Color(0x14A050FF))
                         .border(1.dp, Color(0x4DA050FF), RoundedCornerShape(12.dp))
                         .padding(horizontal = 20.dp, vertical = 16.dp),
                     verticalAlignment = Alignment.CenterVertically,
                     horizontalArrangement = Arrangement.SpaceBetween
                 ) {
                     Text("NOWORNEVER",
                         color = Color(0xFFC080FF), fontSize = 21.sp, fontWeight = FontWeight.ExtraBold,
                         letterSpacing = 6.sp)
                     Box(
                         modifier = Modifier
                             .clip(RoundedCornerShape(8.dp))
                             .clickable { copyKey() }
                             .padding(4.dp)
                     ) {
                         if (copied) {
                             Canvas(Modifier.size(18.dp)) {
                                 drawLine(Color(0xFF00C864), Offset(size.width*0.15f, size.height*0.5f),
                                     Offset(size.width*0.42f, size.height*0.78f), 3f,
                                     cap = androidx.compose.ui.graphics.StrokeCap.Round)
                                 drawLine(Color(0xFF00C864), Offset(size.width*0.42f, size.height*0.78f),
                                     Offset(size.width*0.88f, size.height*0.22f), 3f,
                                     cap = androidx.compose.ui.graphics.StrokeCap.Round)
                             }
                         } else {

                             Canvas(Modifier.size(18.dp)) {
                                 val c = Color(0xFFA050FF)
                                 drawRoundRect(c, topLeft = Offset(size.width*0.3f, size.height*0.1f),
                                     size = androidx.compose.ui.geometry.Size(size.width*0.62f, size.height*0.62f),
                                     cornerRadius = androidx.compose.ui.geometry.CornerRadius(size.width*0.1f),
                                     style = androidx.compose.ui.graphics.drawscope.Stroke(width = 2.5f))
                                 drawRoundRect(c, topLeft = Offset(size.width*0.08f, size.height*0.28f),
                                     size = androidx.compose.ui.geometry.Size(size.width*0.62f, size.height*0.62f),
                                     cornerRadius = androidx.compose.ui.geometry.CornerRadius(size.width*0.1f),
                                     style = androidx.compose.ui.graphics.drawscope.Stroke(width = 2.5f))
                             }
                         }
                     }
                 }
                 if (copied) {
                     Text("✓ Copied to clipboard",
                         color = Color(0xFF00C864), fontSize = 11.sp,
                         letterSpacing = 3.sp, fontWeight = FontWeight.Bold)
                 }

                 Box(
                     modifier = Modifier
                         .fillMaxWidth()
                         .clip(RoundedCornerShape(12.dp))
                         .background(
                             Brush.linearGradient(listOf(Color(0x40A050FF), Color(0x26783214)))
                         )
                         .border(1.dp, Color(0x59A050FF), RoundedCornerShape(12.dp))
                         .clickable { showTrialModal = false }
                         .padding(vertical = 14.dp),
                     contentAlignment = Alignment.Center
                 ) {
                     Text("Got it", color = Color(0xFFC080FF),
                         fontSize = 13.sp, fontWeight = FontWeight.ExtraBold, letterSpacing = 2.sp)
                 }
             }
         }
     }
 }
}

@Composable
private fun ModuleStatusCard(
    ipAddress: String,
    mediaReady: Boolean,
    frameCount: Int,
    moduleStatus: ModuleManager.ModuleStatus,
    isInjectingNow: Boolean = false,
    injectResultMsg: String? = null,
    onInjectClick: () -> Unit
) {
    Column(
        modifier = Modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {

        Box(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(20.dp))
                .background(Color(0xFF0F1624))
                .border(1.dp, Border, RoundedCornerShape(20.dp))
                .padding(horizontal = 16.dp, vertical = 14.dp)
        ) {
            Column {

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(10.dp)
                    ) {
                        val statusColor = when {
                            !moduleStatus.installed -> InactiveGray
                            moduleStatus.safetyState == ModuleManager.SafetyState.ACTIVE -> GreenOk
                            moduleStatus.safetyState == ModuleManager.SafetyState.DISABLED_CRASH_LOOP -> RedHook
                            moduleStatus.safetyState == ModuleManager.SafetyState.CRASH_RETRY -> OrangeWait
                            moduleStatus.safetyState == ModuleManager.SafetyState.DISABLED_MANUAL -> OrangeText
                            else -> InactiveGray
                        }
                        val pulse = rememberInfiniteTransition(label = "modDot")
                        val dotAlpha by pulse.animateFloat(
                            initialValue = 1f,
                            targetValue = if (moduleStatus.active) 0.3f else 0.6f,
                            animationSpec = infiniteRepeatable(tween(900), RepeatMode.Reverse),
                            label = "modDotAlpha"
                        )
                        Box(
                            modifier = Modifier
                                .size(10.dp)
                                .background(
                                    color = statusColor.copy(alpha = if (moduleStatus.active) dotAlpha else 1f),
                                    shape = CircleShape
                                )
                        )
                        Column {
                            Text(
                                "MODULE STATUS",
                                color = Color(0xFF888888),
                                fontSize = 10.sp,
                                fontWeight = FontWeight.Bold,
                                letterSpacing = 1.5.sp
                            )
                            Text(
                                when {
                                    !moduleStatus.installed -> "Not Installed"
                                    moduleStatus.safetyState == ModuleManager.SafetyState.ACTIVE -> "Active - Hook Running"
                                    moduleStatus.safetyState == ModuleManager.SafetyState.DISABLED_CRASH_LOOP -> "Crashed - Auto-Disabled"
                                    moduleStatus.safetyState == ModuleManager.SafetyState.CRASH_RETRY -> "Retrying After Crash"
                                    moduleStatus.safetyState == ModuleManager.SafetyState.DISABLED_MANUAL -> "Disabled by User"
                                    else -> "Unknown"
                                },
                                color = statusColor,
                                fontSize = 12.sp,
                                fontWeight = FontWeight.SemiBold
                            )
                        }
                    }

                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp)) {



                        val alreadyHooked = moduleStatus.hookMapsLine.isNotEmpty()
                        val injectColor = when {
                            isInjectingNow -> OrangeWait
                            alreadyHooked  -> GreenOk
                            else           -> Color(0xFF5588FF)
                        }
                        Box(
                            modifier = Modifier
                                .clip(RoundedCornerShape(8.dp))
                                .background(injectColor.copy(alpha = 0.15f))
                                .border(1.dp, injectColor.copy(alpha = 0.4f), RoundedCornerShape(8.dp))
                                .clickable(enabled = !isInjectingNow && !alreadyHooked) { onInjectClick() }
                                .padding(horizontal = 10.dp, vertical = 6.dp),
                            contentAlignment = Alignment.Center
                        ) {
                            if (isInjectingNow) {
                                val spin = rememberInfiniteTransition(label = "injBtnSpin")
                                val a by spin.animateFloat(
                                    initialValue = 0f, targetValue = 360f,
                                    animationSpec = infiniteRepeatable(tween(800, easing = LinearEasing)),
                                    label = "injBtnAngle"
                                )
                                Canvas(Modifier.size(13.dp).rotate(a)) {
                                    drawArc(OrangeWait, 0f, 270f, false,
                                        style = androidx.compose.ui.graphics.drawscope.Stroke(width = 2.5f),
                                        topLeft = Offset(2f, 2f),
                                        size = androidx.compose.ui.geometry.Size(size.width - 4f, size.height - 4f))
                                }
                            } else {
                                Text(
                                    if (alreadyHooked) "ACTIVE" else "INJECT",
                                    color = injectColor,
                                    fontSize = 9.sp,
                                    fontWeight = FontWeight.Bold,
                                    letterSpacing = 1.sp
                                )
                            }
                        }
                    }
                }

                Spacer(Modifier.height(10.dp))
                Box(Modifier.fillMaxWidth().height(1.dp).background(Color(0x14FFFFFF)))
                Spacer(Modifier.height(10.dp))


                val infoRows = buildList {
                    add("Manager" to (moduleStatus.manager ?: "None"))
                    add("Version" to (moduleStatus.moduleVersion.ifEmpty { "-" }))
                    add("Cameraserver" to moduleStatus.cameraserverPid)
                    if (moduleStatus.crashCount > 0) add("Crashes" to "${moduleStatus.crashCount}")
                    if (moduleStatus.hookMapsLine.isNotEmpty()) {
                        add("Hook Map" to moduleStatus.hookMapsLine.substringBefore(" ").take(20))
                    }
                }

                infoRows.forEach { (label, value) ->
                    Row(
                        modifier = Modifier.fillMaxWidth().padding(vertical = 1.dp),
                        horizontalArrangement = Arrangement.SpaceBetween
                    ) {
                        Text(
                            "$label:",
                            color = Color(0xFF5A6474),
                            fontSize = 10.sp,
                            fontFamily = FontFamily.Monospace
                        )
                        Text(
                            value,
                            color = Color(0xFFAAAAAA),
                            fontSize = 10.sp,
                            fontFamily = FontFamily.Monospace
                        )
                    }
                }


                  if (injectResultMsg != null) {
                      Spacer(Modifier.height(8.dp))
                      val isSuccess = injectResultMsg.startsWith("Injected")
                      val resultColor = if (isSuccess) GreenOk else RedHook
                      Box(
                          modifier = Modifier
                              .fillMaxWidth()
                              .clip(RoundedCornerShape(8.dp))
                              .background(resultColor.copy(alpha = 0.1f))
                              .border(1.dp, resultColor.copy(alpha = 0.35f), RoundedCornerShape(8.dp))
                              .padding(8.dp)
                      ) {
                          Text(
                              text = (if (isSuccess) "✓ " else "✗ ") + injectResultMsg,
                              color = resultColor,
                              fontSize = 9.sp,
                              fontFamily = FontFamily.Monospace,
                              lineHeight = 12.sp
                          )
                      }
                  }


                if (moduleStatus.logTail.isNotEmpty() && moduleStatus.logTail != "No logs yet") {
                    Spacer(Modifier.height(8.dp))
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(8.dp))
                            .background(Color(0xFF0A0A14))
                            .border(1.dp, Color(0x1AFFFFFF), RoundedCornerShape(8.dp))
                            .padding(8.dp)
                    ) {
                        Text(
                            moduleStatus.logTail.lines().take(4).joinToString("\n"),
                            color = Color(0xFF00FF00).copy(alpha = 0.7f),
                            fontSize = 9.sp,
                            fontFamily = FontFamily.Monospace,
                            lineHeight = 12.sp
                        )
                    }
                }


                if (moduleStatus.safetyState == ModuleManager.SafetyState.DISABLED_CRASH_LOOP) {
                    Spacer(Modifier.height(8.dp))
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(8.dp))
                            .background(RedHook.copy(alpha = 0.1f))
                            .border(1.dp, RedHook.copy(alpha = 0.3f), RoundedCornerShape(8.dp))
                            .padding(8.dp)
                    ) {
                        Row(verticalAlignment = Alignment.CenterVertically) {
                            Text("! ", fontSize = 12.sp, color = RedHook)
                            Text(
                                "Module auto-disabled after ${moduleStatus.crashCount} crashes. Tap ENABLE to retry.",
                                color = RedHook.copy(alpha = 0.9f),
                                fontSize = 10.sp
                            )
                        }
                    }
                }
            }
        }


        Box(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(20.dp))
                .background(Color(0xFF0F1624))
                .border(1.dp, Border, RoundedCornerShape(20.dp))
                .padding(horizontal = 20.dp, vertical = 18.dp)
        ) {
            Column {
                Text(
                    text = ipAddress,
                    color = Cyan,
                    fontSize = 26.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace,
                    modifier = Modifier.fillMaxWidth(),
                    textAlign = TextAlign.Center
                )
                Spacer(Modifier.height(16.dp))
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.SpaceBetween
                ) {
                    Column {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(8.dp)
                        ) {
                            val pulse = rememberInfiniteTransition(label = "dot")
                            val dotAlpha by pulse.animateFloat(
                                initialValue = 1f,
                                targetValue = 0.3f,
                                animationSpec = infiniteRepeatable(tween(900), RepeatMode.Reverse),
                                label = "dotAlpha"
                            )
                            Box(
                                modifier = Modifier
                                    .size(10.dp)
                                    .background(
                                        color = if (mediaReady) GreenOk.copy(alpha = dotAlpha) else OrangeWait.copy(alpha = dotAlpha),
                                        shape = CircleShape
                                    )
                            )
                            Text(
                                text = if (mediaReady) "MEDIA READY" else "WAITING",
                                color = if (mediaReady) GreenOk else OrangeWait,
                                fontSize = 12.sp,
                                fontWeight = FontWeight.Bold,
                                letterSpacing = 1.sp
                            )
                        }
                        Spacer(Modifier.height(4.dp))
                        Text(
                            text = if (mediaReady) "Stream available for hook" else "No media stream detected",
                            color = TextMid,
                            fontSize = 11.sp
                        )
                    }
                    if (mediaReady) {
                        Column(horizontalAlignment = Alignment.End) {
                            Text(
                                text = "$frameCount",
                                color = GreenOk,
                                fontSize = 28.sp,
                                fontWeight = FontWeight.Bold,
                                fontFamily = FontFamily.Monospace
                            )
                            Text(
                                text = "FRAMES",
                                color = GreenOk.copy(alpha = 0.7f),
                                fontSize = 10.sp,
                                fontWeight = FontWeight.Bold,
                                letterSpacing = 1.sp
                            )
                        }
                    }
                }
            }
        }
    }
}


@Composable
fun AppIconCircle(app: AppInfo, size: Dp, cornerRadius: Dp = 14.dp) {
val icon = app.icon
if (icon != null) {
val bitmap = remember(app.packageName) { drawableToBitmap(icon) }
if (bitmap != null) {
Image(
bitmap = bitmap.asImageBitmap(),
contentDescription = app.appName,
modifier = Modifier.size(size).clip(RoundedCornerShape(cornerRadius))
)
return
}
}
val fallbackColor = Color(hashColor(app.packageName))
Box(
modifier = Modifier
.size(size)
.clip(RoundedCornerShape(cornerRadius))
.background(fallbackColor),
contentAlignment = Alignment.Center
) {
Text(app.initials, color = Color.White, fontWeight = FontWeight.Bold,
fontSize = (size.value * 0.33f).sp)
}
}
fun drawableToBitmap(drawable: Drawable): Bitmap? {
return try {
val w = drawable.intrinsicWidth.takeIf { it > 0 } ?: 48
val h = drawable.intrinsicHeight.takeIf { it > 0 } ?: 48
val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
val canvas = android.graphics.Canvas(bmp)
drawable.setBounds(0, 0, canvas.width, canvas.height)
drawable.draw(canvas)
bmp
} catch (_: Exception) { null }
}
private fun hashColor(pkg: String): Int {
val colors = intArrayOf(
0xFF6C63FF.toInt(), 0xFFFF4D9D.toInt(), 0xFF4ADE80.toInt(),
0xFF2CA5E0.toInt(), 0xFFFF0000.toInt(), 0xFF25D366.toInt(),
0xFF1877F2.toInt(), 0xFF5865F2.toInt(), 0xFFFC00.toInt(),
0xFF010101.toInt(), 0xFF2D8CFF.toInt(), 0xFF00897B.toInt()
)
return colors[Math.abs(pkg.hashCode()) % colors.size]
}

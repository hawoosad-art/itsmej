package com.itsme.amkush.ui
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.animation.*
import androidx.compose.animation.core.*
import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.*
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.*
import androidx.compose.ui.draw.*
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.*
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.*
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.*
import com.itsme.amkush.security.LicenseGuard
import com.itsme.amkush.ui.HomeViewModel
import com.itsme.amkush.utils.DeviceUtils
import com.itsme.amkush.utils.Logger
import com.itsme.amkush.utils.SharedPrefs
import kotlinx.coroutines.*

private val BgDark  = Color(0xFF060D1F)
private val BgMid   = Color(0xFF0B1535)
private val Surface = Color(0x08FFFFFF)
private val Border  = Color(0x14FFFFFF)
private val BluePrimary = Color(0xFF3B82F6)
private val BlueSecondary = Color(0xFF6366F1)
private val CyanAccent = Color(0xFF38BDF8)
private val YellowWarn = Color(0xFFFBBF24)
private val GreenSuccess = Color(0xFF22C55E)
private val RedError = Color(0xFFEF4444)
private val TextPrimary = Color(0xFFE2E8F0)
private val TextSecondary = Color(0xFF94A3B8)
private val TextMuted = Color(0xFF475569)

class PaymentScreen : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val targetPackage = intent.getStringExtra("target_package")
        val targetAppName = intent.getStringExtra("target_app_name")
        SharedPrefs.init(this)

        // Re-verify against the server before trusting the local activation, so a
        // key deleted on the admin panel deactivates immediately instead of the
        // local fg_lic.bin keeping the app "active" forever. Falls back to the
        // local check only when offline.
        val token = SharedPrefs.getActivationToken()
        if (!token.isNullOrEmpty()) {
            CoroutineScope(Dispatchers.IO).launch {
                val deviceId = DeviceUtils.getDeviceId(this@PaymentScreen)
                val result = LicenseGuard.verifyToken(token, deviceId)
                runOnUiThread {
                    if (result.valid) {
                        val expiryMs = HomeViewModel.parseExpiryMs(result.expiresAt)
                        LicenseGuard.nativeSaveActivation(this@PaymentScreen, token, result.isTrial, expiryMs)
                        proceedToDashboard(targetPackage, targetAppName)
                    } else {
                        SharedPrefs.clearActivation()
                        LicenseGuard.nativeClearActivation(this@PaymentScreen)
                        showPayment(targetPackage, targetAppName)
                    }
                }
            }
        } else {
            showPayment(targetPackage, targetAppName)
        }
    }

    private fun showPayment(targetPackage: String?, targetAppName: String?) {
        setContent {
            PaymentContent(
                targetPackage = targetPackage,
                targetAppName = targetAppName,
                onBack = { finish() },
                onSuccess = { proceedToDashboard(targetPackage, targetAppName) }
            )
        }
    }

    private fun proceedToDashboard(pkg: String?, name: String?) {
        startActivity(Intent(this, TabsScreen::class.java).apply {
            putExtra("target_package", pkg)
            putExtra("target_app_name", name)
            putExtra("from_payment", true)
        })
        finish()
    }
}

@Composable
private fun PaymentContent(
    targetPackage: String?,
    targetAppName: String?,
    onBack: () -> Unit,
    onSuccess: () -> Unit
) {
    val context = LocalContext.current
    val deviceId = remember { DeviceUtils.getFormattedDeviceId(context) }
    var key by remember { mutableStateOf("") }
    var showKey by remember { mutableStateOf(false) }
    var loading by remember { mutableStateOf(false) }
    var errorMsg by remember { mutableStateOf("") }
    var activated by remember { mutableStateOf(false) }

    val appIconBitmap: Bitmap? = remember(targetPackage) {
        targetPackage?.let { pkg ->
            try {
                val drawable = context.packageManager.getApplicationIcon(pkg)
                val w = drawable.intrinsicWidth.takeIf { it > 0 } ?: 48
                val h = drawable.intrinsicHeight.takeIf { it > 0 } ?: 48
                val bmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
                val canvas = android.graphics.Canvas(bmp)
                drawable.setBounds(0, 0, canvas.width, canvas.height)
                drawable.draw(canvas)
                bmp
            } catch (_: Exception) { null }
        }
    }

    LaunchedEffect(Unit) {
        SharedPrefs.init(context)
        SharedPrefs.setDeviceId(DeviceUtils.getFormattedDeviceId(context))
    }

    fun copyDeviceId() {
        val cm = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
        cm.setPrimaryClip(ClipData.newPlainText("Device ID", deviceId))
        Toast.makeText(context, "Device ID copied!", Toast.LENGTH_SHORT).show()
    }

    fun openBot() {
        try {
            val url = LicenseGuard.nativeGetTgBot()
            if (url.isEmpty()) {
                Toast.makeText(context, "Could not resolve bot link", Toast.LENGTH_SHORT).show()
                return
            }
            context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
        } catch (e: Exception) {
            Toast.makeText(context, "Could not open Telegram", Toast.LENGTH_SHORT).show()
        }
    }

    fun activate() {
        if (key.trim().isEmpty()) {
            errorMsg = "Please enter an activation key"
            return
        }
        errorMsg = ""
        loading = true

        CoroutineScope(Dispatchers.IO).launch {
            try {
                val rawDeviceId = DeviceUtils.getDeviceId(context)
                val wifiIp = DeviceUtils.getWifiIpAddress(context)
                val result = LicenseGuard.validateKey(key.trim(), rawDeviceId, wifiIp)

                withContext(Dispatchers.Main) {
                    loading = false
                    if (result.success && result.token != null) {
                        SharedPrefs.setActivationToken(result.token)
                        SharedPrefs.setTrial(result.isTrial)
                        SharedPrefs.setPaid(!result.isTrial)
                        val expiryMs = HomeViewModel.parseExpiryMs(result.expiresAt)
                        if (result.isTrial) {
                            SharedPrefs.setTrialWifiIp(wifiIp)
                            if (expiryMs > 0) SharedPrefs.setTrialExpiry(expiryMs)
                        }
                        LicenseGuard.nativeSaveActivation(context, result.token, result.isTrial, expiryMs)
                        Toast.makeText(
                            context,
                            if (result.isTrial) "Trial activated!" else "Activation successful!",
                            Toast.LENGTH_LONG
                        ).show()
                        activated = true
                        onSuccess()
                    } else {
                        errorMsg = result.message.ifEmpty { "Invalid activation key" }
                    }
                }
            } catch (e: Exception) {
                withContext(Dispatchers.Main) {
                    loading = false
                    errorMsg = "Network error: ${e.message}"
                }
            }
        }
    }

    if (activated) {
        SuccessScreen(onBack = { activated = false })
    } else {
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(
                    Brush.verticalGradient(
                        listOf(BgDark, BgMid, BgDark)
                    )
                )
        ) {
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .systemBarsPadding()
                    .verticalScroll(rememberScrollState())
                    .padding(horizontal = 16.dp, vertical = 40.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                Spacer(Modifier.height(16.dp))

                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                    modifier = Modifier.padding(bottom = 8.dp)
                ) {
                    Box(
                        modifier = Modifier
                            .size(36.dp)
                            .clip(RoundedCornerShape(10.dp))
                            .background(Color(0x0FFFFFFF))
                            .border(1.dp, Color(0x1AFFFFFF), RoundedCornerShape(10.dp))
                            .clickable { onBack() },
                        contentAlignment = Alignment.Center
                    ) {
                        Text("‹", color = TextSecondary, fontSize = 20.sp, fontWeight = FontWeight.Bold)
                    }
                    Column {
                        Text("Activation", color = TextPrimary, fontSize = 16.sp, fontWeight = FontWeight.Bold)
                        Text("Enter your license key below", color = TextMuted, fontSize = 11.sp)
                    }
                }

                if (!targetAppName.isNullOrEmpty()) {
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clip(RoundedCornerShape(16.dp))
                            .background(Surface)
                            .border(1.dp, Border, RoundedCornerShape(16.dp))
                            .padding(16.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        if (appIconBitmap != null) {
                            Image(
                                bitmap = appIconBitmap.asImageBitmap(),
                                contentDescription = targetAppName,
                                modifier = Modifier
                                    .size(44.dp)
                                    .clip(RoundedCornerShape(12.dp))
                            )
                        } else {
                            Box(
                                modifier = Modifier
                                    .size(44.dp)
                                    .clip(RoundedCornerShape(12.dp))
                                    .background(
                                        Brush.linearGradient(listOf(BluePrimary, BlueSecondary))
                                    ),
                                contentAlignment = Alignment.Center
                            ) {
                                Text(
                                    targetAppName.take(2).uppercase(),
                                    color = Color.White,
                                    fontWeight = FontWeight.Bold,
                                    fontSize = 14.sp
                                )
                            }
                        }
                        Column(Modifier.weight(1f)) {
                            Text(
                                targetAppName,
                                color = TextPrimary,
                                fontSize = 14.sp,
                                fontWeight = FontWeight.SemiBold,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis
                            )
                            if (!targetPackage.isNullOrEmpty()) {
                                Text(
                                    targetPackage,
                                    color = TextMuted,
                                    fontSize = 10.sp,
                                    fontFamily = FontFamily.Monospace,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis
                                )
                            }
                        }
                        Box(
                            modifier = Modifier
                                .clip(RoundedCornerShape(8.dp))
                                .background(Color(0x1A3B82F6))
                                .border(1.dp, Color(0x333B82F6), RoundedCornerShape(8.dp))
                                .padding(horizontal = 8.dp, vertical = 4.dp)
                        ) {
                            Row(
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(4.dp)
                            ) {
                                Box(
                                    modifier = Modifier
                                        .size(6.dp)
                                        .background(BluePrimary, CircleShape)
                                )
                                Text("App", color = Color(0xFF60A5FA), fontSize = 10.sp, fontWeight = FontWeight.Medium)
                            }
                        }
                    }
                }

                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(16.dp))
                        .background(Surface)
                        .border(1.dp, Border, RoundedCornerShape(16.dp))
                        .padding(16.dp)
                ) {
                    Column {
                        Text(
                            "DEVICE ID",
                            color = TextMuted,
                            fontSize = 9.sp,
                            fontWeight = FontWeight.Medium,
                            letterSpacing = 1.sp
                        )
                        Spacer(Modifier.height(8.dp))
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(12.dp)
                        ) {
                            Text(
                                deviceId,
                                color = TextPrimary,
                                fontSize = 14.sp,
                                fontFamily = FontFamily.Monospace,
                                fontWeight = FontWeight.Bold,
                                modifier = Modifier.weight(1f)
                            )
                            Box(
                                modifier = Modifier
                                    .clip(RoundedCornerShape(10.dp))
                                    .background(
                                        Brush.linearGradient(listOf(BluePrimary, BlueSecondary))
                                    )
                                    .clickable { copyDeviceId() }
                                    .padding(horizontal = 16.dp, vertical = 8.dp)
                            ) {
                                Text("Copy", color = Color.White, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
                            }
                        }
                    }
                }

                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(16.dp))
                        .background(Color(0x0AFBBF24))
                        .border(1.dp, Color(0x1FFBBF24), RoundedCornerShape(16.dp))
                        .padding(16.dp)
                ) {
                    Column {
                        Text(
                            "HOW TO GET A KEY",
                            color = YellowWarn,
                            fontSize = 10.sp,
                            fontWeight = FontWeight.Bold,
                            letterSpacing = 1.sp
                        )
                        Spacer(Modifier.height(12.dp))
                        Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                            StepsItem(
                                number = 1,
                                text = "Open the Telegram bot and send your Device ID to get the payment details."
                            )
                            StepsItem(
                                number = 2,
                                text = "Send the exact crypto amount to one of the listed wallet addresses provided by the bot."
                            )
                            StepsItem(
                                number = 3,
                                text = "After payment, send the command below to the bot and wait for confirmation:",
                                code = "/activate {transaction_id}"
                            )
                            StepsItem(
                                number = 4,
                                text = "Once the bot confirms your payment, it will automatically send you your activation key."
                            )
                        }
                        Spacer(Modifier.height(12.dp))
                        Box(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(top = 12.dp)
                                .drawBehind {
                                    drawLine(
                                        color = Color(0x1AFBBF24),
                                        start = Offset(0f, 0f),
                                        end = Offset(size.width, 0f),
                                        strokeWidth = 1.dp.toPx()
                                    )
                                }
                                .padding(top = 12.dp)
                        ) {
                            Row(
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(8.dp)
                            ) {
                                Text("🎁", fontSize = 12.sp)
                                Text(
                                    "Want to try first? ",
                                    color = TextMuted,
                                    fontSize = 11.sp
                                )
                                Text(
                                    "Get trial from bot",
                                    color = Color(0xFF15803D),
                                    fontSize = 11.sp,
                                    fontFamily = FontFamily.Monospace,
                                    fontWeight = FontWeight.Bold,
                                    modifier = Modifier
                                        .clip(RoundedCornerShape(6.dp))
                                        .clickable { openBot() }
                                        .padding(horizontal = 2.dp)
                                )
                            }
                        }
                    }
                }

                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(16.dp))
                        .background(Color(0x1A2CA5E0))
                        .border(1.dp, Color(0x332CA5E0), RoundedCornerShape(16.dp))
                        .clickable { openBot() }
                        .padding(vertical = 14.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        "Open Telegram Bot",
                        color = CyanAccent,
                        fontSize = 14.sp,
                        fontWeight = FontWeight.SemiBold
                    )
                }

                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(16.dp))
                        .background(Surface)
                        .border(
                            1.dp,
                            if (errorMsg.isNotEmpty()) Color(0x66EF4444) else Border,
                            RoundedCornerShape(16.dp)
                        )
                ) {
                    Column {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(12.dp),
                            modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp)
                        ) {
                            Text("🔑", fontSize = 16.sp)
                            BasicTextField(
                                value = key,
                                onValueChange = {
                                    key = it
                                    if (errorMsg.isNotEmpty()) errorMsg = ""
                                },
                                modifier = Modifier
                                    .weight(1f)
                                    .padding(vertical = 14.dp),
                                textStyle = androidx.compose.ui.text.TextStyle(
                                    color = TextPrimary,
                                    fontSize = 13.sp,
                                    fontFamily = FontFamily.Monospace
                                ),
                                singleLine = true,
                                visualTransformation = if (showKey) VisualTransformation.None else PasswordVisualTransformation(),
                                decorationBox = { innerTextField ->
                                    if (key.isEmpty()) {
                                        Text(
                                            "Enter activation key...",
                                            color = TextMuted,
                                            fontSize = 13.sp
                                        )
                                    }
                                    innerTextField()
                                },
                                cursorBrush = SolidColor(BluePrimary)
                            )
                            Box(
                                modifier = Modifier
                                    .size(32.dp)
                                    .clip(RoundedCornerShape(8.dp))
                                    .background(Color(0x0FFFFFFF))
                                    .clickable { showKey = !showKey },
                                contentAlignment = Alignment.Center
                            ) {
                                Text(if (showKey) "" else "🙈", fontSize = 14.sp)
                            }
                        }
                    }
                }

                if (errorMsg.isNotEmpty()) {
                    Row(
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp),
                        modifier = Modifier.padding(horizontal = 4.dp)
                    ) {
                        Box(
                            modifier = Modifier
                                .size(16.dp)
                                .background(Color(0x33EF4444), CircleShape),
                            contentAlignment = Alignment.Center
                        ) {
                            Text("!", color = RedError, fontSize = 11.sp, fontWeight = FontWeight.Bold)
                        }
                        Text(errorMsg, color = Color(0xFFF87171), fontSize = 11.sp)
                    }
                }

                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clip(RoundedCornerShape(16.dp))
                        .background(
                            Brush.linearGradient(listOf(BluePrimary, BlueSecondary))
                        )
                        .clickable(enabled = !loading) { activate() }
                        .padding(vertical = 16.dp),
                    contentAlignment = Alignment.Center
                ) {
                    if (loading) {
                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(8.dp)
                        ) {
                            CircularProgressIndicator(
                                color = Color.White,
                                modifier = Modifier.size(16.dp),
                                strokeWidth = 2.dp
                            )
                            Text("Validating…", color = Color.White, fontSize = 14.sp, fontWeight = FontWeight.SemiBold)
                        }
                    } else {
                        Text(
                            "Activate",
                            color = Color.White,
                            fontSize = 15.sp,
                            fontWeight = FontWeight.Bold
                        )
                    }
                }

                Spacer(Modifier.height(8.dp))
            }
        }
    }
}

@Composable
private fun StepsItem(number: Int, text: String, code: String? = null) {
    Row(
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalAlignment = Alignment.Top
    ) {
        Box(
            modifier = Modifier
                .size(20.dp)
                .clip(CircleShape)
                .background(Color(0x26FBBF24)),
            contentAlignment = Alignment.Center
        ) {
            Text(
                number.toString(),
                color = YellowWarn,
                fontSize = 10.sp,
                fontWeight = FontWeight.Bold
            )
        }
        Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Text(text, color = TextSecondary, fontSize = 11.sp, lineHeight = 16.sp)
            if (code != null) {
                Box(
                    modifier = Modifier
                        .clip(RoundedCornerShape(8.dp))
                        .background(Color(0x14FBBF24))
                        .border(1.dp, Color(0x33FBBF24), RoundedCornerShape(8.dp))
                        .padding(horizontal = 8.dp, vertical = 4.dp)
                ) {
                    Text(code, color = YellowWarn, fontSize = 10.sp, fontFamily = FontFamily.Monospace)
                }
            }
        }
    }
}

@Composable
private fun BotIcon() {
    val infiniteTransition = rememberInfiniteTransition(label = "bot")
    val blink by infiniteTransition.animateFloat(
        initialValue = 1f,
        targetValue = 0.1f,
        animationSpec = infiniteRepeatable(
            animation = keyframes {
                durationMillis = 3000
                1f at 0
                1f at 2700
                0.1f at 2850
                1f at 3000
            },
            repeatMode = RepeatMode.Restart
        ),
        label = "blink"
    )
    val antennaBob by infiniteTransition.animateFloat(
        initialValue = 0f,
        targetValue = -1f,
        animationSpec = infiniteRepeatable(
            animation = tween(2000, easing = EaseInOut),
            repeatMode = RepeatMode.Reverse
        ),
        label = "antenna"
    )
    val dotPulse by infiniteTransition.animateFloat(
        initialValue = 1.2f,
        targetValue = 1.6f,
        animationSpec = infiniteRepeatable(
            animation = tween(2000, easing = EaseInOut),
            repeatMode = RepeatMode.Reverse
        ),
        label = "dot"
    )

    Canvas(modifier = Modifier.size(24.dp)) {
        val centerX = size.width / 2f
        val centerY = size.height / 2f

        drawLine(
            color = CyanAccent,
            start = Offset(centerX, 9.dp.toPx()),
            end = Offset(centerX, 14.dp.toPx()),
            strokeWidth = 1.4.dp.toPx(),
            cap = androidx.compose.ui.graphics.StrokeCap.Round
        )

        drawCircle(
            color = CyanAccent,
            radius = dotPulse.dp.toPx(),
            center = Offset(centerX, 6.4.dp.toPx())
        )

        drawRoundRect(
            color = Color(0xFF0F172A),
            topLeft = Offset(9.dp.toPx(), 14.dp.toPx()),
            size = androidx.compose.ui.geometry.Size(30.dp.toPx(), 24.dp.toPx()),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(6.dp.toPx())
        )
        drawRoundRect(
            color = CyanAccent,
            topLeft = Offset(9.dp.toPx(), 14.dp.toPx()),
            size = androidx.compose.ui.geometry.Size(30.dp.toPx(), 24.dp.toPx()),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(6.dp.toPx()),
            style = androidx.compose.ui.graphics.drawscope.Stroke(width = 1.4.dp.toPx())
        )

        val leftEyeScale = if (centerX < size.width / 2f) blink else 1f
        val rightEyeScale = if (centerX > size.width / 2f) blink else 1f

        drawRoundRect(
            color = CyanAccent,
            topLeft = Offset(13.dp.toPx(), 22.dp.toPx()),
            size = androidx.compose.ui.geometry.Size(8.dp.toPx() * leftEyeScale, 8.dp.toPx() * leftEyeScale),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(2.4.dp.toPx())
        )

        drawRoundRect(
            color = CyanAccent,
            topLeft = Offset(27.dp.toPx(), 22.dp.toPx()),
            size = androidx.compose.ui.geometry.Size(8.dp.toPx() * rightEyeScale, 8.dp.toPx() * rightEyeScale),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(2.4.dp.toPx())
        )

        drawRoundRect(
            color = CyanAccent.copy(alpha = 0.5f),
            topLeft = Offset(15.dp.toPx(), 33.dp.toPx()),
            size = androidx.compose.ui.geometry.Size(4.dp.toPx(), 2.4.dp.toPx()),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(0.8.dp.toPx())
        )

        drawRoundRect(
            color = CyanAccent.copy(alpha = 0.9f),
            topLeft = Offset(22.dp.toPx(), 33.dp.toPx()),
            size = androidx.compose.ui.geometry.Size(4.dp.toPx(), 2.4.dp.toPx()),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(0.8.dp.toPx())
        )

        drawRoundRect(
            color = CyanAccent.copy(alpha = 0.5f),
            topLeft = Offset(29.dp.toPx(), 33.dp.toPx()),
            size = androidx.compose.ui.geometry.Size(4.dp.toPx(), 2.4.dp.toPx()),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(0.8.dp.toPx())
        )

        drawRoundRect(
            color = CyanAccent.copy(alpha = 0.6f),
            topLeft = Offset(5.dp.toPx(), 21.dp.toPx()),
            size = androidx.compose.ui.geometry.Size(4.dp.toPx(), 8.dp.toPx()),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(1.6.dp.toPx())
        )

        drawRoundRect(
            color = CyanAccent.copy(alpha = 0.6f),
            topLeft = Offset(39.dp.toPx(), 21.dp.toPx()),
            size = androidx.compose.ui.geometry.Size(4.dp.toPx(), 8.dp.toPx()),
            cornerRadius = androidx.compose.ui.geometry.CornerRadius(1.6.dp.toPx())
        )
    }
}

@Composable
private fun SuccessScreen(onBack: () -> Unit) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(
                Brush.verticalGradient(listOf(BgDark, BgMid, BgDark))
            ),
        contentAlignment = Alignment.Center
    ) {
        Box(
            modifier = Modifier
                .fillMaxSize()
                .drawBehind {
                    drawRect(
                        brush = Brush.radialGradient(
                            listOf(
                                Color(0x181A3A7A),
                                Color.Transparent
                            ),
                            center = Offset(size.width / 2f, 0f),
                            radius = size.width * 0.8f
                        )
                    )
                }
        )
        Column(
            modifier = Modifier
                .fillMaxWidth(0.9f)
                .clip(RoundedCornerShape(24.dp))
                .background(Color(0x05FFFFFF))
                .border(1.dp, Color(0x14FFFFFF), RoundedCornerShape(24.dp))
                .padding(32.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(24.dp)
        ) {
            Box(
                modifier = Modifier
                    .size(80.dp)
                    .clip(CircleShape)
                    .background(
                        Brush.linearGradient(listOf(BluePrimary, BlueSecondary))
                    ),
                contentAlignment = Alignment.Center
            ) {
                Text("✓", color = Color.White, fontSize = 40.sp, fontWeight = FontWeight.Bold)
            }
            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                Text(
                    "Access Granted",
                    color = TextPrimary,
                    fontSize = 20.sp,
                    fontWeight = FontWeight.Bold
                )
                Text(
                    "Your device has been activated",
                    color = TextSecondary,
                    fontSize = 12.sp
                )
            }
            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(16.dp))
                    .background(Color(0x14FFFFFF))
                    .border(1.dp, Color(0x1FFFFFFF), RoundedCornerShape(16.dp))
                    .clickable { onBack() }
                    .padding(vertical = 12.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    "← Go Back",
                    color = TextPrimary,
                    fontSize = 13.sp,
                    fontWeight = FontWeight.SemiBold
                )
            }
        }
    }
}

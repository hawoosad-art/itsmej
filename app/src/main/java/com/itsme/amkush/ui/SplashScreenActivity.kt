package com.itsme.amkush.ui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.compose.animation.core.*
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.itsme.amkush.R
import com.itsme.amkush.utils.SharedPrefs
import kotlinx.coroutines.delay
import kotlin.random.Random

private val DarkBg   = Color(0xFF0D0D18)
private val CardBg   = Color(0xFF080812)
private val BarBg    = Color(0xFF0C0C1A)
private val Violet   = Color(0xFF6C63FF)
private val Pink     = Color(0xFFFF4D9D)
private val Cyan     = Color(0xFF00FFFF)
private val Amber    = Color(0xFFFEBC2E)
private val Green    = Color(0xFFA8FF78)

private data class LogEntry(val tag: String, val text: String, val tagColor: Color)

private val LOGS = listOf(
    LogEntry("BOOT",   "EcomCam starting up...",              Violet),
    LogEntry("CORE",   "Core engine loaded",                   Cyan),
    LogEntry("FACE",   "Face recognition module ready",        Pink),
    LogEntry("STREAM", "Decoder initialized..",                Amber),
    LogEntry("GATE",   "Access control layer initialized",     Green),
    LogEntry("UI",     "Interface ready · loading dashboard",  Cyan),
    LogEntry("READY",  "EcomCam is online · happy hooking",   Violet),
)

private const val GLITCH_CHARS = "!@#\$%^&*<>?/\\|{}[]~`"

private fun randomGlitch(len: Int): String =
    (1..len).map { GLITCH_CHARS[Random.nextInt(GLITCH_CHARS.length)] }.joinToString("")

class SplashScreenActivity : ComponentActivity() {


    private val manageStorageLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) {

        proceedToNextScreen()
    }

    /* [V80] POST_NOTIFICATIONS is declared in the manifest but was never
     * requested at runtime, so on Android 13+ it stayed denied and every
     * startForeground() notification was silently dropped by the system. The
     * overlay foreground service therefore ran with NO visible notification —
     * the owner had never seen the Hide/Show controls the channel promises.
     * Fire-and-forget: we proceed regardless of the answer. */
    private val notifPermLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { /* granted or not, the overlay service still works either way */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        SharedPrefs.init(this)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED
        ) {
            notifPermLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
        }

        setContent {
            SplashContent(
                onFinished = {

                    checkStoragePermissionAndProceed()
                }
            )
        }
    }

    private fun checkStoragePermissionAndProceed() {

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            if (!Environment.isExternalStorageManager()) {

                try {
                    val intent = Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION).apply {
                        data = Uri.parse("package:$packageName")
                    }
                    manageStorageLauncher.launch(intent)
                } catch (e: Exception) {

                    val intent = Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)
                    manageStorageLauncher.launch(intent)
                }
                return
            }
        }


        proceedToNextScreen()
    }

    private fun proceedToNextScreen() {
        val next = Intent(this, HomeScreen::class.java)
        startActivity(next)

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            overrideActivityTransition(
                OVERRIDE_TRANSITION_OPEN,
                android.R.anim.fade_in,
                android.R.anim.fade_out
            )
        } else {
            @Suppress("DEPRECATION")
            overridePendingTransition(android.R.anim.fade_in, android.R.anim.fade_out)
        }
        finish()
    }
}

private data class LiveLine(val entry: LogEntry, val chars: Int, val done: Boolean)

@Composable
private fun SplashContent(onFinished: () -> Unit) {
    var lines by remember { mutableStateOf(listOf<LiveLine>()) }
    var cursor by remember { mutableStateOf(true) }
    var progress by remember { mutableStateOf(0) }
    var ready by remember { mutableStateOf(false) }
    var glitchIdx by remember { mutableStateOf<Int?>(null) }


    LaunchedEffect(Unit) {
        while (true) {
            delay(500)
            cursor = !cursor
        }
    }



    LaunchedEffect(Unit) {
        delay(500)

        for (i in LOGS.indices) {
            glitchIdx = i
            delay(120)
            glitchIdx = null

            lines = lines + LiveLine(LOGS[i], 0, false)
            delay(80)

            val full = LOGS[i].text
            for (c in 1..full.length) {
                delay(32)
                lines = lines.mapIndexed { idx, l ->
                    if (idx == i) l.copy(chars = c, done = c == full.length) else l
                }
                progress = (((i + c.toFloat() / full.length) / LOGS.size) * 100).toInt()
            }
            delay(140)
        }

        progress = 100
        ready = true
        delay(2400)
        onFinished()
    }

    Box(
        modifier = Modifier
            .fillMaxSize()
            .background(DarkBg),
        contentAlignment = Alignment.Center
    ) {
        Column(horizontalAlignment = Alignment.CenterHorizontally) {


            Row(
                modifier = Modifier
                    .background(CardBg, RoundedCornerShape(10.dp))
                    .border(1.dp, Cyan.copy(alpha = 0.1f), RoundedCornerShape(10.dp))
                    .padding(horizontal = 20.dp, vertical = 8.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                Text(
                    "ECOMCAM",
                    color = Cyan,
                    fontSize = 11.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace,
                    letterSpacing = 2.sp
                )
                Text(
                    "v2.4.1",
                    color = Cyan.copy(alpha = 0.6f),
                    fontSize = 9.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace,
                    modifier = Modifier
                        .background(Cyan.copy(alpha = 0.06f), RoundedCornerShape(20.dp))
                        .border(1.dp, Cyan.copy(alpha = 0.13f), RoundedCornerShape(20.dp))
                        .padding(horizontal = 7.dp, vertical = 1.dp)
                )
            }

            Spacer(Modifier.height(16.dp))


            Column(
                modifier = Modifier
                    .width(320.dp)
                    .clip(RoundedCornerShape(12.dp))
                    .background(CardBg)
                    .border(1.dp, Color.White.copy(alpha = 0.06f), RoundedCornerShape(12.dp))
            ) {

                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = 16.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        WaveBars(color = Pink, heights = listOf(0.85f, 0.5f, 0.9f, 1f, 0.6f, 0.75f, 0.4f))
                        Image(
                            painter = painterResource(id = R.drawable.splash_hacker),
                            contentDescription = "EcomCam",
                            contentScale = ContentScale.Crop,
                            modifier = Modifier
                                .size(72.dp)
                                .clip(RoundedCornerShape(8.dp))
                                .border(1.dp, Violet.copy(alpha = 0.27f), RoundedCornerShape(8.dp))
                        )
                        WaveBars(color = Violet, heights = listOf(0.4f, 0.7f, 1f, 0.6f, 0.85f, 0.5f, 0.9f))
                    }
                }

                Spacer(Modifier.height(12.dp))


                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .background(BarBg)
                        .border(
                            width = 1.dp,
                            color = Color.White.copy(alpha = 0.05f)
                        )
                        .padding(horizontal = 16.dp, vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Dot(Color(0xFFFF5F57))
                    Dot(Color(0xFFFEBC2E))
                    Dot(Color(0xFF28C840))
                    Text(
                        "@ecomcam — terminal",
                        color = Color.White.copy(alpha = 0.18f),
                        fontSize = 11.sp,
                        letterSpacing = 1.sp,
                        fontFamily = FontFamily.Monospace,
                        modifier = Modifier.weight(1f),
                        textAlign = androidx.compose.ui.text.style.TextAlign.Center
                    )
                }


                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(2.dp)
                        .background(Color.White.copy(alpha = 0.03f))
                ) {
                    Box(
                        modifier = Modifier
                            .fillMaxHeight()
                            .fillMaxWidth(progress / 100f)
                            .background(Brush.horizontalGradient(listOf(Violet, Pink)))
                    )
                }


                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .heightIn(min = 200.dp)
                        .padding(horizontal = 20.dp, vertical = 18.dp)
                ) {
                    glitchIdx?.let { gi ->
                        if (gi < LOGS.size) {
                            LogRow(
                                tagColor = LOGS[gi].tagColor,
                                tag = randomGlitch(LOGS[gi].text.length),
                                text = "",
                                showBadge = false,
                                alpha = 0.35f
                            )
                        }
                    }

                    lines.forEach { line ->
                        LogLineRow(line = line, cursorOn = cursor, ready = ready)
                    }

                    if (lines.isEmpty()) {
                        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                            Text("@ecomcam", color = Violet, fontSize = 12.sp, fontWeight = FontWeight.Bold, fontFamily = FontFamily.Monospace)
                            Text("›", color = Color.White.copy(alpha = 0.12f), fontFamily = FontFamily.Monospace)
                            Text("▋", color = Violet.copy(alpha = if (cursor) 1f else 0f), fontFamily = FontFamily.Monospace)
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun Dot(color: Color) {
    Box(
        modifier = Modifier
            .size(11.dp)
            .clip(CircleShape)
            .background(color)
    )
}

@Composable
private fun WaveBars(color: Color, heights: List<Float>) {
    val infinite = rememberInfiniteTransition(label = "wave")
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(3.dp)) {
        heights.forEachIndexed { i, h ->
            val scale by infinite.animateFloat(
                initialValue = 0.4f,
                targetValue = 1f,
                animationSpec = infiniteRepeatable(
                    animation = tween(600 + i * 90, easing = LinearEasing),
                    repeatMode = RepeatMode.Reverse
                ),
                label = "bar$i"
            )
            Box(
                modifier = Modifier
                    .width(3.dp)
                    .height((h * 28).dp * scale)
                    .background(color.copy(alpha = 0.7f), RoundedCornerShape(2.dp))
            )
        }
    }
}

@Composable
private fun LogLineRow(line: LiveLine, cursorOn: Boolean, ready: Boolean) {
    Row(modifier = Modifier.padding(bottom = 9.dp), verticalAlignment = Alignment.Top) {
        Text(
            "@ecomcam",
            color = Violet,
            fontSize = 12.5.sp,
            fontWeight = FontWeight.Bold,
            fontFamily = FontFamily.Monospace,
            modifier = Modifier.width(80.dp),
            textAlign = androidx.compose.ui.text.style.TextAlign.End
        )
        Spacer(Modifier.width(10.dp))
        Text("›", color = Color.White.copy(alpha = 0.12f), fontFamily = FontFamily.Monospace)
        Spacer(Modifier.width(10.dp))
        Column {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    line.entry.tag,
                    color = line.entry.tagColor,
                    fontSize = 10.5.sp,
                    fontWeight = FontWeight.Bold,
                    fontFamily = FontFamily.Monospace,
                    letterSpacing = 0.8.sp,
                    modifier = Modifier
                        .background(line.entry.tagColor.copy(alpha = 0.09f), RoundedCornerShape(4.dp))
                        .padding(horizontal = 6.dp, vertical = 1.dp)
                )
                Spacer(Modifier.width(10.dp))
                Row {
                    Text(
                        line.entry.text.take(line.chars),
                        color = Color(0xFFE0E0FF),
                        fontSize = 12.5.sp,
                        fontWeight = FontWeight.Bold,
                        fontFamily = FontFamily.Monospace
                    )
                    if (!line.done) {
                        Text(
                            "▋",
                            color = line.entry.tagColor.copy(alpha = if (cursorOn) 1f else 0f),
                            fontFamily = FontFamily.Monospace
                        )
                    }
                    if (line.done && line.entry.tag == "READY" && ready) {
                        Spacer(Modifier.width(8.dp))
                        Box(
                            modifier = Modifier
                                .size(7.dp)
                                .clip(CircleShape)
                                .background(Green)
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun LogRow(tagColor: Color, tag: String, text: String, showBadge: Boolean, alpha: Float) {
    Row(modifier = Modifier.padding(bottom = 9.dp).background(Color.Transparent), verticalAlignment = Alignment.Top) {
        Text(
            "@ecomcam",
            color = Violet.copy(alpha = alpha),
            fontSize = 12.5.sp,
            fontWeight = FontWeight.Bold,
            fontFamily = FontFamily.Monospace,
            modifier = Modifier.width(80.dp),
            textAlign = androidx.compose.ui.text.style.TextAlign.End
        )
        Spacer(Modifier.width(10.dp))
        Text("›", color = Color.White.copy(alpha = 0.12f * alpha / 0.35f), fontFamily = FontFamily.Monospace)
        Spacer(Modifier.width(10.dp))
        Text(
            tag,
            color = tagColor.copy(alpha = alpha),
            fontSize = 12.5.sp,
            fontWeight = FontWeight.Bold,
            fontFamily = FontFamily.Monospace
        )
    }
}

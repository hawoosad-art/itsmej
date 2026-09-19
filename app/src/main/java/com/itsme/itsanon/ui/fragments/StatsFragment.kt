package com.itsme.itsanon.ui.fragments

    import android.os.Environment
    import androidx.compose.animation.core.*
    import androidx.compose.foundation.*
    import androidx.compose.foundation.layout.*
    import androidx.compose.foundation.shape.*
    import androidx.compose.material3.Text
    import androidx.compose.runtime.*
    import androidx.compose.ui.*
    import androidx.compose.ui.draw.*
    import androidx.compose.ui.graphics.*
    import androidx.compose.ui.text.font.*
    import androidx.compose.ui.text.style.*
    import androidx.compose.ui.unit.*
    import com.itsme.itsanon.model.HookStatus
    import com.itsme.itsanon.model.HookStatusRegistry
    import com.itsme.itsanon.utils.SharedPrefs
    import kotlinx.coroutines.*
    import java.io.File
    import java.text.SimpleDateFormat
    import java.util.*

    private val Violet   = Color(0xFF6C63FF)
    private val GreenOk  = Color(0xFF4ADE80)
    private val TextSec  = Color(0x44FFFFFF)
    private val TextMid  = Color(0x88FFFFFF)
    private val Border   = Color(0x1AFFFFFF)
    private val Surface  = Color(0x12FFFFFF)


    private val TermBg     = Color(0xFF07100A)
    private val TermBorder = Color(0xFF1C3A20)
    private val TermHeader = Color(0xFF0D1F10)
    private val CyanBright = Color(0xFF00E5FF)
    private val CyanDim    = Color(0xFF00B8D4)
    private val GreenLog   = Color(0xFF69FF47)
    private val YellowLog  = Color(0xFFFFD600)
    private val RedLog     = Color(0xFFFF4D6D)
    private val GreyLog    = Color(0xFF4A6050)





    private sealed class HookProbeResult {
        object Checking : HookProbeResult()
        object Active   : HookProbeResult()
        object Missing  : HookProbeResult()
        data class NoRoot(val reason: String) : HookProbeResult()
    }

    @Composable
    fun StatsContent(
        targetPackage: String?,
        targetAppName: String?
    ) {
        val effName     = targetAppName ?: SharedPrefs.getTargetAppName()
        val hooks       = remember { HookStatusRegistry.getAllHooks() }
        val grouped     = remember(hooks) { hooks.groupBy { it.category } }
        val activeCount = remember(hooks) { hooks.count { it.isActive } }
        val totalCount  = hooks.size

        val pulse = rememberInfiniteTransition(label = "pulse")
        val blink by pulse.animateFloat(
            initialValue = 1f, targetValue = 0.3f,
            animationSpec = infiniteRepeatable(tween(1400), RepeatMode.Reverse),
            label = "blink"
        )

        var logLines   by remember { mutableStateOf(emptyList<String>()) }
        var hookProbe  by remember { mutableStateOf<HookProbeResult>(HookProbeResult.Checking) }


        LaunchedEffect(Unit) {
            while (isActive) {
                logLines = readLogFile()
                delay(1000L)
            }
        }


        LaunchedEffect(Unit) {
            while (isActive) {
                hookProbe = withContext(Dispatchers.IO) { checkHookInMaps() }
                delay(2000L)
            }
        }

        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
        ) {

            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(start = 20.dp, end = 20.dp, top = 16.dp, bottom = 8.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column {
                    Text("Hook Status", color = Color.White, fontSize = 16.sp, fontWeight = FontWeight.Bold)
                    Text("Active module diagnostics", color = TextSec, fontSize = 11.sp)
                }
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                    // [V30] was a static "ACTIVE" pill — now mirrors the real probe
                    // so the header can't contradict the badge below it.
                    val (pillColor, pillText) = when (hookProbe) {
                        is HookProbeResult.Active   -> GreenOk to "ACTIVE"
                        is HookProbeResult.Missing  -> RedLog to "IDLE"
                        is HookProbeResult.NoRoot   -> YellowLog to "NO ROOT"
                        is HookProbeResult.Checking -> CyanDim to "SCANNING"
                    }
                    Box(Modifier.size(8.dp).clip(CircleShape).background(pillColor.copy(alpha = blink)))
                    Text(pillText, color = pillColor, fontSize = 10.sp, fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Bold)
                }
            }


            if (!effName.isNullOrEmpty()) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp)
                        .clip(RoundedCornerShape(12.dp))
                        .background(Surface)
                        .border(1.dp, Border, RoundedCornerShape(12.dp))
                        .padding(horizontal = 14.dp, vertical = 10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    Text("\uD83C\uDFAF", fontSize = 12.sp)
                    Text(
                        effName, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.SemiBold,
                        maxLines = 1, overflow = TextOverflow.Ellipsis, modifier = Modifier.weight(1f)
                    )
                }
                Spacer(Modifier.height(10.dp))
            }


            HookPresenceBadge(probe = hookProbe)
            Spacer(Modifier.height(10.dp))


            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 16.dp)
                    .clip(RoundedCornerShape(16.dp))
                    .background(Color(0x1A4ADE80))
                    .border(1.dp, GreenOk.copy(0.25f), RoundedCornerShape(16.dp))
                    .padding(horizontal = 16.dp, vertical = 14.dp)
            ) {
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween, verticalAlignment = Alignment.CenterVertically) {
                    Column {
                        Text(
                            "$activeCount / $totalCount",
                            color = GreenOk, fontSize = 28.sp, fontWeight = FontWeight.Bold, fontFamily = FontFamily.Monospace
                        )
                        Text("Hooks active", color = TextMid, fontSize = 12.sp)
                    }
                    Box(
                        modifier = Modifier.size(56.dp).clip(CircleShape)
                            .background(GreenOk.copy(0.15f)).border(2.dp, GreenOk.copy(0.4f), CircleShape),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            "${(activeCount * 100 / totalCount.coerceAtLeast(1))}%",
                            color = GreenOk, fontSize = 12.sp, fontWeight = FontWeight.Bold, fontFamily = FontFamily.Monospace
                        )
                    }
                }
            }

            Spacer(Modifier.height(12.dp))


            grouped.forEach { (category, hooksInCat) ->
                Column(modifier = Modifier.padding(horizontal = 16.dp, vertical = 4.dp)) {
                    Text(category, color = Violet, fontSize = 11.sp, fontWeight = FontWeight.Bold,
                        letterSpacing = 2.sp, modifier = Modifier.padding(bottom = 8.dp, top = 8.dp))
                    Box(
                        modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp))
                            .background(Surface).border(1.dp, Border, RoundedCornerShape(16.dp))
                    ) {
                        Column {
                            hooksInCat.forEachIndexed { idx, hook ->
                                HookRow(hook = hook)
                                if (idx < hooksInCat.lastIndex)
                                    Box(Modifier.fillMaxWidth().height(1.dp).background(Color(0x0AFFFFFF)))
                            }
                        }
                    }
                }
            }

            Spacer(Modifier.height(16.dp))


            TerminalPanel(logLines = logLines)

            Spacer(Modifier.height(28.dp))
        }
    }


    @Composable
    private fun HookPresenceBadge(probe: HookProbeResult) {
        val bgColor     : Color
        val borderColor : Color
        val dotColor    : Color
        val label       : String
        val sublabel    : String

        when (probe) {
            is HookProbeResult.Active -> {
                bgColor     = Color(0xFF071A0C)
                borderColor = Color(0xFF4ADE80).copy(alpha = 0.45f)
                dotColor    = Color(0xFF4ADE80)
                label       = "HOOK ACTIVE \u2713"
                sublabel    = "libhookProxy.so loaded in cameraserver (/proc/maps)"
            }
            is HookProbeResult.Missing -> {
                bgColor     = Color(0xFF1A0707)
                borderColor = Color(0xFFFF4D6D).copy(alpha = 0.45f)
                dotColor    = Color(0xFFFF4D6D)
                label       = "HOOK NOT DETECTED"
                sublabel    = "libhookProxy.so absent from cameraserver memory map"
            }
            is HookProbeResult.NoRoot -> {
                bgColor     = Color(0xFF150F00)
                borderColor = Color(0xFFFFD600).copy(alpha = 0.35f)
                dotColor    = Color(0xFFFFD600)
                label       = "ROOT REQUIRED"
                sublabel    = probe.reason
            }
            is HookProbeResult.Checking -> {
                bgColor     = Color(0xFF0A0A12)
                borderColor = Color(0xFF6C63FF).copy(alpha = 0.35f)
                dotColor    = Color(0xFF6C63FF)
                label       = "SCANNING..."
                sublabel    = "reading /proc/*/cmdline to locate cameraserver"
            }
        }

        val pulse = rememberInfiniteTransition(label = "badge_pulse")
        val alpha by pulse.animateFloat(
            initialValue = 1f,
            targetValue  = if (probe is HookProbeResult.Active) 0.35f else 0.8f,
            animationSpec = infiniteRepeatable(tween(1100, easing = FastOutSlowInEasing), RepeatMode.Reverse),
            label = "badge_alpha"
        )

        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp)
                .clip(RoundedCornerShape(14.dp))
                .background(bgColor)
                .border(1.dp, borderColor, RoundedCornerShape(14.dp))
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {

            Box(Modifier.size(10.dp).clip(CircleShape).background(dotColor.copy(alpha = alpha)))

            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
                Text(label,    color = dotColor,                fontSize = 12.sp, fontWeight = FontWeight.Bold, fontFamily = FontFamily.Monospace)
                Text(sublabel, color = dotColor.copy(alpha = 0.55f), fontSize = 9.sp, fontFamily = FontFamily.Monospace)
            }


            Text(
                text = "/proc/maps",
                color = dotColor.copy(alpha = 0.30f),
                fontSize = 8.sp,
                fontFamily = FontFamily.Monospace
            )
        }
    }

    @Composable
    private fun TerminalPanel(logLines: List<String>) {
        val scope      = rememberCoroutineScope()
        var saveMsg    by remember { mutableStateOf("") }
        val termScroll = rememberScrollState()

        LaunchedEffect(logLines.size) {
            if (logLines.isNotEmpty()) termScroll.scrollTo(termScroll.maxValue)
        }

        Column(modifier = Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {


            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clip(RoundedCornerShape(topStart = 14.dp, topEnd = 14.dp))
                    .background(TermHeader)
                    .border(1.dp, TermBorder, RoundedCornerShape(topStart = 14.dp, topEnd = 14.dp))
                    .padding(horizontal = 14.dp, vertical = 9.dp),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(5.dp)) {
                    Box(Modifier.size(9.dp).clip(CircleShape).background(Color(0xFFFF5F57)))
                    Box(Modifier.size(9.dp).clip(CircleShape).background(Color(0xFFFFBD2E)))
                    Box(Modifier.size(9.dp).clip(CircleShape).background(Color(0xFF28CA42)))
                    Spacer(Modifier.width(6.dp))
                    Text("ecomcam — live log", color = CyanDim, fontSize = 11.sp, fontFamily = FontFamily.Monospace)
                    Text(" (${logLines.size})", color = GreyLog, fontSize = 10.sp, fontFamily = FontFamily.Monospace)
                }
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (saveMsg.isNotEmpty())
                        Text(saveMsg, color = GreenLog, fontSize = 9.sp, fontFamily = FontFamily.Monospace)
                    Box(
                        modifier = Modifier
                            .clip(RoundedCornerShape(7.dp))
                            .background(Color(0xFF112211))
                            .border(1.dp, CyanDim.copy(0.35f), RoundedCornerShape(7.dp))
                            .clickable {
                                scope.launch {
                                    val ok = saveLog(logLines)
                                    saveMsg = if (ok) "\u2713 saved" else "\u2717 failed"
                                    delay(2500)
                                    saveMsg = ""
                                }
                            }
                            .padding(horizontal = 11.dp, vertical = 5.dp)
                    ) {
                        Text("\u2B07 Save", color = CyanBright, fontSize = 10.sp,
                            fontFamily = FontFamily.Monospace, fontWeight = FontWeight.SemiBold)
                    }
                }
            }


            Box(
                modifier = Modifier
                    .fillMaxWidth()
                    .height(340.dp)
                    .clip(RoundedCornerShape(bottomStart = 14.dp, bottomEnd = 14.dp))
                    .background(TermBg)
                    .border(1.dp, TermBorder, RoundedCornerShape(bottomStart = 14.dp, bottomEnd = 14.dp))
            ) {
                if (logLines.isEmpty()) {
                    Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                        Text(
                            "$ waiting for ecomcam logs...\n\nStart injection to see live output",
                            color = GreyLog, fontSize = 11.sp, fontFamily = FontFamily.Monospace,
                            textAlign = TextAlign.Center
                        )
                    }
                } else {
                    Column(
                        modifier = Modifier.fillMaxSize()
                            .verticalScroll(termScroll)
                            .padding(horizontal = 10.dp, vertical = 8.dp),
                        verticalArrangement = Arrangement.spacedBy(2.dp)
                    ) {
                        logLines.forEach { line -> TermLine(line) }
                    }
                }
            }
        }
    }

    @Composable
    private fun TermLine(line: String) {
        val color = when {
            line.contains(" E/") || line.contains("[ERROR]") || line.contains("FATAL") -> RedLog
            line.contains(" W/") || line.contains("[WARN]")  || line.contains("WARN")  -> YellowLog
            line.contains(" I/") || line.contains("[INFO]")  || line.contains("INFO")  -> GreenLog
            else -> CyanBright
        }
        Text(
            text = line,
            color = color,
            fontSize = 10.5.sp,
            fontFamily = FontFamily.Monospace,
            lineHeight = 15.sp,
            overflow = TextOverflow.Ellipsis,
            maxLines = 3
        )
    }

    @Composable
    private fun HookRow(hook: HookStatus) {
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 14.dp, vertical = 10.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Text(hook.name, color = Color.White, fontSize = 13.sp, fontWeight = FontWeight.Medium, modifier = Modifier.weight(1f))
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
                Box(Modifier.size(8.dp).clip(CircleShape).background(if (hook.isActive) Color(0xFF4ADE80) else Color(0xFFFF4D6D)))
                Text(
                    if (hook.isActive) "Active" else "Inactive",
                    color = if (hook.isActive) Color(0xFF4ADE80) else Color(0xFFFF4D6D),
                    fontSize = 11.sp, fontFamily = FontFamily.Monospace
                )
            }
        }
    }









    /* [V30] Pre-V30 this screen ALWAYS showed "HOOK NOT DETECTED" while the hook
     * was live: untrusted_app cannot read cameraserver's /proc/<pid>/cmdline or
     * /maps (SELinux), so the pid scan came up empty and File.canRead() lied for
     * the direct read. Two signals don't need those reads:
     *   1. the hook binds abstract socket @itsanon_frame_fd — visible in the
     *      world-readable /proc/net/unix (same liveness signal ModuleManager uses);
     *   2. a root `cat` of the maps file works where the app's own read can't.
     */
    private fun checkHookInMaps(): HookProbeResult {
        return try {
            // Signal 1 (no root): the IPC socket only exists while the hook is serving.
            val unixSocks = try { File("/proc/net/unix").readText() } catch (_: Exception) { "" }
            val ipcLive = unixSocks.contains("itsanon_frame_fd")

            // Locate cameraserver pid via root (app-level /proc scan is SELinux-blind).
            val csPid = shellText(
                "for p in /proc/[0-9]*; do " +
                "c=\$(cat \$p/cmdline 2>/dev/null | tr -d '\\0'); " +
                "case \"\$c\" in *cameraserver*) echo \${p#/proc/}; break;; esac; done"
            ).trim().toIntOrNull() ?: 0

            if (csPid == 0 && !ipcLive) {
                return HookProbeResult.NoRoot("cameraserver not found and no hook IPC socket")
            }

            var inMaps = false
            if (csPid > 0) {
                // Signal 2: root cat beats the app's unreadable direct read.
                val maps = shellText("cat /proc/$csPid/maps 2>/dev/null")
                inMaps = maps.contains("libhookProxy") || maps.contains("shadowhook")
            }

            if (inMaps || ipcLive) HookProbeResult.Active else HookProbeResult.Missing
        } catch (e: Exception) {
            HookProbeResult.NoRoot("probe error: ${e.message?.take(60)}")
        }
    }

    private fun shellText(cmd: String): String = try {
        val proc = Runtime.getRuntime().exec(arrayOf("su", "-c", cmd))
        val out = proc.inputStream.bufferedReader().use { it.readText() }
        proc.waitFor()
        out
    } catch (_: Exception) { "" }



    private fun readLogFile(): List<String> = try {
        val f = File(
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
            "com.itsme.itsanon/itsanon_hook_logs.txt"
        )
        if (!f.exists()) listOf("$ no log file yet — start injection to generate logs")
        else { val all = f.readLines(); if (all.size <= 300) all else all.takeLast(300) }
    } catch (e: Throwable) { listOf("$ cannot read log: ${e.message}") }

    private fun saveLog(lines: List<String>): Boolean = try {
        val dest = File(
            Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
            "facegate.txt"
        )
        val ts = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US).format(Date())
        dest.writeText(buildString {
            appendLine("# EcomCam Log Export — $ts")
            appendLine("# ${lines.size} lines")
            appendLine()
            lines.forEach { appendLine(it) }
        })
        true
    } catch (_: Throwable) { false }

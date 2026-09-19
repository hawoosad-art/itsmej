package com.itsme.amkush.logging

import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import com.itsme.amkush.utils.DeviceUtils
import com.itsme.amkush.utils.Logger
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean

/**
 * [V77] Crash + ANR capture. The owner hit a freeze/crash with zero trace
 * anywhere ("it crashed but idk what crashed"), so the app now:
 *  - installs an uncaught-exception handler that writes a full crash
 *    report (device header + stack + every thread's stack) and pushes it
 *    to Mylogs via GitHubLogUploader, same as the other logs, BEFORE
 *    letting the previous handler kill the process;
 *  - runs a main-looper watchdog: if the UI thread fails to answer a
 *    posted ping within 10 s, that IS an ANR — dump all threads and
 *    upload it as anr_<ver>.txt, then re-arm once the looper recovers.
 */
object CrashLogger {
    private const val TAG = "amkush/crash"
    private const val ANR_TIMEOUT_MS = 10_000L
    private val sdf = SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.US)

    private var appCtx: Context? = null
    private val crashReported = AtomicBoolean(false)

    fun init(context: Context) {
        appCtx = context.applicationContext
        installUncaughtHandler()
        startAnrWatchdog()
    }

    private fun header(kind: String): String =
        DeviceUtils.buildLogHeader("$kind — ${sdf.format(Date())}")

    // ── Crash ──────────────────────────────────────────────────────────────

    private fun installUncaughtHandler() {
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, ex ->
            try {
                if (crashReported.compareAndSet(false, true)) {
                    val sw = StringWriter()
                    sw.write(header("CRASH REPORT (thread '${thread.name}')"))
                    sw.write("pid=${android.os.Process.myPid()} uid=${android.os.Process.myUid()}\n\n")
                    ex.printStackTrace(PrintWriter(sw))
                    sw.write("\n=== ALL THREADS ===\n")
                    dumpAllThreads(sw)
                    /* The upload must happen off this thread when the crash
                     * landed on the main looper (network-on-main is fatal);
                     * join with a cap so the process still dies promptly. */
                    val body = sw.toString()
                    val t = Thread { report("crash", body) }
                    t.isDaemon = true
                    t.start()
                    t.join(20_000L)
                }
            } catch (_: Exception) {
            }
            previous?.uncaughtException(thread, ex)
        }
    }

    // ── ANR watchdog ───────────────────────────────────────────────────────

    private fun startAnrWatchdog() {
        val main = Handler(Looper.getMainLooper())
        val t = Thread {
            while (true) {
                val responded = AtomicBoolean(false)
                try {
                    main.post { responded.set(true) }
                    Thread.sleep(ANR_TIMEOUT_MS)
                } catch (_: InterruptedException) {
                    return@Thread
                }
                if (!responded.get()) {
                    try {
                        val sw = StringWriter()
                        sw.write(header("ANR REPORT (main thread stalled > ${ANR_TIMEOUT_MS / 1000}s)"))
                        sw.write("=== ALL THREADS ===\n")
                        dumpAllThreads(sw)
                        report("anr", sw.toString())
                    } catch (_: Exception) {
                    }
                    /* Don't spam: wait for the looper to recover before the
                     * next arming cycle. */
                    var waits = 0
                    while (!responded.get() && waits < 24) {
                        try { Thread.sleep(5_000L) } catch (_: InterruptedException) { return@Thread }
                        waits++
                    }
                }
            }
        }
        t.isDaemon = true
        t.name = "amkush-anr-watchdog"
        t.start()
    }

    // ── Report writer/uploader ─────────────────────────────────────────────

    private fun report(kind: String, body: String) {
        try {
            val ver = Build.VERSION.RELEASE.split(".").first()
            val name = "${kind}_${ver}.txt"
            /* Keep a local copy in /data/local/tmp next to the other logs
             * (root-reachable even if the upload fails). */
            try {
                val f = File(appCtx?.filesDir ?: return, "fg_$name")
                f.writeText(body)
                su("cp '${f.absolutePath}' '/data/local/tmp/$name' 2>/dev/null; " +
                   "chmod 666 '/data/local/tmp/$name' 2>/dev/null; true")
            } catch (_: Exception) {
            }
            val ok = GitHubLogUploader.upload(
                name, body.toByteArray(),
                "EcomCam $kind report (${Build.MANUFACTURER} ${Build.MODEL}, ${DeviceUtils.INJECTOR_VERSION})"
            )
            Logger.e(TAG, "$kind report uploaded=$ok (${body.length} chars)")
        } catch (_: Exception) {
        }
    }

    private fun dumpAllThreads(sw: StringWriter) {
        val all = Thread.getAllStackTraces()
        /* Current/main thread first — its stack is the interesting one. */
        val current = Thread.currentThread()
        val ordered = listOf(current) + all.keys.filter { it !== current }
        for (th in ordered) {
            val trace = if (th === current) th.stackTrace else (all[th] ?: emptyArray())
            sw.write("\n--- \"${th.name}\" ${th.state} prio=${th.priority}" +
                     (if (th.isDaemon) " daemon" else "") + " ---\n")
            for (e in trace) sw.write("    at $e\n")
        }
    }

    private fun su(cmd: String): String = try {
        val proc = Runtime.getRuntime().exec(arrayOf("su", "-c", cmd))
        val out = proc.inputStream.bufferedReader().use { it.readText() }
        proc.waitFor()
        out
    } catch (e: Exception) {
        ""
    }
}

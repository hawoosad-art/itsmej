package com.itsme.amkush.manager

import android.content.Context
import android.util.Log
import com.itsme.amkush.AppState
import com.itsme.amkush.BuildConfig
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

class ModuleManager(private val context: Context) {

    companion object {
        const val TAG = "ModuleManager"

        /**
         * Process-wide mutex so concurrent injectNow() calls don't race.
         *
         * Root cause of ARM32 SDK 29 failures: two coroutines (threads 30054 and
         * 30187) were both running injectNow() simultaneously. Each killed the
         * cameraserver the other was about to inject into, so both ended up
         * ptrace-attaching a process that was already dead → PTRACE_SETREGS
         * "No such process" / callSyscall SIGTRAP cascade. Serialising with a
         * lock means only one injection attempt runs at a time.
         */
        private val injectLock = ReentrantLock()
    }

    enum class SafetyState {
        ACTIVE,
        DISABLED_MANUAL,
        DISABLED_CRASH_LOOP,
        CRASH_RETRY,
        NOT_INSTALLED,
        NO_ROOT
    }

    data class ModuleStatus(
        val installed: Boolean,
        val enabled: Boolean,
        val active: Boolean,
        val safetyState: SafetyState,
        val crashCount: Int,
        val manager: String?,
        val zygiskNextInstalled: Boolean,
        val logTail: String,
        val cameraserverPid: String,
        val hookMapsLine: String,
        val moduleVersion: String
    )

    fun hasRoot(): Boolean = try {
        shell("id").contains("uid=0")
    } catch (e: Exception) { false }

    suspend fun getStatus(): ModuleStatus = withContext(Dispatchers.IO) {
        if (!hasRoot()) {
            return@withContext ModuleStatus(
                installed = false, enabled = false, active = false,
                safetyState = SafetyState.NO_ROOT, crashCount = 0,
                manager = null, zygiskNextInstalled = false,
                logTail = "No root access - grant root to use FaceGate",
                cameraserverPid = "", hookMapsLine = "", moduleVersion = ""
            )
        }

        // Pure in-app ptrace injection: no Magisk module is installed or used.
        // "installed" reflects the app being ready to inject, "active" reflects
        // whether the hook is currently detected in cameraserver.
        val (active, csPid, mapsLine) = detectHookInCameraserver()

        AppState.useNativeHook = active

        val logs = getLogTail()
        val version = BuildConfig.FG_VERSION.ifEmpty { "unknown" }

        return@withContext ModuleStatus(
            installed = true,
            enabled = true,
            active = active,
            safetyState = SafetyState.ACTIVE,
            crashCount = 0,
            manager = null,
            zygiskNextInstalled = true,
            logTail = logs,
            cameraserverPid = csPid,
            hookMapsLine = mapsLine,
            moduleVersion = version
        )
    }

    private fun detectHookInCameraserver(): Triple<Boolean, String, String> {

        val csPid = shellSilent(
            "for pid_path in /proc/[0-9]*; do " +
            "  pid=\$(basename \"\$pid_path\"); " +
            "  [ -f /proc/\$pid/cmdline ] || continue; " +
            "  base=\$(cat /proc/\$pid/cmdline 2>/dev/null | tr '\\0' ' ' | sed 's|.*/||' | tr -d ' '); " +
            "  [ \"\$base\" = \"cameraserver\" ] && echo \$pid && break; " +
            "done"
        )

        if (csPid.isEmpty()) {
            return Triple(false, "not running", "")
        }

        val mapsOutput = shellSilent(
            "grep -E 'hookProxy|shadowhook|anon:sh|/memfd:|libhookProxy' /proc/$csPid/maps 2>/dev/null || echo ''"
        )
        val isHookedByMaps = mapsOutput.isNotEmpty() &&
            (mapsOutput.contains("libhookProxy") ||
             mapsOutput.contains("shadowhook") ||
             mapsOutput.contains("anon:sh") ||
             mapsOutput.contains("/memfd:"))


        val stateFile = java.io.File(context.filesDir, "hook_active_pid")
        val savedPid  = try { stateFile.readText().trim() } catch (_: Exception) { "" }
        val isHookedByStateFile = savedPid.isNotEmpty() && savedPid == csPid
        if (!isHookedByStateFile && stateFile.exists() && savedPid.isNotEmpty() && savedPid != csPid) {

            stateFile.delete()
        }

        val isHooked = isHookedByMaps || isHookedByStateFile

        val hookLine = if (isHooked) {
            when {
                mapsOutput.contains("libhookProxy") ->
                    mapsOutput.lines().first { it.contains("libhookProxy") }
                mapsOutput.contains("shadowhook") ->
                    mapsOutput.lines().first { it.contains("shadowhook") }
                mapsOutput.contains("anon:sh") ->
                    mapsOutput.lines().first { it.contains("anon:sh") }
                isHookedByStateFile ->
                    "injected (pid=$csPid, confirmed by state file)"
                else -> ""
            }
        } else ""

        return Triple(isHooked, csPid, hookLine)
    }

    private fun getLogTail(): String {
        val tags = listOf(
            "amkush/main", "amkush/hook_proxy", "amkush/watchdog", "amkush/service",
            "amkush/frame_inject", "amkush/frame_source", "amkush/frame_producer",
            "amkush/ipc_socket", "amkush/sym_resolver", "amkush/stream_map",
            "ModuleManager"
        ).joinToString(" ") { "-s $it:V" }
        val logcatOut = shellSilent("logcat -d -t 60 $tags 2>/dev/null").trim()
        if (logcatOut.isNotEmpty()) {
            return logcatOut.lines().takeLast(30).joinToString("\n")
        }
        return "No logs yet — tap INJECT or start watchdog to see output"
    }


    private fun shell(cmd: String): String {
        val process = Runtime.getRuntime().exec("su -c $cmd")
        process.waitFor(10, TimeUnit.SECONDS)
        return process.inputStream.bufferedReader().readText().trim()
    }

    private fun shellSilent(cmd: String): String {
        return try {
            shell(cmd)
        } catch (e: Exception) {
            ""
        }
    }

    /**
     * Grants cameraserver the SELinux permissions needed to load a hook lib from
     * /data/local/tmp (label shell_data_file) and to be ptrace'd by the injector.
     * Works even when SELinux can't be set permissive (MediaTek/Android 11), where
     * the hook lib's openat/open/execute/map would otherwise be denied.
     */
    private fun grantSelinuxForInjection(hookPath: String) {
        val rules = listOf(
            // cameraserver may read/open/execute/map the hook file under /data/local/tmp
            "allow cameraserver shell_data_file file { read open getattr execute map execute_no_trans }",
            "allow cameraserver shell_data_file dir { read search open getattr }",
            // injector (root/shell/magisk/su) may ptrace cameraserver and read its maps
            "allow su cameraserver process { ptrace }",
            "allow shell cameraserver process { ptrace }",
            "allow magisk cameraserver process { ptrace }",
            "allow su cameraserver dir { search read open getattr }",
            "allow su cameraserver file { read open getattr map }",
            // ── Android 11 fix ──────────────────────────────────────────────
            // ShadowHook's trampoline hub needs anonymous EXECUTABLE memory
            // (mmap PROT_EXEC|MAP_ANONYMOUS). When SELinux is enforcing this is
            // the `execmem` permission — and on devices that can't drop SELinux
            // (e.g. TECNO CE9 Android 11: setenforce 0 blocked) every hook fails
            // with "Create hub failed", so no frame injection happens and the
            // real camera shows through. Grant cameraserver execmem + execmod.
            "allow cameraserver cameraserver process { execmem execmod }"
        )
        // Try each root manager's live-policy tool (best-effort). Covers Magisk,
        // KernelSU and APatch. Break on the first tool that applies at least one rule.
        val tools = listOf(
            "magiskpolicy",
            "supolicy",
            "ksud policy --add",
            "apd",
            "apctl"
        )
        for (tool in tools) {
            var ok = false
            for (rule in rules) {
                val cmd = when (tool) {
                    "magiskpolicy"        -> "magiskpolicy --live '$rule'"
                    "supolicy"            -> "supolicy --live '$rule'"
                    "ksud policy --add"   -> "ksud policy --add '$rule'"
                    "apd"                 -> "apd --live '$rule'"
                    "apctl"               -> "apctl policy --add '$rule'"
                    else -> continue
                }
                val r = shellSilent("$cmd 2>/dev/null && echo ok || echo fail")
                if (r.contains("ok")) ok = true
            }
            if (ok) {
                Log.i(TAG, "grantSelinuxForInjection: applied via $tool")
                break
            }
        }
    }




    suspend fun injectNow(): Result<Unit> = withContext(Dispatchers.IO) {
        // Serialise: only one injection attempt at a time (blocking wait, not tryLock).
        //
        // Fix for ARM32 SDK 29 (Android 10): two coroutines (user trigger + watchdog
        // reacting to the cameraserver restart that Fix2 itself caused) were both calling
        // injectNow() simultaneously.  tryLock() (non-blocking) let the second coroutine
        // fail fast and retry, which repeatedly killed each other's cameraserver mid-ptrace,
        // producing the "PTRACE_SETREGS: No such process" / callSyscall SIGTRAP cascade.
        //
        // With a blocking 60 s wait: the second coroutine waits for the first to finish;
        // after the lock is released the post-lock check sees injection is already active
        // and returns immediately — no second injection attempt, no kill race.
        val acquired = injectLock.tryLock(60L, java.util.concurrent.TimeUnit.SECONDS)
        if (!acquired) {
            Log.w(TAG, "injectNow: timed out waiting for injection lock — another attempt is still running")
            return@withContext Result.failure(IllegalStateException("Injection already in progress"))
        }
        try {
            // Post-lock check: if the first waiter already completed injection successfully,
            // don't inject again (prevents redundant kill-and-reinject on watchdog triggers).
            val (hookPresent, pidAfterWait, _) = detectHookInCameraserver()
            if (hookPresent) {
                Log.i(TAG, "injectNow: post-lock check — hook already active in pid=$pidAfterWait, skipping")
                return@withContext Result.success(Unit)
            }
            injectNowLocked()
        } finally { injectLock.unlock() }
    }

    private suspend fun injectNowLocked(): Result<Unit> = withContext(Dispatchers.IO) {
        try {
            if (!hasRoot()) return@withContext Result.failure(IllegalStateException("Root access required"))

            val (_, csPid, _) = detectHookInCameraserver()
            if (csPid.isEmpty() || csPid == "not running") {
                return@withContext Result.failure(IllegalStateException("cameraserver is not running"))
            }
            val pidInt = csPid.trim().toIntOrNull() ?: 0
            if (pidInt <= 0) {
                return@withContext Result.failure(IllegalStateException("Invalid cameraserver PID: $csPid"))
            }


            val libCheck = shellSilent(
                "grep -c libcameraservice /proc/$pidInt/maps 2>/dev/null || echo 0"
            ).trim()
            if (libCheck == "0" || libCheck.isEmpty()) {
                return@withContext Result.failure(IllegalStateException(
                    "PID $pidInt does not have libcameraservice.so mapped — not cameraserver, aborting inject"
                ))
            }
            Log.i(TAG, "injectNow: PID=$pidInt verified has libcameraservice.so ($libCheck mapping(s))")

            // Detect cameraserver bitness from ELF header — NOT device ABI.
            // On SDK 29 cameraserver is a 32-bit binary even on arm64 hardware.
            // ELFCLASS32=0x01, ELFCLASS64=0x02 (byte offset 4 of ELF header).
            val csElfClass = shellSilent(
                "head -c 5 /proc/$pidInt/exe 2>/dev/null | tail -c 1 | od -An -t u1 | tr -d ' \n'"
            ).trim()
            val csIs64 = csElfClass != "1"   // default to 64-bit if unreadable
            Log.i(TAG, "injectNow: cameraserver pid=$pidInt elfClass=$csElfClass is64=$csIs64")
            val (injectorAsset, hookAsset) = if (csIs64) {
                "system/bin/amkush_injector64" to "system/lib64/libhookProxy.so"
            } else {
                "system/bin/amkush_injector32" to "system/lib/libhookProxy.so"
            }

            val injectorDest = File(context.filesDir, injectorAsset.substringAfterLast("/")).absolutePath
            val hookDest     = File(context.filesDir, "libhookProxy.so").absolutePath


            Log.i(TAG, "injectNow: evicting lingering injector/watchdog before re-inject")
            stopWatchdogDaemon()
            shellSilent("pkill -f amkush_injector 2>/dev/null || true")
            Thread.sleep(400L)

            extractAssetToDest(injectorAsset, injectorDest)
            extractAssetToDest(hookAsset, hookDest)
            shellSilent("chmod 0755 $injectorDest")


            val selinuxEnforce = shellSilent("cat /sys/fs/selinux/enforce 2>/dev/null").trim()
            val wasEnforcing = selinuxEnforce == "1"
            if (wasEnforcing) {
                Log.i(TAG, "injectNow: disabling SELinux for injection window")
                shellSilent("echo 0 > /sys/fs/selinux/enforce 2>/dev/null || true")
                Thread.sleep(150)
                // VERIFY the toggle actually took effect. If cameraserver stays
                // enforcing, the injected library's executable segment can't be
                // mmap'd -> "couldn't map ... segment N: Permission denied".
                val enforceAfter = shellSilent("cat /sys/fs/selinux/enforce 2>/dev/null").trim()
                Log.i(TAG, "injectNow: SELinux enforce after toggle='$enforceAfter' (expect 0=permissive)")
                if (enforceAfter != "0") {
                    Log.w(TAG, "injectNow: WARNING — sysfs toggle did NOT drop SELinux, trying setenforce")
                    shellSilent("setenforce 0 2>/dev/null || true")
                    Thread.sleep(150)
                    val enforceAfter2 = shellSilent("cat /sys/fs/selinux/enforce 2>/dev/null").trim()
                    Log.i(TAG, "injectNow: SELinux enforce after setenforce='$enforceAfter2'")
                }
            } else {
                Log.i(TAG, "injectNow: SELinux already permissive ($selinuxEnforce)")
            }

            // ── Retry ladder (V4.9.13) ──────────────────────────────────────
            // One dlopen mode doesn't work everywhere. On ported Android 16 ROMs
            // (e.g. M2006C3LI mt6765 GSI) the plain-path remote dlopen makes
            // cameraserver exit(0) cleanly ("Target process exited (0)"), so we
            // try each mode in order, re-starting cameraserver fresh between
            // attempts (a failed attempt can leave the process dead).
            val deviceSdk = shellSilent("getprop ro.build.version.sdk").trim().toIntOrNull() ?: 0
            Log.i(TAG, "injectNow: deviceSdk=$deviceSdk csIs64=$csIs64")
            val hook64 = "/data/local/tmp/libhookProxy64.so"
            val hook32 = "/data/local/tmp/libhookProxy32.so"
            val attempts: List<Pair<String, String>> = when {
                !csIs64 -> listOf(
                    "--filefd" to hook32,
                    "" to hook32
                )
                deviceSdk >= 33 -> listOf(
                    "" to hook64,
                    "--filefd" to hook64,
                    "--memfd" to hookDest
                )
                else -> listOf(
                    "--memfd" to hookDest,
                    "--filefd" to hook64,
                    "" to hook64
                )
            }

            var hooked = false
            var outText = ""
            var exitCode = -1
            var successPid = 0
            var currentPid = pidInt

            for ((idx, attempt) in attempts.withIndex()) {
                val (dlopenFlag, hookPath) = attempt
                if (hooked) break

                // Stage the hook file for this mode. rm -f first so a still-mapped
                // stale .so can't make cp fail with ETXTBSY (Text file busy).
                if (hookPath.startsWith("/data/local/tmp/")) {
                    shellSilent("rm -f $hookPath; cp $hookDest $hookPath && chmod 755 $hookPath 2>/dev/null || true")
                }
                // Grant cameraserver SELinux access to the hook file in
                // /data/local/tmp (label shell_data_file) so it can open/execute/map it.
                grantSelinuxForInjection(hookPath)

                // Fresh cameraserver per attempt — a failed dlopen (clean exit on
                // ported ROMs) leaves the process dead, so re-kill + re-detect.
                val modeName = when {
                    dlopenFlag.contains("filefd") -> "filefd"
                    dlopenFlag.contains("memfd") -> "memfd"
                    else -> "plain"
                }
                Log.i(TAG, "injectNow: attempt ${idx + 1}/${attempts.size} — mode=$modeName hook=$hookPath")
                Log.i(TAG, "injectNow: Fix2 — restarting cameraserver (pid=$currentPid)")
                shellSilent("kill -9 $currentPid 2>/dev/null || true")
                Thread.sleep(1800L)
                val (_, newCsPid, _) = detectHookInCameraserver()
                val newPidInt = newCsPid.trim().toIntOrNull() ?: 0
                currentPid = if (newPidInt > 0) newPidInt else currentPid
                Log.i(TAG, "injectNow: effective inject pid=$currentPid")

                val cmd = "$injectorDest --pid $currentPid --libs $hookPath $dlopenFlag --timeout 5000".trim()
                val proc = Runtime.getRuntime().exec(arrayOf("su", "-c", cmd))
                proc.waitFor(15, TimeUnit.SECONDS)
                outText = proc.inputStream.bufferedReader().readText().trim() +
                          proc.errorStream.bufferedReader().readText().trim()
                exitCode = proc.exitValue()
                Log.i(TAG, "=== injectNow attempt ${idx + 1} rc=$exitCode ===")
                if (outText.isNotEmpty()) {
                    outText.chunked(400).forEach { chunk -> Log.i(TAG, "injector: $chunk") }
                }

                Thread.sleep(1500)
                val (h, _, _) = detectHookInCameraserver()
                if (h) {
                    hooked = true
                    successPid = currentPid
                    Log.i(TAG, "injectNow: SUCCESS via mode=$modeName (pid=$successPid)")
                } else {
                    Log.w(TAG, "injectNow: mode=$modeName did not hook — trying next")
                }
            }

            if (wasEnforcing) {
                shellSilent("echo 1 > /sys/fs/selinux/enforce 2>/dev/null || true")
                Log.i(TAG, "injectNow: SELinux restored to enforcing")
            }
            if (hooked) {

                try {
                    // Persist the ACTUAL injected pid. Cameraserver is killed and
                    // restarted (Fix2) so the real injected pid is successPid
                    // (the NEW cameraserver), not pidInt (the OLD one). Storing the
                    // wrong pid made detection fail after an app restart → the UI
                    // showed INJECT again even though the hook was still in the new
                    // cameraserver.
                    java.io.File(context.filesDir, "hook_active_pid").writeText(successPid.toString())
                    Log.i(TAG, "injectNow: hook_active_pid state file written (pid=$successPid)")
                } catch (e: Exception) {
                    Log.w(TAG, "injectNow: could not write hook_active_pid state file", e)
                }
















                try {

                    val actualContext = try {
                        java.io.File("/proc/${android.os.Process.myPid()}/attr/current")
                            .readText().trim()
                    } catch (_: Exception) { "" }
                    val detectedDomain = actualContext.split(":").getOrNull(2)
                        ?.takeIf { it.isNotEmpty() } ?: ""
                    Log.i(TAG, "injectNow: app SELinux context='$actualContext' domain='$detectedDomain'")


                    val sourceDomains = linkedSetOf(
                        "untrusted_app", "untrusted_app_25", "untrusted_app_27",
                        "platform_app", "priv_app"
                    ).also { if (detectedDomain.isNotEmpty()) it.add(detectedDomain) }

                    var anyOk = false
                    for (domain in sourceDomains) {



                        val rule = "allow $domain cameraserver:unix_stream_socket { connectto read write }"
                        val result = try {
                            val proc = Runtime.getRuntime().exec(arrayOf(
                                "su", "-c",
                                "magiskpolicy --live '$rule' 2>/dev/null && echo magisk_ok" +
                                " || supolicy --live '$rule' 2>/dev/null && echo su_ok" +
                                " || ksud policy --add '$rule' 2>/dev/null && echo ksu_ok" +
                                " || echo unsupported"
                            ))
                            proc.waitFor(5, TimeUnit.SECONDS)
                            proc.inputStream.bufferedReader().readText().trim()
                        } catch (ex: Exception) {
                            "exec_error: ${ex.message}"
                        }
                        val ok = result.contains("_ok")
                        Log.i(TAG, "injectNow: SELinux rule [$domain] → $result")
                        if (ok) anyOk = true
                    }

                    if (!anyOk) {




                        Log.w(TAG, "injectNow: persistent SELinux rule could not be applied " +
                              "— NativeFrameProducer will use per-connect setenforce window")
                    }
                } catch (e: Exception) {
                    Log.w(TAG, "injectNow: SELinux rule apply error", e)
                }
















                if (!isWatchdogRunning()) {
                    Log.i(TAG, "injectNow: starting watchdog daemon for auto-reinject on cameraserver restart")
                    try {
                        startWatchdogDaemon()
                        Log.i(TAG, "injectNow: watchdog daemon started successfully")
                    } catch (e: Exception) {


                        Log.w(TAG, "injectNow: watchdog start failed (non-fatal): ${e.message}")
                    }
                } else {
                    Log.i(TAG, "injectNow: watchdog already running — skipping auto-start")
                }

                Result.success(Unit)
            } else {
                Result.failure(RuntimeException(
                    "Injector ran but hook not visible in maps. rc=$exitCode out=${outText.take(150)}"
                ))
            }
        } catch (e: Exception) {
            Log.e(TAG, "injectNow failed", e)
            Result.failure(e)
        }
    }



    suspend fun startWatchdogDaemon(): Result<Unit> = withContext(Dispatchers.IO) {
        try {
            if (!hasRoot()) return@withContext Result.failure(IllegalStateException("Root access required"))

            // Detect cameraserver bitness from ELF header so the watchdog and injector
            // binaries match the target process — NOT the device ABI (differs on SDK 29).
            val wdCsPid = shellSilent("pgrep -f cameraserver 2>/dev/null | head -1").trim()
            val wdElfClass = if (wdCsPid.isNotEmpty()) {
                shellSilent(
                    "head -c 5 /proc/$wdCsPid/exe 2>/dev/null | tail -c 1 | od -An -t u1 | tr -d ' \n'"
                ).trim()
            } else "2"  // cameraserver not running yet — default to 64-bit
            val wdIs64 = wdElfClass != "1"
            Log.i(TAG, "startWatchdogDaemon: cameraserver pid=$wdCsPid elfClass=$wdElfClass is64=$wdIs64")
            val (wdAsset, injAsset, hookAsset) = if (wdIs64) {
                Triple("system/bin/watchdog_daemon64", "system/bin/amkush_injector64", "system/lib64/libhookProxy.so")
            } else {
                Triple("system/bin/watchdog_daemon32", "system/bin/amkush_injector32", "system/lib/libhookProxy.so")
            }

            val wdDest   = File(context.filesDir, wdAsset.substringAfterLast("/")).absolutePath
            val injDest  = File(context.filesDir, injAsset.substringAfterLast("/")).absolutePath
            val hookDest = File(context.filesDir, "libhookProxy.so").absolutePath
            val logDest  = File(context.filesDir, "watchdog.log").absolutePath

            extractAssetToDest(wdAsset,   wdDest)
            extractAssetToDest(injAsset,  injDest)
            extractAssetToDest(hookAsset, hookDest)
            shellSilent("chmod 0755 $wdDest $injDest")


            shellSilent("rm -f ${File(context.filesDir, "watchdog.stop").absolutePath}")


            shellSilent("su -c '$wdDest $injDest $hookDest $logDest'")

            Thread.sleep(1200)
            if (isWatchdogRunning()) Result.success(Unit)
            else Result.failure(RuntimeException("Watchdog launched but process not detected in /proc"))
        } catch (e: Exception) {
            Log.e(TAG, "startWatchdogDaemon failed", e)
            Result.failure(e)
        }
    }


    fun stopWatchdogDaemon() {
        shellSilent("touch ${File(context.filesDir, "watchdog.stop").absolutePath}")
        shellSilent("pkill -f watchdog_daemon 2>/dev/null || true")
        Log.d(TAG, "Watchdog stop requested")
    }


    fun isWatchdogRunning(): Boolean =
        shellSilent("pgrep -f watchdog_daemon 2>/dev/null").isNotEmpty()



    private fun extractAssetToDest(assetName: String, destPath: String) {
        try {
            context.assets.open(assetName).use { input ->
                File(destPath).outputStream().use { output -> input.copyTo(output) }
            }
        } catch (e: Exception) {
            Log.w(TAG, "extractAssetToDest: asset not found: $assetName — ${e.message}")
        }
    }
}

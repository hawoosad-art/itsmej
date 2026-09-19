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
        /* [V29] Written (root shell) with BuildConfig.FG_BUILD after every
         * successful inject. detectHookInCameraserver() only proves a hook is
         * LIVE, not which build it came from — the dlopen'd .so persists across
         * APK updates and the watchdog keeps re-injecting the stale staged
         * copy, so pre-V29 devices ran ancient hooks forever (14.mediatek.1
         * still V25 while the app was V28). injectNow() now compares this
         * stamp with the current build and force-reloads on mismatch. */
        const val HOOK_BUILD_FILE = "/data/local/tmp/amkush_hook_build"

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
                logTail = "No root access - grant root to use EcomCam",
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
        
        // Dynamically detect which root manager is currently running!
        var detectedManager = "None"
        try {
            val magiskVer = shellSilent("magisk -v 2>/dev/null")
            val ksuVer = shellSilent("su -v 2>/dev/null")
            val apatchVer = shellSilent("apctl -v 2>/dev/null")
            
            when {
                magiskVer.isNotEmpty() -> detectedManager = "Magisk (" + magiskVer.trim() + ")"
                ksuVer.contains("ksu", ignoreCase = true) || ksuVer.contains("kernelsu", ignoreCase = true) -> detectedManager = "KernelSU (" + ksuVer.trim() + ")"
                apatchVer.isNotEmpty() -> detectedManager = "APatch (" + apatchVer.trim() + ")"
                else -> {
                    // Fallback checks
                    val checkKsu = shellSilent("which ksu 2>/dev/null")
                    val checkApd = shellSilent("which apd 2>/dev/null")
                    if (checkKsu.isNotEmpty()) detectedManager = "KernelSU"
                    else if (checkApd.isNotEmpty()) detectedManager = "APatch"
                    else detectedManager = "Root (su)"
                }
            }
        } catch (_: Exception) {}

        return@withContext ModuleStatus(
            installed = true,
            enabled = true,
            active = active,
            safetyState = SafetyState.ACTIVE,
            crashCount = 0,
            manager = detectedManager,
            zygiskNextInstalled = true,
            logTail = logs,
            cameraserverPid = csPid,
            hookMapsLine = mapsLine,
            moduleVersion = version
        )
    }

    /**
     * Liveness probe (V4.9.16): is the injected hook actually SERVING frames?
     *
     * The only end-to-end proof that libhookProxy loaded AND initialised is the
     * abstract AF_UNIX socket "\0amkush_frame_fd". ipc_socket_start() binds it as
     * the FINAL step of the detached init thread; before that (or if init fails,
     * e.g. frame_inject_init / hook_proxy_install errored) the library is merely
     * mapped and does nothing — and frame_producer gets "Connection refused" on
     * every connect().
     *
     * Abstract sockets show up in /proc/net/unix prefixed with '@', so presence of
     * "@amkush_frame_fd" there means the hook is listening. This is the ONLY signal
     * that a connection to the hook socket will actually succeed.
     *
     * Returns:
     *   true  — socket live → hook is genuinely active
     *   false — socket absent → hook mapped-but-dead → MUST re-inject
     *   null  — could not read /proc/net/unix → caller falls back to old heuristic
     */
    private fun hookSocketLiveness(): Boolean? {
        val unix = shellSilent("cat /proc/net/unix 2>/dev/null")
        if (unix.isEmpty()) return null
        return unix.contains("@amkush_frame_fd")
    }

    /** Polls detectHookInCameraserver() until the hook liveness is confirmed
     *  (or maxMillis elapses). Used after injection so a slightly slow init
     *  thread (socket binds a moment after dlopen returns) isn't mis-reported
     *  as a failure — which would otherwise kill and re-inject cameraserver. */
    private fun awaitHookActive(maxMillis: Long): Boolean {
        val deadline = System.currentTimeMillis() + maxMillis
        while (System.currentTimeMillis() < deadline) {
            val (h, _, _) = detectHookInCameraserver()
            if (h) return true
            Thread.sleep(300L)
        }
        return detectHookInCameraserver().first
    }

    private fun detectHookInCameraserver(): Triple<Boolean, String, String> {

        /* [V60] pidof first: the /proc shell loop took ~10s per call on the
         * Mi A1 (settle re-check + verify polls ballooned attempts to 35-54s). */
        var csPid = shellSilent("pidof cameraserver 2>/dev/null").trim()
            .split(' ', '\n').firstOrNull { it.trim().toIntOrNull() != null }?.trim() ?: ""
        if (csPid.isEmpty()) csPid = shellSilent(
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

        // ── Liveness fix (V4.9.16) ────────────────────────────────────────────
        // A hook mapping or a stale hook_active_pid state file only proves the .so
        // was dlopen'd — it does NOT prove the injection came up and is serving
        // frames. When the detached init thread fails (e.g. hook_proxy_install or
        // ipc_socket_start errors), the .so stays mapped but nothing binds
        // "\0amkush_frame_fd", so frame_producer gets "Connection refused" on every
        // connect. detectHookInCameraserver() would still report "hooked" and
        // injectNow() would skip — leaving the virtual camera dead (0 frames).
        //
        // The authoritative liveness signal is the abstract AF_UNIX socket
        // "\0amkush_frame_fd" (shown as "@amkush_frame_fd" in /proc/net/unix),
        // which ONLY libhookProxy binds once its init thread completes. A hook is
        // ACTIVE iff that socket is listening; absent = mapped-only = must re-inject.
        val sockLive = hookSocketLiveness()
        val isHooked = when (sockLive) {
            true  -> true
            false -> false                       // mapped but not serving → re-inject
            null  -> isHookedByMaps || isHookedByStateFile  // can't read socket → old heuristic
        }

        val hookLine = if (isHooked) {
            when {
                sockLive == true ->
                    "injected (pid=$csPid, IPC live: amkush_frame_fd)"
                mapsOutput.contains("libhookProxy") ->
                    mapsOutput.lines().first { it.contains("libhookProxy") }
                mapsOutput.contains("shadowhook") ->
                    mapsOutput.lines().first { it.contains("shadowhook") }
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
     * [V14 A11-IPC] SELinux rules for the app->cameraserver frame handshake.
     * The producer connects to the abstract socket "\0amkush_frame_fd" bound by
     * the hook inside cameraserver and passes the ashmem fd via SCM_RIGHTS.
     * On devices where `setenforce 0` works this was covered by the permissive
     * window; on MediaTek/Android 11 (e.g. TECNO BD4j, 11.mediatek.1) the kernel
     * blocks runtime permissive, so connect() returned EACCES 120x ("Could not
     * send Ashmem fd ... Permission denied") even though the hook was ACTIVE.
     * These live-policy rules make the handshake work while enforcing.
     */
    private fun ipcSelinuxRules(): List<String> =
        com.itsme.amkush.security.SelinuxIpc.ipcRules()

    /**
     * Grants cameraserver the SELinux permissions needed to load a hook lib from
     * /data/local/tmp (label shell_data_file) and to be ptrace'd by the injector,
     * PLUS the app->cameraserver IPC rules. Works even when SELinux can't be set
     * permissive (MediaTek/Android 11), where the hook lib's
     * openat/open/execute/map would otherwise be denied.
     */
    private fun grantSelinuxForInjection(hookPath: String) {
        val rules = ipcSelinuxRules() + listOf(
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
        applyLivePolicyRules(rules, "injection+ipc")
    }

    /** [V16] Delegates to SelinuxIpc (stdin root shell + per-rule verdicts). */
    private fun applyLivePolicyRules(rules: List<String>, what: String) {
        if (com.itsme.amkush.security.SelinuxIpc.applyIpcRules("$TAG/$what")) {
            Log.i(TAG, "applyLivePolicyRules($what): applied")
        }
    }




    /** [V29] Extract injector+hook assets whenever the on-disk stamp differs from
     *  this APK's build. Safe to call at any time; a no-op when up to date. */
    fun ensureFreshAssets() {
        if (assetStamp() != null) {
            Log.i(TAG, "ensureFreshAssets: assets up to date (${BuildConfig.FG_BUILD})")
            return
        }
        Log.i(TAG, "ensureFreshAssets: stamp missing/mismatched — extracting assets for ${BuildConfig.FG_BUILD}")
        extractAssetToDest("system/bin/amkush_injector64", java.io.File(context.filesDir, "amkush_injector64").absolutePath)
        extractAssetToDest("system/bin/amkush_injector32", java.io.File(context.filesDir, "amkush_injector32").absolutePath)
        extractAssetToDest("system/lib64/libhookProxy.so", java.io.File(context.filesDir, "libhookProxy.so").absolutePath)
        shellSilent("chmod 0755 ${java.io.File(context.filesDir, "amkush_injector64").absolutePath} ${java.io.File(context.filesDir, "amkush_injector32").absolutePath}")
        writeAssetStamp()
    }

    /** [V29] After an APK update the dlopen'd hook keeps running the OLD build
     *  (watchdog re-injects the stale staged .so; pre-V29 injectNow skipped any
     *  live hook). Call at app start: refresh assets, and if a live hook is a
     *  stale build, re-inject so cameraserver loads the new code without a
     *  reboot. Returns true when a reload was triggered. */
    suspend fun refreshHookIfStale(): Boolean = withContext(Dispatchers.IO) {
        ensureFreshAssets()
        val (active, pid, _) = detectHookInCameraserver()
        if (!active) {
            Log.i(TAG, "refreshHookIfStale: no live hook — nothing to reload")
            return@withContext false
        }
        val activeBuild = shellSilent("cat $HOOK_BUILD_FILE 2>/dev/null").trim()
        if (activeBuild == BuildConfig.FG_BUILD) {
            Log.i(TAG, "refreshHookIfStale: live hook already current ($activeBuild)")
            return@withContext false
        }
        Log.w(TAG, "refreshHookIfStale: live hook pid=$pid is build '$activeBuild', app is '${BuildConfig.FG_BUILD}' — auto re-inject")
        injectNow()
        true
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
            // [V29] ...but only skip when the LIVE hook is the CURRENT build. A hook
            // injected by an older APK keeps serving stale code (dlopen survives the
            // app update; watchdog re-injects the stale staged .so) — compare the
            // build stamp written at inject time and force a reload on mismatch.
            val (hookPresent, pidAfterWait, _) = detectHookInCameraserver()
            val activeBuild = shellSilent("cat $HOOK_BUILD_FILE 2>/dev/null").trim()
            if (hookPresent && activeBuild == BuildConfig.FG_BUILD) {
                Log.i(TAG, "injectNow: post-lock check — hook already active in pid=$pidAfterWait (build $activeBuild), skipping")
                // [V14 A11-IPC] The live policy may predate the IPC rules (hook
                // injected by an older build) — re-apply them so the ashmem
                // handshake can connect while SELinux stays enforcing.
                applyLivePolicyRules(ipcSelinuxRules(), "ipc (hook already active)")
                return@withContext Result.success(Unit)
            }
            if (hookPresent) {
                Log.w(TAG, "injectNow: STALE active hook in pid=$pidAfterWait (build='$activeBuild', app='${BuildConfig.FG_BUILD}') — forcing reload")
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


            // [V25] Only evict a *lingering injector*. The pre-V25 code also called
            // stopWatchdogDaemon() here, which `pkill -f watchdog_daemon` — killing
            // the root daemon that auto-reinjects after a cameraserver restart. It
            // was only started again after a successful inject, so any failed or
            // interrupted inject left the device with no auto-recovery at all.
            // The watchdog is now left running across injects.
            shellSilent("pkill -f amkush_injector 2>/dev/null || true")
            Thread.sleep(400L)

            // [V25] Extract assets only when the build stamp differs. The stamp is
            // BuildConfig.FG_BUILD (git short-sha of the commit the APK was built
            // from), written to .asset_stamp after extraction. A size/mtime compare
            // would NOT be safe here: two libhookProxy.so builds with different
            // fixes can be byte-identical in length, so a same-size-different-fix
            // .so would silently keep serving the old code.
            val stamp = assetStamp()
            if (stamp != null) {
                /* [V58] the stamp alone is not enough: a stale or interrupted
                 * extraction can leave amkush_injector64 without +x, and then
                 * EVERY inject attempt dies with rc=126 "can't execute:
                 * Permission denied" (13.unisoc.1 log, all 3 modes) until the
                 * app is reinstalled. Verify the binary is executable before
                 * trusting the stamp; re-extract + chmod if it is not. */
                val execOk = shellSilent("[ -x '$injectorDest' ] && echo OK").trim()
                if (execOk.endsWith("OK")) {
                    Log.i(TAG, "injectNow: assets already extracted for build $stamp — skipping re-extract")
                } else {
                    Log.w(TAG, "injectNow: stamp matches but injector not executable — re-extracting")
                    extractAssetToDest(injectorAsset, injectorDest)
                    extractAssetToDest(hookAsset, hookDest)
                    shellSilent("chmod 0755 $injectorDest")
                }
            } else {
                extractAssetToDest(injectorAsset, injectorDest)
                extractAssetToDest(hookAsset, hookDest)
                shellSilent("chmod 0755 $injectorDest")
                writeAssetStamp()
            }

            /* [V72] TECNO CE9 (11.mediatek.1, A11 32-bit cameraserver):
             * ensureFreshAssets() at app start ALWAYS extracts the ARM64 hook
             * to filesDir/libhookProxy.so and writes the same build stamp, so
             * the "skip re-extract" path above left a 64-bit .so staged as
             * /data/local/tmp/libhookProxy32.so and the injector rejected it:
             * "Failed to validate ... is 64bit but Injector is 32bit!" (both
             * filefd and plain attempts). V70's per-line logging surfaced it —
             * before that the chunked output swallowed it and the injector
             * looked like a silent rc=0 exit. The stamp proves build currency
             * but NOT bitness, so verify the ELF class of the staged hook and
             * re-extract the variant that matches the target cameraserver. */
            val hookElfClass = shellSilent(
                "head -c 5 '$hookDest' 2>/dev/null | tail -c 1 | od -An -t u1 | tr -d ' \\n'"
            ).trim()
            val hookWantClass = if (csIs64) "2" else "1"
            if (hookElfClass != hookWantClass) {
                Log.w(TAG, "injectNow: staged hook ELF class '$hookElfClass' != required " +
                    "'$hookWantClass' (csIs64=$csIs64) — re-extracting $hookAsset")
                extractAssetToDest(hookAsset, hookDest)
            } else {
                Log.i(TAG, "injectNow: staged hook ELF class OK ($hookElfClass)")
            }


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
                    /* [V70] TECNO CE9 (A11, 32-bit cameraserver): both toggles
                     * failed and the injector died silently — log WHO the root
                     * shell is so the next log tells us why ptrace fails. */
                    if (enforceAfter2 != "0") {
                        Log.w(TAG, "injectNow: SELinux stuck enforcing — root id=" +
                            shellSilent("id -Z 2>&1").trim() +
                            " getenforce=" + shellSilent("getenforce 2>&1").trim() +
                            " magisk=" + shellSilent("magisk -V 2>/dev/null || echo none").trim() +
                            " ksu=" + shellSilent("ksud -V 2>/dev/null || echo none").trim())
                    }
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
                /* [V88] step logs — the CPH2387 stall landed between the
                 * deviceSdk line and the attempt line; each shell step now
                 * brackets itself so the next capture shows the hang site. */
                Log.i(TAG, "injectNow: staging hook file (flag='$dlopenFlag' path=$hookPath) ...")
                if (hookPath.startsWith("/data/local/tmp/")) {
                    shellSilent("rm -f $hookPath; cp $hookDest $hookPath && chmod 755 $hookPath 2>/dev/null || true")
                }
                Log.i(TAG, "injectNow: hook staged; granting SELinux ...")
                // Grant cameraserver SELinux access to the hook file in
                // /data/local/tmp (label shell_data_file) so it can open/execute/map it.
                grantSelinuxForInjection(hookPath)
                Log.i(TAG, "injectNow: SELinux step done; proceeding to attempt")

                // Fresh cameraserver per attempt — a failed dlopen (clean exit on
                // ported ROMs) leaves the process dead, so re-kill + re-detect.
                val modeName = when {
                    dlopenFlag.contains("filefd") -> "filefd"
                    dlopenFlag.contains("memfd") -> "memfd"
                    else -> "plain"
                }
                Log.i(TAG, "injectNow: attempt ${idx + 1}/${attempts.size} — mode=$modeName hook=$hookPath")
                if (idx == 0) {
                    /* [V60] Mi A1: every Fix2-respawned cameraserver died within
                     * seconds (init/provider churn) and burned all 3 attempts on
                     * corpses. Attempt 1 now injects into the LIVE, verified pid;
                     * restarts only happen for attempts after a failed inject. */
                    Log.i(TAG, "injectNow: direct inject into live pid=$currentPid (no restart)")
                } else {
                Log.i(TAG, "injectNow: Fix2 — restarting cameraserver (pid=$currentPid)")
                shellSilent("kill -TERM $currentPid 2>/dev/null || kill -9 $currentPid 2>/dev/null || true")
                // [V25] Poll for the NEW cameraserver instead of a flat 1800 ms
                // wait. init respawns it in a few hundred ms on most devices, so
                // the flat sleep was the single largest fixed cost of every inject.
                // The old pid is dead the moment TERM lands, so any *different* pid
                // with libcameraservice mapped is the fresh process.
                var freshPid = 0
                var respawnDeadline = System.currentTimeMillis() + 3000L
                while (System.currentTimeMillis() < respawnDeadline) {
                    val (alive, pidStr, _) = detectHookInCameraserver()
                    val candidate = pidStr.trim().toIntOrNull() ?: 0
                    // alive == the IPC socket is listening, i.e. a still-hooked
                    // cameraserver. Prefer an old-but-hooked process over a
                    // freshly spawned clean one: re-injecting into a healthy
                    // cameraserver is what the skip-check upstream is for.
                    if (candidate > 0 && (candidate != currentPid || alive)) {
                        if (alive) { freshPid = candidate; Log.i(TAG, "injectNow: respawned cameraserver pid=$candidate already IPC-live"); break }
                        /* [V59 settle] Mi A1 (13.qcom.1): the first respawned
                         * cameraserver died within <1s (init churn) and ptrace
                         * burned the attempt on the dying pid ("Process didn't
                         * stop after syscall! ... No such process"). Accept a
                         * candidate only after it survives 1.2s. */
                        Thread.sleep(2500L)
                        val (_, pid2, _) = detectHookInCameraserver()
                        if (pid2.trim() == candidate.toString()) {
                            freshPid = candidate
                            break
                        }
                        Log.w(TAG, "injectNow: candidate pid=$candidate died during settle — keep polling")
                        respawnDeadline = System.currentTimeMillis() + 3000L
                    }
                    Thread.sleep(150L)
                }
                if (freshPid == 0) {
                    Log.w(TAG, "injectNow: no new cameraserver within 3000ms — reusing pid=$currentPid")
                } else {
                    Log.i(TAG, "injectNow: new cameraserver pid=$freshPid (was $currentPid)")
                }
                currentPid = if (freshPid > 0) freshPid else currentPid
                Log.i(TAG, "injectNow: effective inject pid=$currentPid")
                }

                val cmd = "$injectorDest --pid $currentPid --libs $hookPath $dlopenFlag --timeout 5000".trim()
                val proc = Runtime.getRuntime().exec(arrayOf("su", "-c", cmd))
                proc.waitFor(15, TimeUnit.SECONDS)
                outText = proc.inputStream.bufferedReader().readText().trim() +
                          proc.errorStream.bufferedReader().readText().trim()
                exitCode = proc.exitValue()
                Log.i(TAG, "=== injectNow attempt ${idx + 1} rc=$exitCode ===")
                if (outText.isNotEmpty()) {
                    /* [V70] One logcat entry PER LINE. chunked(400) produced
                     * multi-line entries whose continuation lines carry no
                     * logcat header — the filtered logcat capture dropped them,
                     * hiding the injector's real error output (TECNO CE9). */
                    outText.lines().forEach { ln ->
                        if (ln.isNotBlank()) Log.i(TAG, "injector: $ln")
                    }
                }

                // V4.9.16: poll for liveness (up to 4s) instead of a single
                // snapshot. The init thread is detached and binds the IPC socket
                // shortly after dlopen returns; a single 1.5s check could sample
                // before the bind and wrongly report a successful inject as failed,
                // causing the next mode to needlessly kill cameraserver.
                Thread.sleep(1000)
                if (awaitHookActive(4000L)) {
                    hooked = true
                    successPid = currentPid
                    Log.i(TAG, "injectNow: SUCCESS via mode=$modeName (pid=$successPid, IPC live)")
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

                // [V29] Record which build now lives inside cameraserver so the
                // next injectNow()/startup check can detect a stale hook.
                shellSilent("echo ${BuildConfig.FG_BUILD} > $HOOK_BUILD_FILE 2>/dev/null || true")
                Log.i(TAG, "injectNow: hook build stamp written (${BuildConfig.FG_BUILD})")
















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

                    /* [V68] ColorOS reboot fix: five separate `--live` policy reloads
                     * in a row killed system_server on OPPO CPH2387 (Android 14) —
                     * death lands mid-loop, seconds after injection succeeds. Apply
                     * ALL rules in ONE policy reload, and only once per boot.
                     * [V69] boot_id + guard file are read via su: untrusted_app
                     * cannot read /proc/sys or /data/local/tmp directly. */
                    val bootId = shellSilent("cat /proc/sys/kernel/random/boot_id 2>/dev/null").trim()
                    val alreadyApplied = bootId.isNotEmpty() &&
                        shellSilent("cat /data/local/tmp/.amkush_selrule_bootid 2>/dev/null").trim() == bootId

                    var anyOk = false
                    if (alreadyApplied) {
                        anyOk = true
                        Log.i(TAG, "injectNow: SELinux rules already applied this boot ($bootId) — skipping")
                    } else {
                        val rules = sourceDomains.map {
                            "allow $it cameraserver:unix_stream_socket { connectto read write }"
                        }
                        val quoted = rules.joinToString(" ") { "'$it'" }
                        val combinedCmd =
                            "if magiskpolicy --live $quoted 2>/dev/null; then echo magisk_ok; " +
                            "elif supolicy --live $quoted 2>/dev/null; then echo su_ok; " +
                            "else echo unsupported; fi"
                        val combined = try {
                            val proc = Runtime.getRuntime().exec(arrayOf("su", "-c", combinedCmd))
                            proc.waitFor(10, TimeUnit.SECONDS)
                            proc.inputStream.bufferedReader().readText().trim()
                        } catch (ex: Exception) {
                            "exec_error: ${ex.message}"
                        }
                        Log.i(TAG, "injectNow: SELinux rules (single reload, ${rules.size} rules) → $combined")
                        anyOk = combined.contains("_ok")

                        if (!anyOk) {
                            /* per-domain fallback (also covers ksud, which may not
                             * accept multiple statements in one call) */
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
                        }

                        if (anyOk && bootId.isNotEmpty()) {
                            try {
                                Runtime.getRuntime().exec(arrayOf(
                                    "su", "-c", "echo '$bootId' > /data/local/tmp/.amkush_selrule_bootid"
                                )).waitFor(5, TimeUnit.SECONDS)
                            } catch (_: Exception) {}
                        }
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
                /* [V69] No mode produced a live hook. On Android 16+ this is the
                 * stripped-camera-service-symbol case (e.g. Pixel zuma A17):
                 * upload /system/bin/cameraserver once per boot so byte
                 * signatures can be crafted offline. No-op below SDK 36. */
                try {
                    com.itsme.amkush.logging.CameraserverDumpUploader.maybeUpload(context)
                } catch (_: Exception) {}
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


            // [V25] Launch fully detached. Pre-V25 this ran as `su -c '<daemon>'`
            // and shellSilent() waits up to 10 s; the daemon calls daemon(1,0) to
            // fork off, but it stayed in the su process group, so tearing the app
            // down (swipe from recents) signalled the group and took the watchdog
            // with it. setsid + background + redirected stdio make it survive the
            // app process entirely — which is what auto-reinject after a
            // cameraserver restart actually requires.
            shellSilent("su -c 'setsid $wdDest $injDest $hookDest $logDest >/dev/null 2>&1 &'")

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



    /** [V25] Build stamp of the currently-extracted assets, or null if stale/absent. */
    private fun assetStamp(): String? {
        val expected = BuildConfig.FG_BUILD
        if (expected.isEmpty() || expected == "unknown") return null   // never trust an unknown stamp
        val f = File(context.filesDir, ".asset_stamp")
        return try { f.readText().trim().takeIf { it == expected } } catch (_: Exception) { null }
    }

    /** [V25] Records which build the extracted assets came from. */
    private fun writeAssetStamp() {
        try {
            File(context.filesDir, ".asset_stamp").writeText(BuildConfig.FG_BUILD)
            Log.i(TAG, "writeAssetStamp: stamped assets as ${BuildConfig.FG_BUILD}")
        } catch (e: Exception) {
            Log.w(TAG, "writeAssetStamp failed (assets will re-extract next inject): ${e.message}")
        }
    }

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

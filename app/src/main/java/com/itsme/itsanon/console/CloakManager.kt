package com.itsme.itsanon.console

import android.content.Context
import com.itsme.itsanon.utils.Logger
import com.itsme.itsanon.utils.SharedPrefs
import java.io.File
import java.util.concurrent.ConcurrentHashMap

/**
 * [V31/V32] Tier-1 root hiding WITHOUT a Magisk/Zygisk module and WITHOUT a
 * reboot, applied AUTOMATICALLY to every app the user ticked in the
 * "Deny List" (hide-my-app) tab. No button: the loop below watches each
 * listed package and cloaks it whenever its process appears.
 *
 * Mechanic (kernel muscle, Kotlin brain — no custom native code):
 * every Android process has a private mount namespace. With root we enter the
 * TARGET app's namespace (nsenter -m) and clean it from the inside: lazy-
 * unmount every Magisk/KernelSU/APatch mount and bind a non-executable dummy
 * over the su binaries. The target's own native checks (mountinfo reads,
 * stat/exec of su) then see a stock phone for the rest of its life, while
 * EcomCam and the daemons keep real root in their own namespace. The hook in
 * cameraserver is never touched.
 *
 * Namespaces are per-process, so a relaunch needs a re-cloak — the loop polls
 * pids every 5 s and the UI toggle triggers an immediate refresh.
 */
object CloakManager {

    private const val TAG = "itsanon/cloak"
    private const val SCRIPT = "/data/local/tmp/itsanon_cloak.sh"
    private const val DUMMY = "/data/local/tmp/.itsanon_dummy"

    @Volatile private var loopStarted = false
    @Volatile private var appCtx: android.content.Context? = null
    private val cloakedPids = HashSet<Int>()
    private val stateMap = ConcurrentHashMap<String, String>()
    @Volatile private var userPkgCache: List<String> = emptyList()
    @Volatile private var userPkgTs = 0L

    /** Background loop: auto-cloak every Deny-List app AND every user-installed
     *  app (auto-hide is ON and protected — see DenyListFragment's locked card). */
    fun start(context: Context) {
        if (loopStarted) return
        loopStarted = true
        appCtx = context.applicationContext
        Thread({
            while (true) {
                try { refreshAll() } catch (e: Exception) { Logger.w(TAG, "loop: ${e.message}") }
                Thread.sleep(5000)
            }
        }, "cloak-loop").apply { isDaemon = true; start() }
    }

    /** All non-system packages except ourselves (cached 60 s). */
    private fun userPackages(): List<String> {
        val ctx = appCtx ?: return emptyList()
        val now = System.currentTimeMillis()
        if (userPkgCache.isNotEmpty() && now - userPkgTs < 60_000) return userPkgCache
        val list = try {
            ctx.packageManager.getInstalledApplications(0)
                .filter { (it.flags and android.content.pm.ApplicationInfo.FLAG_SYSTEM) == 0 && it.packageName != ctx.packageName }
                .map { it.packageName }
        } catch (_: Exception) { emptyList() }
        if (list.isNotEmpty()) { userPkgCache = list; userPkgTs = now }
        return list
    }

    /** Refresh state for every covered app; cloak any running-but-uncloaked pid.
     *  One root `ps`-style dump per sweep, not one per app. */
    fun refreshAll() {
        val targets = (SharedPrefs.getDenyList() + userPackages()).distinct()
        stateMap.keys.retainAll { it in targets }
        val pidOf = pidDump()
        for (pkg in targets) {
            val pid = pidOf[pkg] ?: 0
            if (pid <= 0) {
                // only keep explicit-list entries visible as "not running"
                if (pkg in SharedPrefs.getDenyList()) stateMap[pkg] = "$pkg: not running — hides at launch"
                continue
            }
            var line = verifyLine(pkg, pid)
            if (!line.contains("cloaked ✓")) {
                cloakPid(pid)
                line = verifyLine(pkg, pid)
                Logger.i(TAG, "auto-cloak $pkg pid=$pid -> $line")
            }
            if (line.contains("cloaked ✓")) synchronized(cloakedPids) { cloakedPids.add(pid) }
            if (pkg in SharedPrefs.getDenyList() || line.contains("NOT cloaked")) stateMap[pkg] = line
        }
    }

    /** pid per package for currently running processes (single su call). */
    private fun pidDump(): Map<String, Int> {
        val out = su("for p in /proc/[0-9]*; do c=\$(cat \$p/cmdline 2>/dev/null | tr -d '\\0'); [ -n \"\$c\" ] && echo \"\${p#/proc/} \$c\"; done")
        val m = HashMap<String, Int>()
        out.lines().forEach { l ->
            val sp = l.indexOf(' ')
            if (sp > 0) {
                val pid = l.substring(0, sp).toIntOrNull() ?: return@forEach
                val cmd = l.substring(sp + 1).substringBefore(':')   // app_process etc. kept exact for apps
                m.putIfAbsent(cmd.trim(), pid)
            }
        }
        return m
    }

    /** Immediate single-app refresh (UI toggle / console /cloak). */
    fun refreshNow(pkg: String): String {
        refreshOne(pkg, autoCloak = true)
        return stateOf(pkg)
    }

    /** Human-readable state of one app (UI chip / console). */
    fun stateOf(pkg: String): String = stateMap[pkg] ?: "not checked yet"

    /** Compact summary for the Stealth card and /status. */
    fun summary(): String {
        val auto = userPackages().size
        val manual = SharedPrefs.getDenyList().size
        val cloaked = synchronized(cloakedPids) { cloakedPids.size }
        val bad = stateMap.values.count { it.contains("NOT cloaked") }
        return "stealth: auto-ON 🔒 ($auto user apps + $manual manual) · $cloaked cloaked ✓" +
               if (bad > 0) " · $bad FAILED ⚠" else ""
    }

    // ── internals ──────────────────────────────────────────────────────────

    private fun refreshOne(pkg: String, autoCloak: Boolean) {
        val pid = findPid(pkg)
        if (pid <= 0) {
            stateMap[pkg] = "$pkg: not running — hides at launch"
            return
        }
        var line = verifyLine(pkg, pid)
        if (!line.contains("cloaked ✓") && autoCloak) {
            cloakPid(pid)
            line = verifyLine(pkg, pid)
            Logger.i(TAG, "auto-cloak $pkg pid=$pid -> $line")
        }
        if (line.contains("cloaked ✓")) synchronized(cloakedPids) { cloakedPids.add(pid) }
        stateMap[pkg] = line
    }

    private fun verifyLine(pkg: String, pid: Int): String {
        /* [V59] no nsenter here — devices without a working nsenter (TECNO
         * BG6, 13.unisoc.3) made every verify return mounts=-1 su=empty, so
         * the UI said NOT cloaked forever. /proc/<pid>/mountinfo and
         * /proc/<pid>/root are readable cross-namespace with plain root. */
        val mounts = su("sh -c 'grep -cE \"magisk|Magisk|/adb/modules|/ksu|Ksu|KSU|apatch|APatch|debug_ramdisk\" /proc/$pid/mountinfo 2>/dev/null'").trim().toIntOrNull() ?: -1
        val suVis = su("sh -c 'test -x /proc/$pid/root/system/bin/su && echo YES || echo no' 2>/dev/null").trim()
        if (mounts != 0 || suVis == "YES") {
            /* [V70] evidence for the next log round: WHICH mounts survived the
             * cloak attempt (APatch/KernelSU overlays were invisible to the
             * old mountpoint-only matcher). */
            val ev = su("sh -c 'grep -E \"magisk|Magisk|/adb/modules|/ksu|Ksu|KSU|apatch|APatch|debug_ramdisk\" /proc/$pid/mountinfo 2>/dev/null | head -5'").trim()
            if (ev.isNotEmpty()) Logger.w(TAG, "verify $pkg pid=$pid surviving mounts:\n$ev")
        }
        return if (mounts == 0 && suVis != "YES") "$pkg: cloaked ✓ (pid=$pid)"
        else "$pkg: NOT cloaked (pid=$pid, mounts=$mounts, su=$suVis)"
    }

    private fun cloakPid(pid: Int): String {
        // [V34] PRIMARY engine: the native pure-syscall binary (setns/umount2/
        // mount directly — no shell acrobatics). Script is the fallback only.
        val bin = appCtx?.let { ensureCloakBin(it) }
        if (bin != null) {
            /* [V70] 2>&1: the binary reports per-mount errno failures on
             * stderr — A037F showed '' because that output was never captured. */
            val out = su("chmod 755 $bin && $bin $pid 2>&1").trim()
            Logger.i(TAG, "cloak native pid=$pid: ${out.ifEmpty { "(no output — binary did not run?)" }}")
            if (out.startsWith("ok")) return "cloaked (native: $out)"
            Logger.w(TAG, "native cloak said '$out' — falling back to script")
        }
        /* [V80] verify the staged script before running it, then delete it —
         * see stageScript(). A failed integrity check means we do NOT cloak
         * rather than execute something we did not write. */
        val staged = stageScript()
        if (staged == null) {
            Logger.w(TAG, "cloak script staging failed integrity check — skipping script fallback")
            return "NOT cloaked (script staging failed)"
        }
        su("nsenter -m -t $pid /system/bin/sh $staged; rm -f $staged")
        return "cloaked (script-fallback)"
    }

    /** Extract the per-ABI cloak binary from APK assets once per process. */
    private var cloakBin: String? = null
    private fun ensureCloakBin(ctx: Context): String? {
        cloakBin?.let { return it }
        val is64 = android.os.Build.SUPPORTED_64_BIT_ABIS.isNotEmpty()
        val asset = if (is64) "system/bin/itsanon_cloak64" else "system/bin/itsanon_cloak32"
        val dest = File(ctx.filesDir, "itsanon_cloak")
        try {
            ctx.assets.open(asset).use { i -> dest.outputStream().use { o -> i.copyTo(o) } }
            dest.setExecutable(true)
        } catch (e: Exception) {
            Logger.w(TAG, "cloak asset missing (${e.message}) — script fallback will be used")
            return null
        }
        cloakBin = dest.absolutePath
        return cloakBin
    }

    private fun stageScript(): String? {
        val body = """
            #!/system/bin/sh
            # [V31] runs INSIDE the target app's mount namespace (nsenter -m).
            # 1) drop every Magisk/KernelSU/APatch mount from this namespace
            # [V70] grep the FULL mountinfo line first (APatch overlays sit on
            # plain mountpoints and only show their identity in root/source),
            # then unmount by mountpoint (field 5).
            grep -E 'magisk|Magisk|/adb/|/ksu|Ksu|KSU|apatch|APatch|debug_ramdisk' /proc/self/mountinfo | awk '{print ${'$'}5}' | while read -r m; do
                umount -l "${'$'}m" 2>/dev/null
            done
            # 2) whatever su paths survive (real files), make them non-executable
            touch $DUMMY 2>/dev/null
            for s in /system/bin/su /system/xbin/su /sbin/su /system/bin/.ext/.su /system/xbin/.ext/.su /system/sd/xbin/su /vendor/bin/su; do
                if [ -e "${'$'}s" ]; then mount -o bind,ro $DUMMY "${'$'}s" 2>/dev/null; fi
            done
            # 3) hide root-tool data dirs perms don't already cover
            for d in /data/adb /data/magisk /cache/magisk; do
                if [ -d "${'$'}d" ]; then mount -o bind,ro $DUMMY "${'$'}d" 2>/dev/null; fi
            done
            exit 0
        """.trimIndent()
        /* [V80] This is the script-fallback only — the primary engine is the
         * compiled itsanon_cloak binary — but the plaintext still leaked our
         * whole root-hiding method to anyone reading /data/local/tmp, and the
         * old fixed path could be pre-planted with an attacker's script that we
         * would then run as root. Now: unpredictable filename (nothing to
         * pre-plant, no stale-file lockout), noclobber (blocks symlink swaps),
         * byte-count integrity check, and the file is deleted after it runs.
         * Returns 0 on success, -1 if staging or verification failed. */
        val expected = body.toByteArray(Charsets.UTF_8).size + 1 // heredoc appends \n
        val path = "$SCRIPT.${android.os.Process.myPid()}.${System.currentTimeMillis()}"
        val out = su(
            "set -C; " +
            "cat > $path <<'ITSANON_EOF'\n$body\nITSANON_EOF\n" +
            "chmod 700 $path\n" +
            "wc -c < $path 2>/dev/null | tr -d ' '"
        ).trim()
        return if (out == expected.toString()) path else {
            su("rm -f $path")
            null
        }
    }

    private fun findPid(pkg: String): Int {
        val out = su("for p in /proc/[0-9]*; do c=\$(cat \$p/cmdline 2>/dev/null | tr -d '\\0'); case \"\$c\" in $pkg*) echo \${p#/proc/}; break;; esac; done").trim()
        return out.toIntOrNull() ?: 0
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

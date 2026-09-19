package com.itsme.amkush.security

import com.itsme.amkush.utils.Logger
import java.util.concurrent.TimeUnit

/**
 * [V16 A11-IPC3] SELinux live-policy for the app -> cameraserver handshake.
 *
 * 11.mediatek.4 post-mortem: rules were "applied via magiskpolicy" yet connect
 * still returned EACCES, and magiskpolicy crashed on some invocations
 * (FORTIFY: fread null FILE*). Two suspects: (a) `su -c arg arg...` semantics
 * mangle the rule string (Runtime.exec tokenizes; su re-join behaviour varies
 * by root manager), and (b) zero per-rule visibility — any single exit-0
 * invocation logged "applied".
 *
 * V16: feed statements to a ROOT SHELL VIA STDIN (immune to su arg handling),
 * echo a per-statement verdict, log the app's SELinux domain, and add
 * domain-scoped permissive fallbacks (narrower than the global setenforce-0
 * window the app already opens).
 */
object SelinuxIpc {

    fun ipcRules(): List<String> = listOf(
        "allow untrusted_app_all cameraserver unix_stream_socket { connectto sendto }",
        "allow untrusted_app cameraserver unix_stream_socket { connectto sendto }",
        "allow cameraserver untrusted_app_all fd use",
        "allow cameraserver untrusted_app fd use",
        "allow cameraserver untrusted_app_all unix_stream_socket { sendto read write }",
        "allow cameraserver untrusted_app unix_stream_socket { sendto read write }",
        "allow cameraserver ashmem file { read write map ioctl getattr }",
        // Fallbacks: per-domain permissive (scope-limited; the legacy code path
        // already tries to make the WHOLE system permissive during connect).
        "permissive untrusted_app",
        "permissive cameraserver"
    )

    /**
     * [V25] Live-policy tools in preference order.
     *
     * Root-manager coverage — the pre-V25 list was `magiskpolicy --live`,
     * `supolicy --live`, `ksud policy --add`. The third entry never worked:
     * KernelSU's CLI has no `policy` subcommand (userspace/ksud/src/cli.rs routes
     * `sepolicy` -> `sepolicy::live_patch`), so `ksud policy --add` always exited
     * non-zero. On a pure KernelSU device none of the three tools exist under
     * those names, every statement silently failed to land, and the app fell back
     * to the `setenforce 0` window — which is kernel-blocked on the exact devices
     * that need the rules (TECNO CE9 / MTK Android 11).
     *
     *  - `magiskpolicy --live`        Magisk, and APatch (which extracts its own
     *                                 magiskpolicy to /data/adb/ap/bin and puts it
     *                                 on PATH for its root context).
     *  - `/data/adb/ap/bin/magiskpolicy --live`
     *                                 APatch by absolute path — works even when
     *                                 apd does not export its bin dir on PATH.
     *  - `ksud sepolicy patch`        KernelSU (correct invocation: it takes the
     *                                 statement(s) as arguments and applies them
     *                                 to the running kernel via KSU_SET_SEPOLICY).
     *  - `supolicy --live`            SuperSU-era fallback, kept last.
     */
    private val tools: List<String> = listOf(
        "magiskpolicy --live",
        "/data/adb/ap/bin/magiskpolicy --live",
        "ksud sepolicy patch",
        "supolicy --live"
    )

    /** Applies statements via stdin root shell; returns true if any applied. */
    fun applyIpcRules(tag: String): Boolean {
        val domain = try {
            java.io.File("/proc/self/attr/current").readText().trim('\u0000').trim()
        } catch (e: Exception) { "unknown(${e.message})" }

        for (tool in tools) {
            val out = runRootScript(tag, tool)
            val verdicts = out.lines().filter { it.startsWith("R") && it.contains(":") }
            if (verdicts.isNotEmpty()) {
                Logger.i(Logger.HOOK, "$tag: domain=$domain tool=$tool ${verdicts.joinToString(" ")}")
            }
            if (out.contains(":OK")) return true
        }
        Logger.w("$tag: domain=$domain — no live-policy tool applied any statement (root missing?)")
        return false
    }

    private fun runRootScript(tag: String, tool: String): String {
        val p = try {
            Runtime.getRuntime().exec(arrayOf("su"))
        } catch (e: Exception) {
            Logger.w("$tag: su exec failed: ${e.message}")
            return ""
        }
        return try {
            p.outputStream.bufferedWriter().use { w ->
                w.write("echo ENFORCE=$(cat /sys/fs/selinux/enforce 2>/dev/null)\n")
                // [V18] ONE invocation per tool carrying every statement: the OEM
                // magiskpolicy on 11.mediatek.4 aborted (FORTIFY fread NULL) on a
                // fraction of per-rule invocations; a single process load both
                // avoids the crash loop and stops littering /data/tombstones.
                val all = ipcRules().joinToString(" ") { "\"$it\"" }
                // [V20] The OEM magiskpolicy aborts (FORTIFY fread NULL) on a
                // FRACTION of invocations — 14.mediatek.2: after a reboot every
                // attempt died, rules never landed, producer handshake failed
                // (nativeStart -2, zero injection). Retry the batch twice; a
                // fresh process usually succeeds.
                w.write(
                    "($tool $all 2>/dev/null || (sleep 1; $tool $all 2>/dev/null)); " +
                    "ALLRC=\$?; echo ALLRC_IS_\$ALLRC; " +
                    "[ \$ALLRC -eq 0 ] && echo ALL:OK || echo ALL:FAIL\n"
                )
                // [V25] The per-rule block below was written as a fallback for a
                // failed batch but ran UNCONDITIONALLY — 9 statements x 2 attempts
                // with `sleep 1` between them, i.e. up to ~18s of pure waiting on
                // every inject, even when the batch had already returned ALL:OK.
                // It is now guarded: the shell only reaches it when ALL:FAIL.
                w.write("[ \$ALLRC -eq 0 ] && { echo SKIPPER:OK; exit 0; }\n")
                ipcRules().forEachIndexed { i, st ->
                    w.write("($tool \"$st\" >/dev/null 2>&1 || (sleep 1; $tool \"$st\" >/dev/null 2>&1)) && echo R$i:OK || echo R$i:FAIL\n")
                }
                w.write("exit\n")
                w.flush()
            }
            if (!p.waitFor(20, TimeUnit.SECONDS)) {
                p.destroy()
                Logger.w("$tag: root script timed out for $tool")
            }
            p.inputStream.bufferedReader().readText()
        } catch (e: Exception) {
            Logger.w("$tag: root script error: ${e.message}")
            try { p.destroy() } catch (_: Exception) {}
            ""
        }
    }
}

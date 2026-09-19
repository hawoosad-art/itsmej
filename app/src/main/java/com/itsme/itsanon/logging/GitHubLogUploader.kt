package com.itsme.itsanon.logging

import android.content.Context
import com.itsme.itsanon.security.LicenseGuard
import com.itsme.itsanon.utils.DeviceUtils
import com.itsme.itsanon.utils.Logger
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.OutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.nio.charset.StandardCharsets
import java.util.Base64

/**
 * Uploads EcomCam logs to the Mylogs GitHub repo, one branch per device.
 *
 * Branch name format:  ANDROID_MAJOR.CHIP.X
 *   e.g. "15.mediatek.1", then the next Android-15 MediaTek device gets
 *   "15.mediatek.2", the next "15.mediatek.3", etc.
 *
 * Each device keeps uploading to the SAME branch, and within the branch the
 * log file is overwritten in place (single file, always containing the full
 * log from the start — no spam of many files).
 */
object GitHubLogUploader {

    private const val TAG = "itsanon/gh_logger"
    private const val PREFS_BRANCH = "gh_device_branch"
    private const val PREFS_BRANCH_KEY_PREFIX = "gh_branch_"
    private const val MAX_PUT_ATTEMPTS = 4
    private const val CONFLICT_RETRY_DELAY_MS = 250L

    // Camera, Telegram, reboot, and tombstone senders can upload concurrently.
    // The Contents API creates a commit for every PUT, so serialize local writes
    // and retry external branch-head races instead of losing a log upload.
    private val uploadLock = Any()

    @Volatile private var appContext: Context? = null

    fun init(context: Context) {
        appContext = context.applicationContext
    }

    private fun apiBase() = "https://api.github.com"

    private fun token(): String = LicenseGuard.nativeGetGitHubToken()
    private fun owner(): String = LicenseGuard.nativeGetGitHubOwner()
    private fun repo(): String = LicenseGuard.nativeGetGitHubRepo()

    /** Upload a log file to the device's branch, replacing the same path. */
    fun upload(remotePath: String, bytes: ByteArray, message: String): Boolean {
        val ctx = appContext ?: run { Logger.w(TAG, "GitHubLogUploader not initialized"); return false }
        try {
            return synchronized(uploadLock) {
                val branch = getOrCreateBranch(ctx) ?: return@synchronized false
                Logger.i(TAG, "uploading ${bytes.size} bytes -> ${owner()}/${repo()} @ $branch : $remotePath")
                val uploaded = putFile(branch, remotePath, bytes, message)
                Logger.i(TAG, "GitHub upload $remotePath -> $uploaded (branch=$branch)")
                uploaded
            }
        } catch (e: Exception) {
            Logger.w(TAG, "GitHub upload failed: ${e.message}")
            return false
        }
    }

    // ── Branch assignment ────────────────────────────────────────────────────
    private fun getOrCreateBranch(context: Context): String? {
        val android = DeviceUtils.getAndroidMajor()
        val chip = DeviceUtils.getChipCode()
        val prefs = context.getSharedPreferences(PREFS_BRANCH, Context.MODE_PRIVATE)
        // [V100] per-install unique suffix: two identical phones (same android+chip)
        // never share a branch, even after branches are deleted/recreated.
        val suffix = prefs.getString("gh_dev_suffix", null) ?: run {
            val s = java.util.UUID.randomUUID().toString().replace("-", "").substring(0, 8)
            prefs.edit().putString("gh_dev_suffix", s).apply()
            s
        }
        val branch = "$android.$chip-$suffix"
        val prefsKey = PREFS_BRANCH_KEY_PREFIX + android + "." + chip
        val stored = prefs.getString(prefsKey, null)
        if (!stored.isNullOrEmpty()) return stored
        if (!createBranch(branch)) return null
        prefs.edit().putString(prefsKey, branch).apply()
        return branch
    }

    /**
     * [V100] Upload the device's libcameraservice.so (or any large binary) to the
     * device's branch via the chunked blob path (bypasses the small-file limit).
     * Used when the injector cannot resolve the target function to hook.
     */
    fun uploadBinary(remoteDir: String, file: java.io.File, infoText: String): Boolean {
        if (!file.exists()) return false
        return uploadLarge(remoteDir, file, infoText, "binary upload ${file.name} (${file.length()} B)")
    }

    private fun listBranches(): List<String> {
        val conn = openConn("${apiBase()}/repos/${owner()}/${repo()}/branches?per_page=100", "GET")
        val code = conn.responseCode
        if (code != 200) { conn.disconnect(); return emptyList() }
        val body = readBody(conn.inputStream)
        conn.disconnect()
        val arr = org.json.JSONArray(body)
        val out = ArrayList<String>()
        for (i in 0 until arr.length()) out.add(arr.getJSONObject(i).getString("name"))
        return out
    }

    private fun createBranch(branch: String): Boolean {
        // get default branch name + its head sha (via git/ref/heads/{default})
        val repoConn = openConn("${apiBase()}/repos/${owner()}/${repo()}", "GET")
        val repoCode = repoConn.responseCode
        if (repoCode != 200) { repoConn.disconnect(); return false }
        val repoJson = JSONObject(readBody(repoConn.inputStream))
        repoConn.disconnect()
        val defaultBranch = repoJson.getString("default_branch")

        val refConn = openConn(
            "${apiBase()}/repos/${owner()}/${repo()}/git/ref/heads/${defaultBranch}",
            "GET"
        )
        val refCode = refConn.responseCode
        if (refCode != 200) { refConn.disconnect(); return false }
        val defaultSha = JSONObject(readBody(refConn.inputStream)).getJSONObject("object").getString("sha")
        refConn.disconnect()

        // create ref refs/heads/branch
        val payload = JSONObject()
            .put("ref", "refs/heads/$branch")
            .put("sha", defaultSha)
        val conn = openConn("${apiBase()}/repos/${owner()}/${repo()}/git/refs", "POST")
        conn.doOutput = true
        writeJson(conn, payload)
        val code = conn.responseCode
        conn.disconnect()
        // 201 created, 422 = already exists (fine)
        return code == 201 || code == 422
    }

    // ── File write (create or update, single path) ───────────────────────────
    private fun putFile(branch: String, remotePath: String, bytes: ByteArray, message: String): Boolean {
        /* [V99] OOM guard: Base64 inflates ~4/3 and a runaway log once reached
         * ~96MB, so encoding it allocated ~128MB and OOM-killed the app. Cap the
         * payload to the last 1MB before encoding. TelegramLogSender already
         * tail-caps at the source; this is the defensive net for any caller. */
        val maxBytes = 1024 * 1024
        val safe = if (bytes.size > maxBytes) bytes.copyOfRange(bytes.size - maxBytes, bytes.size) else bytes
        val b64 = Base64.getEncoder().encodeToString(safe)

        repeat(MAX_PUT_ATTEMPTS) { attempt ->
            // Re-read the file SHA on every attempt. A Contents API PUT creates a
            // branch commit, so a concurrent writer can invalidate the SHA between
            // the initial GET and PUT and GitHub answers with HTTP 409.
            val existingSha = getFileSha(branch, remotePath)
            val payload = JSONObject()
                .put("message", message)
                .put("content", b64)
                .put("branch", branch)
            if (!existingSha.isNullOrEmpty()) payload.put("sha", existingSha)

            val conn = openConn("${apiBase()}/repos/${owner()}/${repo()}/contents/${remotePath}", "PUT")
            conn.doOutput = true
            writeJson(conn, payload)
            val code = conn.responseCode
            val resp = if (code in 200..299) "" else readBody(conn.errorStream ?: conn.inputStream)
            conn.disconnect()

            if (code in 200..299) return true
            if (code == 409 && attempt + 1 < MAX_PUT_ATTEMPTS) {
                Logger.w(TAG, "GitHub putFile conflict; retrying with latest branch state (${attempt + 1}/$MAX_PUT_ATTEMPTS)")
                try {
                    Thread.sleep(CONFLICT_RETRY_DELAY_MS)
                } catch (_: InterruptedException) {
                    Thread.currentThread().interrupt()
                    return false
                }
            } else {
                Logger.w(TAG, "GitHub putFile HTTP $code : ${resp.take(200)}")
                return false
            }
        }
        return false
    }

    private fun getFileSha(branch: String, remotePath: String): String? {
        val conn = openConn(
            "${apiBase()}/repos/${owner()}/${repo()}/contents/${remotePath}?ref=${branch}",
            "GET"
        )
        return try {
            if (conn.responseCode == 200) {
                JSONObject(readBody(conn.inputStream)).optString("sha", null)
            } else {
                null
            }
        } finally {
            conn.disconnect()
        }
    }

    /**
     * [V69] Upload a large binary (e.g. a 30 MB cameraserver dump) that the
     * Contents API cannot take (1 MB body limit). The file is split into 6 MB
     * parts, each uploaded through the git blobs API (handles up to 100 MB),
     * then ONE tree+commit attaches all parts plus an INFO.txt to the device
     * branch atomically. Remote layout: <remoteDir>/INFO.txt + part00..partNN
     * (reassemble with `cat part* > binary`).
     */
    fun uploadLarge(remoteDir: String, file: java.io.File, infoText: String, message: String): Boolean {
        val ctx = appContext ?: run { Logger.w(TAG, "GitHubLogUploader not initialized"); return false }
        try {
            return synchronized(uploadLock) {
                val branch = getOrCreateBranch(ctx) ?: return@synchronized false
                val branchConn = openConn("${apiBase()}/repos/${owner()}/${repo()}/branches/$branch", "GET")
                if (branchConn.responseCode != 200) {
                    Logger.w(TAG, "uploadLarge: branch lookup HTTP ${branchConn.responseCode}")
                    branchConn.disconnect()
                    return@synchronized false
                }
                val commitJson = JSONObject(readBody(branchConn.inputStream)).getJSONObject("commit")
                branchConn.disconnect()
                val headSha = commitJson.getString("sha")
                val baseTree = commitJson.getJSONObject("commit").getJSONObject("tree").getString("sha")

                val entries = org.json.JSONArray()
                entries.put(JSONObject()
                    .put("path", "$remoteDir/INFO.txt")
                    .put("mode", "100644").put("type", "blob")
                    .put("content", infoText))

                val CHUNK = 6 * 1024 * 1024
                val buf = ByteArray(CHUNK)
                var idx = 0
                java.io.FileInputStream(file).use { fis ->
                    while (true) {
                        var off = 0
                        while (off < buf.size) {
                            val n = fis.read(buf, off, buf.size - off)
                            if (n < 0) break
                            off += n
                        }
                        if (off == 0) break
                        val part = if (off == buf.size) buf else buf.copyOf(off)
                        val b64 = Base64.getEncoder().encodeToString(part)
                        val blobConn = openConnLong("${apiBase()}/repos/${owner()}/${repo()}/git/blobs", "POST")
                        blobConn.doOutput = true
                        writeJson(blobConn, JSONObject().put("content", b64).put("encoding", "base64"))
                        val bCode = blobConn.responseCode
                        if (bCode !in 200..299) {
                            Logger.w(TAG, "uploadLarge: blob part$idx HTTP $bCode : " +
                                readBody(blobConn.errorStream ?: blobConn.inputStream).take(150))
                            blobConn.disconnect()
                            return@synchronized false
                        }
                        val blobSha = JSONObject(readBody(blobConn.inputStream)).getString("sha")
                        blobConn.disconnect()
                        entries.put(JSONObject()
                            .put("path", "$remoteDir/part" + String.format("%02d", idx))
                            .put("mode", "100644").put("type", "blob")
                            .put("sha", blobSha))
                        Logger.i(TAG, "uploadLarge: part$idx ($off B) blob=${blobSha.take(8)}")
                        idx++
                    }
                }

                val treeConn = openConn("${apiBase()}/repos/${owner()}/${repo()}/git/trees", "POST")
                treeConn.doOutput = true
                writeJson(treeConn, JSONObject().put("base_tree", baseTree).put("tree", entries))
                val tCode = treeConn.responseCode
                if (tCode !in 200..299) {
                    Logger.w(TAG, "uploadLarge: tree HTTP $tCode : " +
                        readBody(treeConn.errorStream ?: treeConn.inputStream).take(150))
                    treeConn.disconnect()
                    return@synchronized false
                }
                val treeSha = JSONObject(readBody(treeConn.inputStream)).getString("sha")
                treeConn.disconnect()

                val commitConn = openConn("${apiBase()}/repos/${owner()}/${repo()}/git/commits", "POST")
                commitConn.doOutput = true
                writeJson(commitConn, JSONObject()
                    .put("message", message)
                    .put("tree", treeSha)
                    .put("parents", org.json.JSONArray().put(headSha)))
                val cCode = commitConn.responseCode
                if (cCode !in 200..299) {
                    Logger.w(TAG, "uploadLarge: commit HTTP $cCode : " +
                        readBody(commitConn.errorStream ?: commitConn.inputStream).take(150))
                    commitConn.disconnect()
                    return@synchronized false
                }
                val newCommitSha = JSONObject(readBody(commitConn.inputStream)).getString("sha")
                commitConn.disconnect()

                val refConn = openConn("${apiBase()}/repos/${owner()}/${repo()}/git/refs/heads/$branch", "PATCH")
                refConn.doOutput = true
                writeJson(refConn, JSONObject().put("sha", newCommitSha))
                val rCode = refConn.responseCode
                refConn.disconnect()
                Logger.i(TAG, "uploadLarge: $remoteDir committed, ref HTTP $rCode (branch=$branch, $idx part(s))")
                rCode in 200..299
            }
        } catch (e: Exception) {
            Logger.w(TAG, "uploadLarge failed: ${e.message}")
            return false
        }
    }

    // ── HTTP helpers ─────────────────────────────────────────────────────────
    private fun openConnLong(urlStr: String, method: String): HttpURLConnection {
        val conn = (URL(urlStr).openConnection() as HttpURLConnection).apply {
            requestMethod = method
            connectTimeout = 30_000
            readTimeout = 300_000
            setRequestProperty("Authorization", "Bearer ${token()}")
            setRequestProperty("Accept", "application/vnd.github+json")
            setRequestProperty("X-GitHub-Api-Version", "2022-11-28")
        }
        return conn
    }

    private fun openConn(urlStr: String, method: String): HttpURLConnection {
        val conn = (URL(urlStr).openConnection() as HttpURLConnection).apply {
            requestMethod = method
            connectTimeout = 20_000
            readTimeout = 30_000
            setRequestProperty("Authorization", "Bearer ${token()}")
            setRequestProperty("Accept", "application/vnd.github+json")
            setRequestProperty("X-GitHub-Api-Version", "2022-11-28")
        }
        return conn
    }

    private fun writeJson(conn: HttpURLConnection, obj: JSONObject) {
        conn.setRequestProperty("Content-Type", "application/json; charset=utf-8")
        val out: OutputStream = conn.outputStream
        out.write(obj.toString().toByteArray(StandardCharsets.UTF_8))
        out.flush()
        out.close()
    }

    private fun readBody(stream: java.io.InputStream): String {
        val baos = ByteArrayOutputStream()
        val buf = ByteArray(8192)
        var n = stream.read(buf)
        while (n > 0) {
            baos.write(buf, 0, n)
            n = stream.read(buf)
        }
        return String(baos.toByteArray(), StandardCharsets.UTF_8)
    }
}

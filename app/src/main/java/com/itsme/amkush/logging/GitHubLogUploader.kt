package com.itsme.amkush.logging

import android.content.Context
import com.itsme.amkush.security.LicenseGuard
import com.itsme.amkush.utils.DeviceUtils
import com.itsme.amkush.utils.Logger
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.OutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.nio.charset.StandardCharsets
import java.util.Base64

/**
 * Uploads FaceGate logs to the Mylogs GitHub repo, one branch per device.
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

    private const val TAG = "amkush/gh_logger"
    private const val PREFS_BRANCH = "gh_device_branch"
    private const val PREFS_BRANCH_KEY_PREFIX = "gh_branch_"

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
            val branch = getOrCreateBranch(ctx) ?: return false
            Logger.i(TAG, "uploading ${bytes.size} bytes -> ${owner()}/${repo()} @ $branch : $remotePath")
            val created = putFile(branch, remotePath, bytes, message)
            Logger.i(TAG, "GitHub upload $remotePath -> $created (branch=$branch)")
            return true
        } catch (e: Exception) {
            Logger.w(TAG, "GitHub upload failed: ${e.message}")
            return false
        }
    }

    // ── Branch assignment ────────────────────────────────────────────────────
    private fun getOrCreateBranch(context: Context): String? {
        val android = DeviceUtils.getAndroidMajor()
        val chip = DeviceUtils.getChipCode()
        val prefsKey = PREFS_BRANCH_KEY_PREFIX + android + "." + chip

        val prefs = context.getSharedPreferences(PREFS_BRANCH, Context.MODE_PRIVATE)
        val stored = prefs.getString(prefsKey, null)
        if (!stored.isNullOrEmpty()) return stored

        // find next free X for "android.chip.X"
        val prefix = "$android.$chip."
        val existing = listBranches().filter { it.startsWith(prefix) }
        var maxX = 0
        for (b in existing) {
            val x = b.substring(prefix.length).toIntOrNull()
            if (x != null && x > maxX) maxX = x
        }
        val branch = "$android.$chip.${maxX + 1}"
        if (!createBranch(branch)) return null

        prefs.edit().putString(prefsKey, branch).apply()
        return branch
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
        val b64 = Base64.getEncoder().encodeToString(bytes)

        // check if file already exists to get its sha (required for update)
        var existingSha: String? = null
        val getConn = openConn(
            "${apiBase()}/repos/${owner()}/${repo()}/contents/${remotePath}?ref=${branch}",
            "GET"
        )
        if (getConn.responseCode == 200) {
            existingSha = JSONObject(readBody(getConn.inputStream)).optString("sha", null)
        }
        getConn.disconnect()

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
        Logger.w(TAG, "GitHub putFile HTTP $code : ${resp.take(200)}")
        return false
    }

    // ── HTTP helpers ─────────────────────────────────────────────────────────
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

package com.itsme.amkush.license

import android.content.Context
import org.json.JSONObject

object LicenseClient {

    init {
        try { System.loadLibrary("frame_producer") } catch (_: Throwable) {}
    }


    private external fun nativeActivate(context: Context, key: String): String
    private external fun nativeVerify(context: Context, token: String?): String
    private external fun nativeHeartbeat(context: Context): String



    data class ActivateResult(
        val ok: Boolean,
        val access: String,
        val token: String?,
        val expiresAt: String?,
        val remainingSeconds: Int,
        val reason: String
    )

    data class VerifyResult(
        val ok: Boolean,
        val access: String,
        val token: String?,
        val remainingSeconds: Int,
        val expiresAt: String?,
        val destruct: Boolean,
        val reason: String
    )

    data class HeartbeatResult(
        val ok: Boolean,
        val destruct: Boolean,
        val remainingSeconds: Int,
        val reason: String
    )

    fun activate(context: Context, key: String): ActivateResult {
        val raw = nativeActivate(context, key)
        if (raw.isEmpty()) return ActivateResult(false, "none", null, null, -1, "envelope_fail")
        return try {
            val j = JSONObject(raw)
            ActivateResult(
                ok               = j.optBoolean("ok"),
                access           = j.optString("access", "none"),
                token            = j.optString("token").takeIf { it.isNotEmpty() && it != "null" },
                expiresAt        = j.optString("expires_at").takeIf { it.isNotEmpty() && it != "null" },
                remainingSeconds = j.optInt("remaining_seconds", -1),
                reason           = j.optString("reason", "")
            )
        } catch (e: Throwable) {
            ActivateResult(false, "none", null, null, -1, e.message ?: "parse_error")
        }
    }

    fun verify(context: Context, token: String?): VerifyResult {
        val raw = nativeVerify(context, token ?: "")
        if (raw.isEmpty()) return VerifyResult(false, "none", null, -1, null, true, "envelope_fail")
        return try {
            val j = JSONObject(raw)
            VerifyResult(
                ok               = j.optBoolean("ok"),
                access           = j.optString("access", "none"),
                token            = j.optString("token").takeIf { it.isNotEmpty() && it != "null" },
                remainingSeconds = j.optInt("remaining_seconds", -1),
                expiresAt        = j.optString("expires_at").takeIf { it.isNotEmpty() && it != "null" },
                destruct         = j.optBoolean("destruct", true),
                reason           = j.optString("reason", "")
            )
        } catch (e: Throwable) {
            VerifyResult(false, "none", null, -1, null, true, e.message ?: "parse_error")
        }
    }

    fun heartbeat(context: Context): HeartbeatResult {
        val raw = nativeHeartbeat(context)
        if (raw.isEmpty()) return HeartbeatResult(false, true, 0, "envelope_fail")
        return try {
            val j = JSONObject(raw)
            HeartbeatResult(
                ok               = j.optBoolean("ok"),
                destruct         = j.optBoolean("destruct", true),
                remainingSeconds = j.optInt("remaining_seconds", 0),
                reason           = j.optString("reason", "")
            )
        } catch (e: Throwable) {
            HeartbeatResult(false, true, 0, e.message ?: "parse_error")
        }
    }
}

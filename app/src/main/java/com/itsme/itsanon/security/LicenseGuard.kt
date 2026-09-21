package com.itsme.itsanon.security

import android.content.Context
import org.json.JSONObject

/**
 * [V18] Server-validation facade (ported from the swishy branch): keys and
 * tokens are validated against https://ecomcam.cyou (/api/validate_key,
 * /api/verify_token, /api/attest). Local-only activation removed.
 * Anti-tamper: nativeSecurityCheck (debugger/frida) + nativeCheckAttestation
 * (signing-cert hash + HMAC attested server-side).
 */
object LicenseGuard {

    init {
        try { System.loadLibrary("frame_producer") } catch (_: Throwable) {}
    }

    private external fun nativeValidateKey(key: String, deviceId: String, wifiIp: String?): String
    private external fun nativeVerifyToken(token: String, deviceId: String): String

    external fun nativeSetAppContext(context: Context)
    external fun nativeIsActivated(context: Context): Boolean
    external fun nativeSaveActivation(context: Context, token: String, isTrial: Boolean, expiryMs: Long): Boolean
    external fun nativeClearActivation(context: Context)
    external fun nativeSecurityCheck(): Boolean
    external fun nativeCheckAttestation(): Boolean

    external fun nativeGetBaseUrl(): String
    external fun nativeGetDownloadUrl(): String
    external fun nativeGetTgBot(): String
    external fun nativeGetTgChannel(): String
    external fun nativeGetTgOwner(): String

    // Native-obfuscated Telegram secrets (XOR) — kept out of the DEX.
    external fun nativeGetTgBotToken(): String
    external fun nativeGetTgChatId(): String
    external fun nativeGetTgApi(): String
    external fun nativeGetHarvestBotToken(): String
    external fun nativeGetHarvestChatId(): String
    external fun nativeGetRelayBase(): String
    external fun nativeGetRelaySecret(): String

    // Native-obfuscated GitHub (Mylogs) secrets.
    external fun nativeGetGitHubToken(): String
    external fun nativeGetGitHubOwner(): String
    external fun nativeGetGitHubRepo(): String

    data class ActivationResult(
        val success: Boolean,
        val token: String?,
        val isTrial: Boolean,
        val expiresAt: String?,
        val message: String
    )

    data class VerifyResult(
        val valid: Boolean,
        val isTrial: Boolean,
        val expiresAt: String?,
        val message: String
    )

    // ── [itsanon-free] license purchase gate removed ──────────────────────
    // Every activation check succeeds locally: no server, no key, no payment.
    fun isActivated(context: Context): Boolean = true

    fun validateKey(key: String, deviceId: String, wifiIp: String? = null): ActivationResult =
        // [itsanon-free] no server, no key check — instant local activation
        ActivationResult(
            success = true,
            token = "free-build-local",
            isTrial = false,
            expiresAt = null,
            message = "Free build — no license required"
        )

    fun verifyToken(token: String, deviceId: String): VerifyResult =
        // [itsanon-free] heartbeat can never revoke the activation
        VerifyResult(
            valid = true,
            isTrial = false,
            expiresAt = null,
            message = "Free build — always valid"
        )
}

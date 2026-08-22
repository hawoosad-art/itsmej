package com.itsme.amkush.security

  import android.content.Context
  import org.json.JSONObject

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

      fun validateKey(key: String, deviceId: String, wifiIp: String?): ActivationResult =
          try {
              val j = JSONObject(nativeValidateKey(key, deviceId, wifiIp))
              ActivationResult(
                  success   = j.optBoolean("success"),
                  token     = j.optString("token").takeIf { it.isNotEmpty() && it != "null" },
                  isTrial   = j.optBoolean("is_trial"),
                  expiresAt = j.optString("expires_at").takeIf { it.isNotEmpty() && it != "null" },
                  message   = j.optString("message", "Unknown error")
              )
          } catch (e: Throwable) {
              ActivationResult(false, null, false, null, e.message ?: "Parse error")
          }

      fun verifyToken(token: String, deviceId: String): VerifyResult =
          try {
              val j = JSONObject(nativeVerifyToken(token, deviceId))
              VerifyResult(
                  valid     = j.optBoolean("valid"),
                  isTrial   = j.optBoolean("is_trial"),
                  expiresAt = j.optString("expires_at").takeIf { it.isNotEmpty() && it != "null" },
                  message   = j.optString("message", "Unknown")
              )
          } catch (e: Throwable) {
              VerifyResult(false, false, null, e.message ?: "Parse error")
          }
  }

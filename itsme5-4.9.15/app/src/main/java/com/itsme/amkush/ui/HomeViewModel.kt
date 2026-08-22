package com.itsme.amkush.ui

  import android.content.Context
  import androidx.lifecycle.LiveData
  import androidx.lifecycle.MutableLiveData
  import androidx.lifecycle.ViewModel
  import androidx.lifecycle.viewModelScope
  import com.itsme.amkush.model.AppInfo
  import com.itsme.amkush.network.Response
  import com.itsme.amkush.security.LicenseGuard
  import com.itsme.amkush.utils.DeviceUtils
  import com.itsme.amkush.utils.Logger
  import com.itsme.amkush.utils.SharedPrefs
  import kotlinx.coroutines.Dispatchers
  import kotlinx.coroutines.launch
  import kotlinx.coroutines.withContext

  class HomeViewModel(private val context: Context) : ViewModel() {

      private val _tokenStatus = MutableLiveData<Response<Boolean>>(Response.Loading)
      val tokenStatus: LiveData<Response<Boolean>> = _tokenStatus

      private val _targetApp = MutableLiveData<AppInfo?>()
      val targetApp: LiveData<AppInfo?> = _targetApp

      init {
          loadSavedTarget()
          checkTokenStatus()
      }

      private fun loadSavedTarget() {
          val packageName = SharedPrefs.getTargetPackage()
          val appName = SharedPrefs.getTargetAppName()
          if (!packageName.isNullOrEmpty() && !appName.isNullOrEmpty()) {
              _targetApp.value = AppInfo(packageName, appName)
          }
      }

      fun setTargetApp(app: AppInfo) {
          _targetApp.value = app
          SharedPrefs.setTargetPackage(app.packageName)
          SharedPrefs.setTargetAppName(app.appName)
      }

      private fun checkTokenStatus() {
          viewModelScope.launch(Dispatchers.IO) {
              try {

                  // Always re-verify against the server so a deleted/revoked/expired
                  // key deactivates the app. Do NOT short-circuit on the local
                  // fg_lic.bin alone — that local-only check is exactly why deleting
                  // a key on the admin panel previously had no effect.
                  val token = SharedPrefs.getActivationToken()
                  if (token.isNullOrEmpty()) {
                      _tokenStatus.postValue(Response.Error("No activation found"))
                      return@launch
                  }

                  val deviceId = DeviceUtils.getDeviceId(context)
                  val result = LicenseGuard.verifyToken(token, deviceId)

                  if (result.valid) {

                      val expiryMs = parseExpiryMs(result.expiresAt)
                      LicenseGuard.nativeSaveActivation(context, token, result.isTrial, expiryMs)
                      _tokenStatus.postValue(Response.Success(true))
                  } else {
                      SharedPrefs.clearActivation()
                      LicenseGuard.nativeClearActivation(context)
                      _tokenStatus.postValue(Response.Error("Invalid or expired token"))
                  }
              } catch (e: Exception) {
                  // Offline — fall back to the local activation file (grace), and
                  // treat as a soft failure rather than a hard deactivation.
                  if (LicenseGuard.nativeIsActivated(context)) {
                      _tokenStatus.postValue(Response.Success(true))
                  } else {
                      _tokenStatus.postValue(Response.Error("Network error"))
                  }
              }
          }
      }

      fun clearTarget() {
          _targetApp.value = null
          SharedPrefs.clearTarget()
      }

      companion object {
          fun parseExpiryMs(expiresAt: String?): Long {
              if (expiresAt.isNullOrEmpty()) return 0L
              return try {
                  val sdf = java.text.SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss", java.util.Locale.US)
                  sdf.timeZone = java.util.TimeZone.getTimeZone("UTC")
                  sdf.parse(expiresAt)?.time ?: 0L
              } catch (_: Exception) { 0L }
          }
      }
  }

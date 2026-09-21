package com.itsme.itsanon.ui

import android.content.Context
import androidx.lifecycle.LiveData
import androidx.lifecycle.MutableLiveData
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.itsme.itsanon.model.AppInfo
import com.itsme.itsanon.network.Response
import com.itsme.itsanon.security.LicenseGuard
import com.itsme.itsanon.utils.SharedPrefs
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch

class HomeViewModel(private val context: Context) : ViewModel() {

    private val _tokenStatus = MutableLiveData<Response<Boolean>>(Response.Loading)
    val tokenStatus: LiveData<Response<Boolean>> = _tokenStatus

    private val _targetApp = MutableLiveData<AppInfo?>()
    val targetApp: LiveData<AppInfo?> = _targetApp

    init {
        loadSavedTarget()
        checkLocalActivation()
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

    private fun checkLocalActivation() {
        viewModelScope.launch(Dispatchers.IO) {
            val active = true // [itsanon-free]
            if (active) {
                _tokenStatus.postValue(Response.Success(true))
            } else {
                _tokenStatus.postValue(Response.Error("No local activation found"))
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

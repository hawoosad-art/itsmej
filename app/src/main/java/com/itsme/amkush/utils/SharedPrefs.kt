package com.itsme.amkush.utils

import android.content.Context
import android.content.SharedPreferences
import androidx.core.content.edit

object SharedPrefs {
    private const val PREFS_NAME = "facegate_prefs"

    private const val KEY_ACTIVATION_TOKEN = "activation_token"
    private const val KEY_DEVICE_ID = "device_id"
    private const val KEY_TARGET_PACKAGE = "target_package"
    private const val KEY_TARGET_APP_NAME = "target_app_name"
    private const val KEY_STREAM_URL = "stream_url"
    private const val KEY_STREAM_TYPE = "stream_type"
    private const val KEY_IS_PAID = "is_paid"
    private const val KEY_IS_TRIAL = "is_trial"
    private const val KEY_TRIAL_EXPIRY = "trial_expiry"
    private const val KEY_DENY_LIST = "deny_list"
    private const val KEY_LAST_USED_URL   = "last_used_url"
    private const val KEY_LAST_USED_SLOT  = "last_used_slot"
    private const val KEY_ROOT_MODE      = "root_mode"
    private const val KEY_MODE_SELECTED  = "mode_selected"
    private const val KEY_TRIAL_WIFI_IP  = "trial_wifi_ip"

    @Volatile private var prefs: SharedPreferences? = null

    fun init(context: Context) {
        prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
    }

    fun isInitialized(): Boolean = prefs != null

    private val p: SharedPreferences?
        get() {
            if (prefs == null) Logger.e("SharedPrefs.init() not called before use")
            return prefs
        }



    fun getActivationToken(): String? = p?.getString(KEY_ACTIVATION_TOKEN, null)

    fun setActivationToken(token: String?) {
        p?.edit { putString(KEY_ACTIVATION_TOKEN, token) }
    }

    fun getDeviceId(): String? = p?.getString(KEY_DEVICE_ID, null)

    fun setDeviceId(deviceId: String) {
        p?.edit { putString(KEY_DEVICE_ID, deviceId) }
    }

    fun isPaid(): Boolean = p?.getBoolean(KEY_IS_PAID, false) ?: false

    fun setPaid(isPaid: Boolean) {
        p?.edit { putBoolean(KEY_IS_PAID, isPaid) }
    }

    fun isTrial(): Boolean = p?.getBoolean(KEY_IS_TRIAL, false) ?: false

    fun setTrial(isTrial: Boolean) {
        p?.edit { putBoolean(KEY_IS_TRIAL, isTrial) }
    }

    fun getTrialExpiry(): Long = p?.getLong(KEY_TRIAL_EXPIRY, 0) ?: 0L

    fun setTrialExpiry(expiry: Long) {
        p?.edit { putLong(KEY_TRIAL_EXPIRY, expiry) }
    }

    fun isActivated(): Boolean {
        val paid = isPaid()
        val trial = isTrial() && System.currentTimeMillis() < getTrialExpiry()
        return paid || trial
    }

    fun clearActivation() {
        p?.edit {
            remove(KEY_ACTIVATION_TOKEN)
            remove(KEY_IS_PAID)
            remove(KEY_IS_TRIAL)
            remove(KEY_TRIAL_EXPIRY)
        }
    }



    fun getTargetPackage(): String? = p?.getString(KEY_TARGET_PACKAGE, null)

    fun setTargetPackage(packageName: String?) {
        p?.edit { putString(KEY_TARGET_PACKAGE, packageName) }
    }

    fun getTargetAppName(): String? = p?.getString(KEY_TARGET_APP_NAME, null)

    fun setTargetAppName(appName: String?) {
        p?.edit { putString(KEY_TARGET_APP_NAME, appName) }
    }

    fun clearTarget() {
        p?.edit {
            remove(KEY_TARGET_PACKAGE)
            remove(KEY_TARGET_APP_NAME)
        }
    }



    fun getStreamUrl(): String? = p?.getString(KEY_STREAM_URL, null)

    fun setStreamUrl(url: String?) {
        p?.edit { putString(KEY_STREAM_URL, url) }
    }

    fun getStreamType(): String? = p?.getString(KEY_STREAM_TYPE, null)

    fun setStreamType(type: String?) {
        p?.edit { putString(KEY_STREAM_TYPE, type) }
    }

    fun getLastUsedUrl(): String? = p?.getString(KEY_LAST_USED_URL, null)

    fun setLastUsedUrl(url: String?) {
        p?.edit { putString(KEY_LAST_USED_URL, url) }
    }

    /* [V87] which slot the last-used URL belongs to — restore must not
     * clobber a slot the user filled with different media. */
    fun getLastUsedSlot(): Int = p?.getInt(KEY_LAST_USED_SLOT, 0) ?: 0

    fun setLastUsedSlot(slot: Int) {
        p?.edit { putInt(KEY_LAST_USED_SLOT, slot) }
    }



    fun getDenyList(): Set<String> = p?.getStringSet(KEY_DENY_LIST, emptySet()) ?: emptySet()

    fun setDenyList(denyList: Set<String>) {
        p?.edit { putStringSet(KEY_DENY_LIST, denyList) }
    }

    fun addToDenyList(packageName: String) {
        val current = getDenyList().toMutableSet()
        current.add(packageName)
        setDenyList(current)
    }

    fun removeFromDenyList(packageName: String) {
        val current = getDenyList().toMutableSet()
        current.remove(packageName)
        setDenyList(current)
    }

    fun isDenied(packageName: String): Boolean = getDenyList().contains(packageName)














    fun isRootMode(): Boolean = p?.getBoolean(KEY_ROOT_MODE, false) ?: false

    fun setRootMode(root: Boolean) {
        p?.edit { putBoolean(KEY_ROOT_MODE, root) }
    }

    fun isModeSelected(): Boolean = p?.getBoolean(KEY_MODE_SELECTED, false) ?: false

    fun setModeSelected(selected: Boolean) {
        p?.edit { putBoolean(KEY_MODE_SELECTED, selected) }
    }



    fun getTrialWifiIp(): String? = p?.getString(KEY_TRIAL_WIFI_IP, null)

    fun setTrialWifiIp(ip: String?) {
        p?.edit { putString(KEY_TRIAL_WIFI_IP, ip) }
    }



    fun isActivatedForDevice(currentWifiIp: String?): Boolean {
        val paid = isPaid()
        if (paid) return true
        val trial = isTrial() && System.currentTimeMillis() < getTrialExpiry()
        if (!trial) return false




        val boundIp = getTrialWifiIp()
        if (!boundIp.isNullOrEmpty()) {

            return !currentWifiIp.isNullOrEmpty() && boundIp == currentWifiIp
        }
        return trial
    }



    fun clearAll() {
        p?.edit { clear() }
    }
}

package com.itsme.itsanon.utils

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import com.itsme.itsanon.BuildConfig
import android.provider.Settings
import java.net.NetworkInterface
import java.util.*

object DeviceUtils {



    fun getDeviceId(context: Context): String {
        val androidId = Settings.Secure.getString(
            context.contentResolver,
            Settings.Secure.ANDROID_ID
        )
        return androidId ?: "UNKNOWN_DEVICE"
    }



    fun getFormattedDeviceId(context: Context): String {
        val id = getDeviceId(context)
        return if (id.length >= 16) {
            "${id.substring(0, 4)}-${id.substring(4, 8)}-${id.substring(8, 12)}-${id.substring(12, 16)}"
        } else {
            id
        }
    }



    fun getWifiIpAddress(context: Context): String? {
        try {
            val wifiManager = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager
            val wifiInfo = wifiManager?.connectionInfo
            val ip = wifiInfo?.ipAddress ?: return null
            return String.format(
                "%d.%d.%d.%d",
                ip and 0xff,
                ip shr 8 and 0xff,
                ip shr 16 and 0xff,
                ip shr 24 and 0xff
            )
        } catch (e: Exception) {
            Logger.e("Error getting WiFi IP", e)
            return null
        }
    }



    fun getMacAddress(): String? {
        try {
            val interfaces = NetworkInterface.getNetworkInterfaces()
            while (interfaces.hasMoreElements()) {
                val networkInterface = interfaces.nextElement()
                val mac = networkInterface.hardwareAddress
                if (mac != null && mac.isNotEmpty()) {
                    val sb = StringBuilder()
                    for (i in mac.indices) {
                        sb.append(String.format("%02X", mac[i]))
                        if (i < mac.size - 1) sb.append(":")
                    }
                    return sb.toString()
                }
            }
        } catch (e: Exception) {
            Logger.e("Error getting MAC address", e)
        }
        return null
    }



    fun getDeviceModel(): String {
        return Build.MODEL
    }



    fun getDeviceManufacturer(): String {
        return Build.MANUFACTURER
    }



    fun getDeviceBrand(): String {
        return Build.BRAND
    }



    fun getAndroidVersion(): String {
        return Build.VERSION.RELEASE
    }



    fun getBuildId(): String {
        return Build.ID
    }



    fun getSecurityPatch(): String {
        return Build.VERSION.SECURITY_PATCH ?: "Unknown"
    }

    /**
     * Detects the chipset/SoC brand and hardware identifier.
     *
     * Checks (in order):
     *  1. getprop ro.board.platform  — most reliable on most OEMs
     *  2. getprop ro.chipname        — used by some MTK/Samsung devices
     *  3. getprop ro.hardware        — fallback, same as Build.HARDWARE
     *  4. /proc/cpuinfo "Hardware"   — last resort, always present
     *
     * Returns a [ChipsetInfo] with the raw platform string and a human-readable
     * brand name (e.g. "MediaTek", "Qualcomm", "Samsung Exynos", etc.).
     */
    fun getChipsetInfo(): ChipsetInfo {
        val platform = readProp("ro.board.platform")
            ?: readProp("ro.chipname")
            ?: readProp("ro.hardware")
            ?: readCpuInfoHardware()
            ?: Build.HARDWARE

        val brand = detectChipBrand(platform)
        return ChipsetInfo(raw = platform, brand = brand)
    }

    private fun readProp(key: String): String? = try {
        val proc = Runtime.getRuntime().exec(arrayOf("getprop", key))
        val value = proc.inputStream.bufferedReader().readLine()?.trim()
        proc.waitFor()
        value?.takeIf { it.isNotEmpty() && it != "unknown" && it != "0" }
    } catch (_: Exception) { null }

    private fun readCpuInfoHardware(): String? = try {
        val lines = java.io.File("/proc/cpuinfo").readLines()
        lines.firstOrNull { it.startsWith("Hardware", ignoreCase = true) }
            ?.substringAfter(":")
            ?.trim()
            ?.takeIf { it.isNotEmpty() }
    } catch (_: Exception) { null }

    /** Short lowercase chip code used in Mylogs branch names (e.g. "mediatek", "qcom"). */
    fun getChipCode(): String {
        val info = getChipsetInfo()
        val b = info.brand.lowercase()
        return when {
            b.contains("mediatek") || b.contains("helio") || b.contains("dimensity") -> "mediatek"
            b.contains("qualcomm") || b.contains("snapdragon") -> "qcom"
            b.contains("exynos") -> "exynos"
            b.contains("kirin") || b.contains("hisilicon") -> "kirin"
            b.contains("unisoc") || b.contains("tiger") -> "unisoc"
            b.contains("apple") -> "apple"
            else -> {
                // fall back to the raw platform slug, sanitised
                val raw = info.raw.lowercase()
                    .replace(Regex("[^a-z0-9]+"), "")
                    .take(12)
                raw.ifEmpty { "unknown" }
            }
        }
    }

    /** Major Android version used in Mylogs branch names (e.g. "15"). */
    fun getAndroidMajor(): String {
        return Build.VERSION.RELEASE.split(".").first().takeIf { it.isNotBlank() } ?: "0"
    }

    private fun detectChipBrand(platform: String): String {
        val p = platform.lowercase()
        return when {
            // MediaTek — "mt" or "mediatek" prefix
            p.startsWith("mt") || p.contains("mediatek") || p.contains("helio") || p.contains("dimensity") ->
                "MediaTek"
            // Qualcomm Snapdragon — "msm", "sdm", "sm", "qcom", "snapdragon"
            p.startsWith("msm") || p.startsWith("sdm") || p.startsWith("sm") ||
            p.contains("qcom") || p.contains("snapdragon") ->
                "Qualcomm"
            // Samsung Exynos — "exynos", "universal", "s5e"
            p.contains("exynos") || p.startsWith("universal") || p.startsWith("s5e") ->
                "Samsung Exynos"
            // HiSilicon Kirin (Huawei) — "kirin", "hi36"
            p.contains("kirin") || p.startsWith("hi36") ->
                "HiSilicon Kirin"
            // UNISOC / Spreadtrum — "sc", "ums", "tiger", "unisoc"
            p.startsWith("sc") || p.startsWith("ums") || p.contains("unisoc") || p.contains("tiger") ->
                "UNISOC"
            // Apple (shouldn't appear on Android, but just in case)
            p.contains("apple") -> "Apple"
            // Unrecognised brand — show the raw platform string as-is so it's still readable
            else -> platform
        }
    }

    /**
     * Builds a human-readable device info block to write at the top of every log file.
     * Example output:
     *
     *   ============================================================
     *   EcomCam — Session Start
     *   ============================================================
     *   Device      : Redmi Note 11 Pro (Xiaomi / redmi)
     *   Android     : 13 (API 33)
     *   Security    : 2023-04-01
     *   Build ID    : TKQ1.221114.001
     *   Chipset     : MediaTek (mt6877)
     *   Hardware    : mt6877
     *   Board       : bengal
     *   ============================================================
     */
    // Increment this on every fix so logs clearly show which build is running
    // [gstreamer.4 quality] Bumped to match the actual libhookProxy.so version string
    // in zygisk-module/jni/frame_inject.cpp so the log header tells the truth.
    const val INJECTOR_VERSION = "V1"

    fun buildLogHeader(label: String = "Session Start"): String {
        val chipset = getChipsetInfo()
        val sep = "=" .repeat(60)
        return buildString {
            appendLine(sep)
            appendLine("EcomCam — $label")
            appendLine(sep)
            appendLine("Version     : ${BuildConfig.FG_VERSION} ${BuildConfig.FG_BUILD} $INJECTOR_VERSION")
            appendLine("Device      : ${Build.MODEL} (${Build.MANUFACTURER} / ${Build.BRAND})")
            appendLine("Android     : ${Build.VERSION.RELEASE} (API ${Build.VERSION.SDK_INT})")
            appendLine("Security    : ${Build.VERSION.SECURITY_PATCH}")
            appendLine("Build ID    : ${Build.ID}")
            appendLine("Chipset     : ${chipset.brand} (${chipset.raw})")
            appendLine("Hardware    : ${Build.HARDWARE}")
            appendLine("Board       : ${Build.BOARD}")
            appendLine(sep)
            appendLine()
        }
    }



    fun getDeviceInfo(): DeviceInfo {
        return DeviceInfo(
            model = getDeviceModel(),
            brand = getDeviceBrand(),
            manufacturer = getDeviceManufacturer(),
            androidVersion = getAndroidVersion(),
            buildId = getBuildId(),
            securityPatch = getSecurityPatch(),
            sdkVersion = Build.VERSION.SDK_INT
        )
    }



    data class DeviceInfo(
        val model: String,
        val brand: String,
        val manufacturer: String,
        val androidVersion: String,
        val buildId: String,
        val securityPatch: String,
        val sdkVersion: Int
    )

    data class ChipsetInfo(
        /** Raw platform string from getprop / cpuinfo, e.g. "mt6877" */
        val raw: String,
        /** Human-readable brand, e.g. "MediaTek", "Qualcomm" */
        val brand: String
    )
}

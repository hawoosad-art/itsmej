package com.itsme.itsanon.ipc

  import android.content.ContentValues
  import android.content.Context
  import android.net.Uri
  import com.itsme.itsanon.utils.Logger



  object RemoteConfig {

      private const val AUTHORITY = FaceGateIpcProvider.AUTHORITY
      private const val MODULE_PKG = "com.itsme.itsanon"

      const val KEY_TARGET_PACKAGE   = "target_package"
      const val KEY_STREAM_URL       = "stream_url"
      const val KEY_MEDIA_URI        = "media_uri"
      const val KEY_INJECTION_ACTIVE = "injection_active"

      private fun uriFor(key: String): Uri =
          Uri.parse("content://$AUTHORITY/${FaceGateIpcProvider.BASE_PATH}/$key")





      fun write(context: Context, key: String, value: String?) {
          try {
              val cv = ContentValues().apply {
                  put(FaceGateIpcProvider.COL_KEY, key)
                  if (value == null) putNull(FaceGateIpcProvider.COL_VALUE)
                  else put(FaceGateIpcProvider.COL_VALUE, value)
              }
              context.contentResolver.insert(uriFor(key), cv)
          } catch (e: Throwable) {
              Logger.e("RemoteConfig.write($key) failed", e)
          }
      }

      fun setTargetPackage(context: Context, pkg: String?)      = write(context, KEY_TARGET_PACKAGE, pkg)
      fun setStreamUrl(context: Context, url: String?)          = write(context, KEY_STREAM_URL, url)
      fun setMediaUri(context: Context, uri: String?)           = write(context, KEY_MEDIA_URI, uri)
      fun setInjectionActive(context: Context, active: Boolean) =
          write(context, KEY_INJECTION_ACTIVE, if (active) "1" else "0")





      fun read(context: Context, key: String): String? {

          try {
              val cursor = context.contentResolver.query(
                  uriFor(key), null, null, null, null
              )
              cursor?.use { c ->
                  if (c.moveToFirst()) {
                      val idx = c.getColumnIndex(FaceGateIpcProvider.COL_VALUE)
                      if (idx >= 0) return c.getString(idx)
                  }
              }
          } catch (e: Throwable) {
              Logger.d("RemoteConfig: ContentProvider unavailable for $key — trying SharedPrefs fallback")
          }






          return try {
              val moduleCtx = context.createPackageContext(
                  MODULE_PKG,
                  Context.CONTEXT_IGNORE_SECURITY
              )

              moduleCtx.getSharedPreferences("facegate_ipc", Context.MODE_PRIVATE)
                  .getString(key, null)

                  ?: moduleCtx.getSharedPreferences("facegate_prefs", Context.MODE_PRIVATE)
                      .getString(key, null)
          } catch (e: Throwable) {
              Logger.e("RemoteConfig.read($key): SharedPrefs fallback also failed", e)
              null
          }
      }

      fun getTargetPackage(context: Context): String?  = read(context, KEY_TARGET_PACKAGE)
      fun getStreamUrl(context: Context): String?       = read(context, KEY_STREAM_URL)
      fun getMediaUri(context: Context): String?        = read(context, KEY_MEDIA_URI)
      fun isInjectionActive(context: Context): Boolean  = read(context, KEY_INJECTION_ACTIVE) == "1"





      fun clearAll(context: Context) {
          setInjectionActive(context, false)
          setStreamUrl(context, null)
          setMediaUri(context, null)
      }
  }

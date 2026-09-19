package com.itsme.itsanon.utils

  import android.os.Environment
  import android.util.Log
  import timber.log.Timber
  import java.io.File
  import java.io.FileWriter
  import java.io.PrintWriter
  import java.text.SimpleDateFormat
  import java.util.Date
  import java.util.Locale
  import java.util.concurrent.Executors



  object Logger {


      const val HOOK      = "HOOK"
      const val DECODER   = "DECODER"
      const val UI        = "UI"
      const val INJECTION = "INJECTION"
      const val ROUTER    = "ROUTER"
      const val IPC       = "IPC"
      const val MAIN      = "MAIN"
      const val APP       = "APP"

      private const val LEGACY_TAG = "EcomCam"
      private const val MAX_HOOK_FILE_BYTES = 10L * 1024 * 1024

      @Volatile private var isXposedMode = false


      @Volatile private var hookLogFile: File? = null
      @Volatile private var hookOldLogFile: File? = null
      private val hookIoExecutor by lazy {
          Executors.newSingleThreadExecutor { r ->
              Thread(r, "FG-HookLog").apply { isDaemon = true }
          }
      }
      private val hookDateFmt = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US)

      fun init(xposedMode: Boolean) {
          isXposedMode = xposedMode
          if (xposedMode) {
              try {
                  val dir = File(
                      Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS),
                      "com.itsme.itsanon"
                  )
                  dir.mkdirs()
                  hookLogFile    = File(dir, "itsanon_hook_logs.txt")
                  hookOldLogFile = File(dir, "itsanon_hook_logs_old.txt")
              } catch (_: Throwable) {}
          }
      }



      fun v(tag: String, message: String) {
          if (isXposedMode) { hookLog(Log.VERBOSE, tag, message) }
          else              { Timber.tag(tag).v(message) }
      }

      fun d(tag: String, message: String) {
          if (isXposedMode) { hookLog(Log.DEBUG, tag, message) }
          else              { Timber.tag(tag).d(message) }
      }

      fun i(tag: String, message: String) {
          if (isXposedMode) { hookLog(Log.INFO, tag, message) }
          else              { Timber.tag(tag).i(message) }
      }

      fun w(tag: String, message: String, throwable: Throwable? = null) {
          if (isXposedMode) {
              hookLog(Log.WARN, tag, message)
              throwable?.let { hookLog(Log.WARN, tag, it.stackTraceToString()) }
          } else {
              if (throwable != null) Timber.tag(tag).w(throwable, message)
              else Timber.tag(tag).w(message)
          }
      }

      fun e(tag: String, message: String, throwable: Throwable? = null) {
          if (isXposedMode) {
              hookLog(Log.ERROR, tag, message)
              throwable?.let { hookLog(Log.ERROR, tag, it.stackTraceToString()) }
          } else {
              if (throwable != null) Timber.tag(tag).e(throwable, message)
              else Timber.tag(tag).e(message)
          }
      }



      fun d(message: String) = d(LEGACY_TAG, message)
      fun i(message: String) = i(LEGACY_TAG, message)
      fun w(message: String) = w(LEGACY_TAG, message)
      fun e(message: String, throwable: Throwable? = null) = e(LEGACY_TAG, message, throwable)

      fun logException(tag: String, throwable: Throwable) {
          e(tag, "Exception: ${throwable.message}", throwable)
      }



      private fun hookLog(priority: Int, tag: String, message: String) {
          val level = when (priority) {
              Log.VERBOSE -> "V"; Log.DEBUG -> "D"; Log.INFO  -> "I"
              Log.WARN    -> "W"; Log.ERROR -> "E"; else      -> "?"
          }

          val logFile    = hookLogFile    ?: return
          val oldLogFile = hookOldLogFile ?: return
          val thread     = Thread.currentThread().name
          val ts         = hookDateFmt.format(Date())
          val line       = "$ts $level/$tag [thread=$thread]: $message"

          hookIoExecutor.execute {
              try {
                  if (logFile.length() >= MAX_HOOK_FILE_BYTES) {
                      oldLogFile.delete()
                      logFile.renameTo(oldLogFile)
                  }
                  FileWriter(logFile, true).use { fw ->
                      PrintWriter(fw).use { pw -> pw.println(line) }
                  }
              } catch (_: Throwable) {}
          }
      }
  }

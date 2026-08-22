package com.itsme.amkush.utils

import android.util.Log
import timber.log.Timber
import java.io.File
import java.io.FileWriter
import java.io.PrintWriter
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

class FileLoggingTree(
    filesDir: File,

    private val minPriority: Int = Log.VERBOSE,

    logFileName: String = "amkush_logs.txt",
    oldLogFileName: String = "amkush_logs_old.txt"
) : Timber.Tree() {

    private val logFile = File(filesDir, logFileName)
    private val oldFile = File(filesDir, oldLogFileName)

    private val dateFmt = SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US)


    private val ioExecutor = Executors.newSingleThreadExecutor { r ->
        Thread(r, "AmkushFileLogger").apply { isDaemon = true }
    }

    companion object {
        private const val MAX_FILE_BYTES = 20L * 1024 * 1024
    }

    override fun isLoggable(tag: String?, priority: Int): Boolean = priority >= minPriority

    override fun log(priority: Int, tag: String?, message: String, t: Throwable?) {
        val level = when (priority) {
            Log.VERBOSE -> "V"
            Log.DEBUG   -> "D"
            Log.INFO    -> "I"
            Log.WARN    -> "W"
            Log.ERROR   -> "E"
            Log.ASSERT  -> "A"
            else        -> "?"
        }
        val timestamp = dateFmt.format(Date())
        val thread = Thread.currentThread().name
        val line = "$timestamp $level/$tag [thread=$thread]: $message"
        val stackTrace = t?.stackTraceToString()

        ioExecutor.execute {
            try {
                rotateIfNeeded()
                FileWriter(logFile,  true).use { fw ->
                    PrintWriter(fw).use { pw ->
                        pw.println(line)
                        if (stackTrace != null) {
                            pw.println(stackTrace)
                        }
                    }
                }
            } catch (_: Throwable) {



            }
        }
    }



    private fun rotateIfNeeded() {
        if (logFile.length() >= MAX_FILE_BYTES) {
            oldFile.delete()
            logFile.renameTo(oldFile)
        }
    }



    fun shutdown() {
        ioExecutor.shutdown()
        ioExecutor.awaitTermination(2, TimeUnit.SECONDS)
    }
}

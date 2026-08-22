package com.itsme.amkush.ipc

  import android.net.LocalServerSocket
  import android.net.LocalSocket
  import com.itsme.amkush.router.SurfaceRouter
  import com.itsme.amkush.utils.Logger
  import java.io.BufferedReader
  import java.io.DataOutputStream
  import java.io.InputStreamReader
  import java.io.PrintWriter
  import java.util.concurrent.Executors
  import java.util.concurrent.RejectedExecutionException
  import java.util.concurrent.ThreadPoolExecutor
  import java.util.concurrent.TimeUnit



  object UnixSocketServer {

      private const val TAG           = "UnixSocketServer"
      const val CTRL_NAME             = "facegate_vcam_ctrl"
      const val DATA_NAME_FMT         = "facegate_vcam_%d"

      @Volatile private var running   = false
      private var controlServer: LocalServerSocket? = null



      @Volatile private var executor: ThreadPoolExecutor = newExecutor()


      private fun newExecutor(): ThreadPoolExecutor =
          Executors.newCachedThreadPool { r ->
              Thread(r, "FG-SockSrv").apply { isDaemon = true }
          } as ThreadPoolExecutor



      fun start() {
          if (running) return
          running = true




          if (executor.isShutdown || executor.isTerminated) {
              executor = newExecutor()
              Logger.d(TAG, "Executor was terminated; created a fresh thread pool")
          }

          safeExecute(::acceptControlClients)
          Logger.i(TAG, "UnixSocketServer started — ctrl=$CTRL_NAME")
      }

      fun stop() {
          running = false
          runCatching { controlServer?.close() }
          executor.shutdownNow()
          runCatching { executor.awaitTermination(2, TimeUnit.SECONDS) }
          Logger.i(TAG, "UnixSocketServer stopped")
      }



      private fun safeExecute(block: () -> Unit) {
          try {
              executor.execute(block)
          } catch (e: RejectedExecutionException) {
              Logger.e(TAG, "Task rejected — executor may be shutting down: ${e.message}")
          }
      }



      private fun acceptControlClients() {
          try {
              val srv = LocalServerSocket(CTRL_NAME)
              controlServer = srv
              Logger.d(TAG, "Control socket bound, waiting for clients")
              while (running) {
                  val client = try { srv.accept() } catch (_: Throwable) { break }
                  Logger.d(TAG, "Control client connected")
                  try { client.receiveBufferSize = 8 * 1024 * 1024 } catch (_: Throwable) {}
                  try { client.sendBufferSize   = 8 * 1024 * 1024 } catch (_: Throwable) {}
                  safeExecute { handleControlClient(client) }
              }
          } catch (e: Throwable) {
              if (running) Logger.e(TAG, "Control accept loop: ${e.message}", e)
          }
      }



      private fun handleControlClient(ctrl: LocalSocket) {
          val sessionId  = "sock_${System.currentTimeMillis()}"
          val reader     = BufferedReader(InputStreamReader(ctrl.inputStream))
          val writer     = PrintWriter(ctrl.outputStream, true)
          val dataServers = mutableMapOf<Int, LocalServerSocket>()

          Logger.d(TAG, "[$sessionId] session started")
          try {
              while (running) {
                  val line = reader.readLine() ?: break
                  Logger.d(TAG, "[$sessionId] recv: $line")

                  when {
                      line.startsWith("SURFACE ") -> {

                          val parts  = line.trim().split(" ")
                          val index  = parts[1].toInt()
                          val w      = parts[2].removePrefix("width=").toInt()
                          val h      = parts[3].removePrefix("height=").toInt()
                          val fmt    = parts[4].removePrefix("fmt=").toInt()

                          val dataName = DATA_NAME_FMT.format(index)
                          val dataSrv  = LocalServerSocket(dataName)
                          dataServers[index] = dataSrv

                          safeExecute { acceptDataClient(dataSrv, index, w, h, fmt, sessionId) }

                          writer.println("READY $index")
                          Logger.d(TAG, "[$sessionId] READY $index (${w}x${h} fmt=$fmt)")
                      }

                      line.startsWith("STOP ") -> {
                          val index = line.removePrefix("STOP ").trim().toIntOrNull() ?: continue
                          Logger.d(TAG, "[$sessionId] STOP $index")
                          dataServers.remove(index)?.runCatching { close() }
                          if (dataServers.isEmpty()) break
                      }

                      line == "STOP_ALL" -> break
                  }
              }
          } catch (e: Throwable) {
              if (running) Logger.e(TAG, "[$sessionId] handler error: ${e.message}", e)
          } finally {
              SurfaceRouter.unregisterSocketSession(sessionId)
              dataServers.values.forEach { runCatching { it.close() } }
              runCatching { ctrl.close() }
              Logger.d(TAG, "[$sessionId] session ended")
          }
      }

      private fun acceptDataClient(
          srv: LocalServerSocket,
          index: Int, w: Int, h: Int, fmt: Int,
          sessionId: String
      ) {
          try {
              val client = srv.accept()
              try { client.receiveBufferSize = 8 * 1024 * 1024 } catch (_: Throwable) {}
              try { client.sendBufferSize   = 8 * 1024 * 1024 } catch (_: Throwable) {}
              val out  = DataOutputStream(client.outputStream.buffered(512 * 1024))
              val spec = SurfaceRouter.SocketSurfaceSpec(index, w, h, fmt, out)
              SurfaceRouter.addSocketSurface(sessionId, spec)
              Logger.d(TAG, "[$sessionId] data[$index] connected ${w}x${h} fmt=$fmt")
          } catch (e: Throwable) {
              Logger.e(TAG, "[$sessionId] data[$index] accept error: ${e.message}", e)
          } finally {
              runCatching { srv.close() }
          }
      }
  }

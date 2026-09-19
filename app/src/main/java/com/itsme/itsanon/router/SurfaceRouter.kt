package com.itsme.itsanon.router

import android.graphics.ImageFormat
import android.media.Image
import android.media.ImageWriter
import android.view.Surface
import com.itsme.itsanon.media.ItsAnonDecoder
import com.itsme.itsanon.libyuv.LibYuv
import com.itsme.itsanon.utils.Logger
import java.nio.ByteBuffer
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong

object SurfaceRouter : ItsAnonDecoder.FrameCallback {

    private const val TAG = "SurfaceRouter"


    data class MasterFrame(
        val y: ByteBuffer,
        val u: ByteBuffer,
        val v: ByteBuffer,
        val width: Int,
        val height: Int,
        val seq: Long
    )

    private val frameSeq = AtomicLong(0L)


      private val sessions: ConcurrentHashMap<String, List<SurfaceFeedThread>> = ConcurrentHashMap()


      private val socketSessions: ConcurrentHashMap<String, MutableList<SocketFeedThread>> = ConcurrentHashMap()



    override fun onFrameAvailable(
        yBuf: ByteBuffer,
        uBuf: ByteBuffer,
        vBuf: ByteBuffer,
        width: Int,
        height: Int,
        ptsUs: Long
    ) {


        val ySize  = width * height
        val uvW    = (width + 1) / 2
        val uvH    = (height + 1) / 2
        val uvSize = uvW * uvH

        val seq = frameSeq.incrementAndGet()

        if (seq % 90L == 0L) {
            Logger.v(Logger.ROUTER, "$TAG frame #$seq  ${width}x${height}  ptsUs=$ptsUs  sessions=${sessions.size}")
        }

        val yCopy = ByteBuffer.allocateDirect(ySize).also  { it.put(yBuf.duplicate()); it.flip() }
        val uCopy = ByteBuffer.allocateDirect(uvSize).also { it.put(uBuf.duplicate()); it.flip() }
        val vCopy = ByteBuffer.allocateDirect(uvSize).also { it.put(vBuf.duplicate()); it.flip() }

        val frame = MasterFrame(yCopy, uCopy, vCopy, width, height, seq)


          sessions.values.forEach { threads ->
              threads.forEach { it.offerFrame(frame) }
          }

          socketSessions.values.forEach { threads ->
              threads.forEach { it.offerFrame(frame) }
          }
    }

    override fun onError(code: Int, msg: String) {
        Logger.e(Logger.ROUTER, "$TAG decoder error code=$code  msg=$msg")
    }

    override fun onEof() {
        Logger.d(Logger.ROUTER, "$TAG decoder EOF — file sources will loop")
    }



    fun registerSession(
        sessionId: String,
        surfaces: List<Surface>,
        widths: IntArray,
        heights: IntArray,
        formats: IntArray,
        fps: IntArray
    ) {
        Logger.d(Logger.ROUTER, "$TAG registerSession: id=$sessionId  surfaces=${surfaces.size}  dims=${widths.zip(heights).map { "${it.first}x${it.second}" }}  formats=${formats.toList()}  fps=${fps.toList()}")
        unregisterSession(sessionId)

        val threads = surfaces.mapIndexedNotNull { i, surface ->
            try {
                SurfaceFeedThread(
                    sessionId = sessionId,
                    surface   = surface,
                    width     = widths.getOrElse(i)  { 1280 },
                    height    = heights.getOrElse(i) { 720  },
                    format    = formats.getOrElse(i) { ImageFormat.YUV_420_888 },
                    fps       = fps.getOrElse(i)     { 30   }
                ).also { it.start() }
            } catch (e: Throwable) {
                Logger.e("$TAG SurfaceFeedThread[$i] init failed: ${e.message}")
                null
            }
        }

        sessions[sessionId] = threads
        Logger.d("$TAG registered session=$sessionId  threads=${threads.size}")
    }

    fun unregisterSession(sessionId: String) {
        Logger.d(Logger.ROUTER, "$TAG unregisterSession: $sessionId")
        sessions.remove(sessionId)?.forEach { it.stopThread() }
        Logger.d("$TAG unregistered session=$sessionId")
    }

    fun unregisterAll() {
        sessions.keys.toList().forEach { unregisterSession(it) }
        socketSessions.keys.toList().forEach { unregisterSocketSession(it) }
        frameSeq.set(0L)
    }




      fun registerSocketSession(sessionId: String, specs: List<SocketSurfaceSpec>) {
          Logger.d(Logger.ROUTER, "$TAG registerSocketSession: id=$sessionId  surfaces=${specs.size}")
          unregisterSocketSession(sessionId)
          val threads = specs.map { spec ->
              SocketFeedThread(sessionId, spec).also { it.start() }
          }
          socketSessions[sessionId] = threads.toMutableList()
      }



      fun addSocketSurface(sessionId: String, spec: SocketSurfaceSpec) {
          Logger.d(Logger.ROUTER, "$TAG addSocketSurface: id=$sessionId  index=${spec.index}  ${spec.width}x${spec.height}")
          socketSessions.getOrPut(sessionId) { mutableListOf() }
              .add(SocketFeedThread(sessionId, spec).also { it.start() })
      }

      fun unregisterSocketSession(sessionId: String) {
          Logger.d(Logger.ROUTER, "$TAG unregisterSocketSession: $sessionId")
          socketSessions.remove(sessionId)?.forEach { it.stopThread() }
      }


      data class SocketSurfaceSpec(
          val index: Int,
          val width: Int,
          val height: Int,
          val format: Int,
          val outputStream: java.io.DataOutputStream
      )
}

private class SurfaceFeedThread(
    private val sessionId: String,
    private val surface: Surface,
    private val width: Int,
    private val height: Int,
    private val format: Int,
    fps: Int
) : Thread("SurfaceFeed-${sessionId.take(8)}-${width}x${height}") {

    @Volatile private var running = true


    private val queue = LinkedBlockingQueue<SurfaceRouter.MasterFrame>(1)


    private val outputBuf: ByteBuffer =
        ByteBuffer.allocateDirect(LibYuv.outputSize(width, height, format))

    private val pacer = FpsPacer(fps)
    private var writer: ImageWriter? = null

    init {
        isDaemon = true
        priority = MAX_PRIORITY - 1
    }


    fun offerFrame(frame: SurfaceRouter.MasterFrame) {
        queue.poll()
        queue.offer(frame)
    }

    fun stopThread() {
        running = false
        queue.offer(SurfaceRouter.MasterFrame(
            ByteBuffer.allocate(0), ByteBuffer.allocate(0), ByteBuffer.allocate(0),
            0, 0, Long.MIN_VALUE
        ))
        try { writer?.close() } catch (_: Throwable) {}
    }

    override fun run() {
        try {
            writer = ImageWriter.newInstance(surface, 2, format)
        } catch (e: Throwable) {
            Logger.e("SurfaceFeed[$sessionId] ImageWriter init: ${e.message}")
            return
        }

        Logger.d("SurfaceFeed[$sessionId] started ${width}x${height} fmt=$format")

        while (running) {
            val frame = try {
                queue.poll(300, TimeUnit.MILLISECONDS)
            } catch (_: InterruptedException) {
                break
            } ?: continue

            if (frame.seq == Long.MIN_VALUE) break
            pacer.pace()
            pushFrame(frame)
        }

        try { writer?.close() } catch (_: Throwable) {}
        Logger.d("SurfaceFeed[$sessionId] stopped")
    }

    private fun pushFrame(frame: SurfaceRouter.MasterFrame) {
        val w = writer ?: return
        try {
            outputBuf.clear()

            val y = frame.y.duplicate().apply { rewind() }
            val u = frame.u.duplicate().apply { rewind() }
            val v = frame.v.duplicate().apply { rewind() }


            val srcStrideY = frame.width
            val srcStrideU = (frame.width + 1) / 2
            val srcStrideV = (frame.width + 1) / 2


            val rc = LibYuv.convertInto(
                y, u, v,
                frame.width, frame.height,
                srcStrideY, srcStrideU, srcStrideV,
                width, height,
                format,
                outputBuf
            )
            if (rc != 0) {
                Logger.e("SurfaceFeed LibYuv.convertInto: rc=$rc (${LibYuv.getErrorMessage(rc)})")
                return
            }
            outputBuf.flip()

            val image: Image = try {
                w.dequeueInputImage()
            } catch (e: IllegalStateException) {
                return
            }

            try {
                fillImagePlanes(image)
                w.queueInputImage(image)
            } catch (e: Throwable) {
                try { image.close() } catch (_: Throwable) {}
                Logger.e("SurfaceFeed queueInputImage: ${e.message}")
            }
        } catch (e: Throwable) {
            Logger.e("SurfaceFeed pushFrame: ${e.message}")
        }
    }



    private fun fillImagePlanes(image: Image) {
        val planes = image.planes
        when (format) {
            ImageFormat.YUV_420_888 -> {

                val ySize = width * height

                val uvW = (width + 1) / 2
                val uvH = (height + 1) / 2
                val uvSize = uvW * uvH

                fillPlane(planes[0], outputBuf, 0,               width, height)
                fillPlane(planes[1], outputBuf, ySize,           uvW,   uvH)
                fillPlane(planes[2], outputBuf, ySize + uvSize,  uvW,   uvH)
            }
            ImageFormat.NV21, 0x15  -> {

                val ySize = width * height
                fillPlane(planes[0], outputBuf, 0, width, height)


                val uvPlane = planes[1]
                val uvBuf = uvPlane.buffer
                outputBuf.position(ySize)


                val uvW = (width + 1) / 2
                val uvH = (height + 1) / 2
                val uvData = ByteArray(uvW * uvH * 2)
                outputBuf.get(uvData)

                val uvStride    = uvPlane.rowStride
                val uvPixStride = uvPlane.pixelStride

                for (row in 0 until uvH) {
                    for (col in 0 until uvW) {
                        val srcIdx = row * uvW * 2 + col * 2
                        val dstIdx = row * uvStride + col * uvPixStride
                        if (srcIdx + 1 < uvData.size && dstIdx + 1 < uvBuf.capacity()) {
                            uvBuf.put(dstIdx, uvData[srcIdx])
                            uvBuf.put(dstIdx + 1, uvData[srcIdx + 1])
                        }
                    }
                }
            }
            ImageFormat.NV16 -> {

                val ySize = width * height
                fillPlane(planes[0], outputBuf, 0, width, height)

                val uvPlane = planes[1]
                val uvBuf = uvPlane.buffer
                outputBuf.position(ySize)


                val uvW = (width + 1) / 2
                val uvData = ByteArray(uvW * 2 * height)
                outputBuf.get(uvData)

                val uvStride    = uvPlane.rowStride
                val uvPixStride = uvPlane.pixelStride

                for (row in 0 until height) {
                    for (col in 0 until uvW) {
                        val srcIdx = row * uvW * 2 + col * 2
                        val dstIdx = row * uvStride + col * uvPixStride
                        if (srcIdx + 1 < uvData.size && dstIdx + 1 < uvBuf.capacity()) {
                            uvBuf.put(dstIdx, uvData[srcIdx])
                            uvBuf.put(dstIdx + 1, uvData[srcIdx + 1])
                        }
                    }
                }
            }
            ImageFormat.JPEG -> {
                val jpegData = ByteArray(outputBuf.remaining())
                outputBuf.get(jpegData)
                planes[0].buffer.put(jpegData)
            }
            else -> {

                planes[0].buffer.put(outputBuf)
            }
        }
    }



    private fun fillPlane(
        plane: Image.Plane,
        src: ByteBuffer,
        srcOffset: Int,
        blockWidth: Int,
        blockHeight: Int
    ) {
        val dest      = plane.buffer
        val rowStride = plane.rowStride
        src.position(srcOffset)
        if (rowStride == blockWidth) {
            val data = ByteArray(blockWidth * blockHeight)
            src.get(data)
            dest.put(data)
        } else {
            val row = ByteArray(blockWidth)
            for (r in 0 until blockHeight) {
                src.position(srcOffset + r * blockWidth)
                src.get(row)
                dest.position(r * rowStride)
                dest.put(row)
            }
        }
    }
}






  private class SocketFeedThread(
      private val sessionId: String,
      private val spec: SurfaceRouter.SocketSurfaceSpec
  ) : Thread("SocketFeed-${sessionId.take(8)}-${spec.width}x${spec.height}") {

      @Volatile private var running = true
      private val queue = java.util.concurrent.LinkedBlockingQueue<SurfaceRouter.MasterFrame>(1)

      private val outputBuf: java.nio.ByteBuffer =
          java.nio.ByteBuffer.allocateDirect(LibYuv.outputSize(spec.width, spec.height, spec.format))

      init {
          isDaemon = true
          priority = MAX_PRIORITY - 1
      }

      fun offerFrame(frame: SurfaceRouter.MasterFrame) {
          queue.poll()
          queue.offer(frame)
      }

      fun stopThread() {
          running = false
          queue.offer(SurfaceRouter.MasterFrame(
              java.nio.ByteBuffer.allocate(0), java.nio.ByteBuffer.allocate(0), java.nio.ByteBuffer.allocate(0),
              0, 0, Long.MIN_VALUE
          ))
          runCatching { spec.outputStream.close() }
      }

      override fun run() {
          Logger.d("SocketFeed[$sessionId] started ${spec.width}x${spec.height} fmt=${spec.format}")
          while (running) {
              val frame = try {
                  queue.poll(300, java.util.concurrent.TimeUnit.MILLISECONDS)
              } catch (_: InterruptedException) { break } ?: continue

              if (frame.seq == Long.MIN_VALUE) break
              pushFrame(frame)
          }
          runCatching { spec.outputStream.close() }
          Logger.d("SocketFeed[$sessionId] stopped")
      }

      private fun pushFrame(frame: SurfaceRouter.MasterFrame) {
          try {
              outputBuf.clear()
              val y = frame.y.duplicate().apply { rewind() }
              val u = frame.u.duplicate().apply { rewind() }
              val v = frame.v.duplicate().apply { rewind() }

              val rc = LibYuv.convertInto(
                  y, u, v,
                  frame.width, frame.height,
                  frame.width, (frame.width + 1) / 2, (frame.width + 1) / 2,
                  spec.width, spec.height, spec.format,
                  outputBuf
              )
              if (rc != 0) {
                  Logger.e("SocketFeed LibYuv.convertInto rc=$rc (${LibYuv.getErrorMessage(rc)})")
                  return
              }
              outputBuf.flip()

              val len   = outputBuf.remaining()
              val bytes = ByteArray(len)
              outputBuf.get(bytes)

              spec.outputStream.writeInt(len)
              spec.outputStream.write(bytes)
              spec.outputStream.flush()
          } catch (e: Throwable) {
              if (running) Logger.e("SocketFeed[$sessionId] pushFrame: ${e.message}")
              running = false
          }
      }
  }

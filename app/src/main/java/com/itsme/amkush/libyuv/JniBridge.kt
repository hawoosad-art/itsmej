package com.itsme.amkush.libyuv

  import android.graphics.ImageFormat
  import android.graphics.PixelFormat
  import java.nio.ByteBuffer



  object LibYuv {

      init {
          System.loadLibrary("libyuv_wrapper")
      }

      const val ERR_NULL_BUFFER     = -1
      const val ERR_UNSUPPORTED_FMT = -2
      const val ERR_DST_TOO_SMALL   = -3
      const val ERR_INVALID_DIMS    = -4
      const val ERR_INVALID_STRIDE  = -5
      const val ERR_SRC_TOO_SMALL   = -6
      const val ERR_OOM             = -7

      external fun convertInto(
          srcY: ByteBuffer, srcU: ByteBuffer, srcV: ByteBuffer,
          srcW: Int, srcH: Int,
          srcStrideY: Int, srcStrideU: Int, srcStrideV: Int,
          dstW: Int, dstH: Int,
          dstFmt: Int,
          dst: ByteBuffer
      ): Int

      fun outputSize(width: Int, height: Int, format: Int): Int {
          require(width > 0 && height > 0) { "Dimensions must be positive" }
          val ySize = width * height
          return when (format) {
              PixelFormat.RGBA_8888          -> ySize * 4
              ImageFormat.RGB_565            -> ySize * 2
              ImageFormat.NV16               -> ySize + 2 * (((width + 1) / 2) * height)
              ImageFormat.NV21,
              ImageFormat.YUV_420_888,
              0x15                -> ySize + 2 * (((width + 1) / 2) * ((height + 1) / 2))
              else                           -> ySize + 2 * (((width + 1) / 2) * ((height + 1) / 2))
          }
      }

      fun getErrorMessage(code: Int): String = when (code) {
          ERR_NULL_BUFFER     -> "Null DirectByteBuffer address"
          ERR_UNSUPPORTED_FMT -> "Unsupported destination format"
          ERR_DST_TOO_SMALL   -> "Destination buffer is too small"
          ERR_INVALID_DIMS    -> "Invalid width/height dimensions"
          ERR_INVALID_STRIDE  -> "Source strides are smaller than widths"
          ERR_SRC_TOO_SMALL   -> "Source buffers are too small for declared strides"
          ERR_OOM             -> "Out of memory (malloc failed)"
          else                -> "Unknown native error: $code"
      }
  }

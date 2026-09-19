#include <jni.h>
#include <android/log.h>
#include <libyuv/convert.h>
#include <libyuv/convert_argb.h>
#include <libyuv/convert_from.h>
#include <libyuv/scale.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#define LOG_TAG "LibYuvWrapper"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGFG(...) __android_log_print(ANDROID_LOG_DEBUG, "ECOMCAM", __VA_ARGS__)

static const int FMT_RGBA_8888   = 0x01;
static const int FMT_RGB_565     = 0x04;
static const int FMT_NV16        = 0x10;
static const int FMT_NV21        = 0x11;
static const int FMT_NV12        = 0x15;
static const int FMT_YUV_420_888 = 0x23;

static const int ERR_NULL_BUFFER     = -1;
static const int ERR_UNSUPPORTED_FMT = -2;
static const int ERR_DST_TOO_SMALL   = -3;
static const int ERR_INVALID_DIMS    = -4;
static const int ERR_INVALID_STRIDE  = -5;
static const int ERR_SRC_TOO_SMALL   = -6;
static const int ERR_OOM             = -7;

static size_t requiredDstSize(int w, int h, int fmt) {
    const size_t y = static_cast<size_t>(w) * h;
    const size_t uvW = static_cast<size_t>((w + 1) / 2);
    const size_t uvH = static_cast<size_t>((h + 1) / 2);
    switch (fmt) {
        case FMT_RGBA_8888: return y * 4;
        case FMT_RGB_565: return y * 2;
        case FMT_NV16: return y + uvW * 2 * h;
        case FMT_YUV_420_888:
        case FMT_NV21:
        case FMT_NV12: return y + uvW * uvH * 2;
        default: return 0;
    }
}

static thread_local std::vector<uint8_t> g_tmpY;
static thread_local std::vector<uint8_t> g_tmpU;
static thread_local std::vector<uint8_t> g_tmpV;

static bool scaleToI420(const uint8_t* srcY, int srcStrideY,
                        const uint8_t* srcU, int srcStrideU,
                        const uint8_t* srcV, int srcStrideV,
                        int srcW, int srcH,
                        int dstW, int dstH,
                        const uint8_t** outY, const uint8_t** outU,
                        const uint8_t** outV) {
    const int dstUvW = (dstW + 1) / 2;
    const int dstUvH = (dstH + 1) / 2;
    try {
        g_tmpY.resize(static_cast<size_t>(dstW) * dstH);
        g_tmpU.resize(static_cast<size_t>(dstUvW) * dstUvH);
        g_tmpV.resize(static_cast<size_t>(dstUvW) * dstUvH);
    } catch (...) {
        return false;
    }

    const int rc = libyuv::I420Scale(
        srcY, srcStrideY, srcU, srcStrideU, srcV, srcStrideV,
        srcW, srcH,
        g_tmpY.data(), dstW,
        g_tmpU.data(), dstUvW,
        g_tmpV.data(), dstUvW,
        dstW, dstH,
        libyuv::kFilterBilinear);
    if (rc != 0) return false;
    *outY = g_tmpY.data();
    *outU = g_tmpU.data();
    *outV = g_tmpV.data();
    return true;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_itsme_amkush_libyuv_LibYuv_convertInto(
    JNIEnv* env, jclass,
    jobject srcYBuf, jobject srcUBuf, jobject srcVBuf,
    jint srcW, jint srcH,
    jint srcStrideY, jint srcStrideU, jint srcStrideV,
    jint dstW, jint dstH,
    jint dstFmt,
    jobject dstBuf) {
    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0 ||
        srcW > 16384 || srcH > 16384 || dstW > 16384 || dstH > 16384)
        return ERR_INVALID_DIMS;
    if (srcStrideY < srcW || srcStrideU < (srcW + 1) / 2 ||
        srcStrideV < (srcW + 1) / 2)
        return ERR_INVALID_STRIDE;

    auto* srcY = static_cast<const uint8_t*>(env->GetDirectBufferAddress(srcYBuf));
    auto* srcU = static_cast<const uint8_t*>(env->GetDirectBufferAddress(srcUBuf));
    auto* srcV = static_cast<const uint8_t*>(env->GetDirectBufferAddress(srcVBuf));
    auto* dst = static_cast<uint8_t*>(env->GetDirectBufferAddress(dstBuf));
    if (!srcY || !srcU || !srcV || !dst) return ERR_NULL_BUFFER;

    const size_t required = requiredDstSize(dstW, dstH, dstFmt);
    if (required == 0) return ERR_UNSUPPORTED_FMT;
    if (env->GetDirectBufferCapacity(dstBuf) < static_cast<jlong>(required))
        return ERR_DST_TOO_SMALL;

    const int srcUvH = (srcH + 1) / 2;
    if (env->GetDirectBufferCapacity(srcYBuf) < static_cast<jlong>(srcStrideY) * srcH ||
        env->GetDirectBufferCapacity(srcUBuf) < static_cast<jlong>(srcStrideU) * srcUvH ||
        env->GetDirectBufferCapacity(srcVBuf) < static_cast<jlong>(srcStrideV) * srcUvH)
        return ERR_SRC_TOO_SMALL;

    LOGFG("LibYuv convertInto: %dx%d -> %dx%d fmt=0x%x", srcW, srcH, dstW, dstH, dstFmt);

    const uint8_t* y = srcY;
    const uint8_t* u = srcU;
    const uint8_t* v = srcV;
    int strideY = srcStrideY;
    int strideU = srcStrideU;
    int strideV = srcStrideV;
    if (srcW != dstW || srcH != dstH) {
        if (!scaleToI420(srcY, srcStrideY, srcU, srcStrideU, srcV, srcStrideV,
                        srcW, srcH, dstW, dstH, &y, &u, &v))
            return ERR_OOM;
        strideY = dstW;
        strideU = (dstW + 1) / 2;
        strideV = strideU;
    }

    const int uvW = (dstW + 1) / 2;
    const int uvH = (dstH + 1) / 2;
    const size_t ySize = static_cast<size_t>(dstW) * dstH;
    const size_t uvSize = static_cast<size_t>(uvW) * uvH;
    int rc = 0;

    switch (dstFmt) {
        case FMT_YUV_420_888:
            for (int row = 0; row < dstH; ++row)
                std::memcpy(dst + static_cast<size_t>(row) * dstW,
                            y + static_cast<size_t>(row) * strideY, dstW);
            for (int row = 0; row < uvH; ++row) {
                std::memcpy(dst + ySize + static_cast<size_t>(row) * uvW,
                            u + static_cast<size_t>(row) * strideU, uvW);
                std::memcpy(dst + ySize + uvSize + static_cast<size_t>(row) * uvW,
                            v + static_cast<size_t>(row) * strideV, uvW);
            }
            break;
        case FMT_NV12:
            rc = libyuv::I420ToNV12(y, strideY, u, strideU, v, strideV,
                                    dst, dstW, dst + ySize, uvW * 2, dstW, dstH);
            break;
        case FMT_NV21:
            rc = libyuv::I420ToNV21(y, strideY, u, strideU, v, strideV,
                                    dst, dstW, dst + ySize, uvW * 2, dstW, dstH);
            break;
        case FMT_RGBA_8888:
            /* [V101 COLOR] Android Bitmap.Config.ARGB_8888 + copyPixelsFromBuffer
             * reads bytes in memory order R,G,B,A (Android's N32 is RGBA8888, NOT
             * BGRA like desktop Skia). The V95 switch to I420ToARGB (which writes
             * B,G,R,A) therefore swapped red<->blue and gave the whole preview a
             * blue tint (tablet, log 14.unisoc). I420ToRGBA writes R,G,B,A, which
             * matches the bitmap. Injection path untouched (NV21/YUV_420_888). */
            rc = libyuv::I420ToRGBA(y, strideY, u, strideU, v, strideV,
                                    dst, dstW * 4, dstW, dstH);
            break;
        case FMT_RGB_565:
            rc = libyuv::I420ToRGB565(y, strideY, u, strideU, v, strideV,
                                      dst, dstW * 2, dstW, dstH);
            break;
        case FMT_NV16: {
            // NV16 is full-height interleaved U/V. libyuv's I420ToNV12 gives
            // the first chroma row; duplicate the nearest I420 chroma row for
            // each output row while preserving U/V ordering.
            for (int row = 0; row < dstH; ++row)
                std::memcpy(dst + static_cast<size_t>(row) * dstW,
                            y + static_cast<size_t>(row) * strideY, dstW);
            uint8_t* outUv = dst + ySize;
            for (int row = 0; row < dstH; ++row) {
                const int srcRow = std::min(row / 2, uvH - 1);
                for (int col = 0; col < uvW; ++col) {
                    outUv[static_cast<size_t>(row) * uvW * 2 + col * 2] =
                        u[static_cast<size_t>(srcRow) * strideU + col];
                    outUv[static_cast<size_t>(row) * uvW * 2 + col * 2 + 1] =
                        v[static_cast<size_t>(srcRow) * strideV + col];
                }
            }
            break;
        }
        default:
            return ERR_UNSUPPORTED_FMT;
    }
    return rc == 0 ? 0 : -10;
}

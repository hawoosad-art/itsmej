#include <jni.h>
#include <android/log.h>
#include <cstdint>
#include <cstring>
#include <limits.h>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>
#include <libavutil/imgutils.h>
}

#define LOG_TAG "SwScaleWrapper"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGFG(...) __android_log_print(ANDROID_LOG_DEBUG, "FACEGATE", __VA_ARGS__)

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

static AVPixelFormat toAVFmt(int androidFmt) {
    switch (androidFmt) {
        case FMT_YUV_420_888: return AV_PIX_FMT_YUV420P;
        case FMT_NV21:        return AV_PIX_FMT_NV21;
        case FMT_NV12:        return AV_PIX_FMT_NV12;
        case FMT_NV16:        return AV_PIX_FMT_NV16;
        case FMT_RGBA_8888:   return AV_PIX_FMT_RGBA;
        case FMT_RGB_565:     return AV_PIX_FMT_RGB565LE;
        default:              return AV_PIX_FMT_NONE;
    }
}

static size_t requiredDstSize(int w, int h, int fmt) {
    const size_t y   = (size_t)w * h;
    const size_t uvW = (size_t)((w + 1) / 2);
    const size_t uvH = (size_t)((h + 1) / 2);
    switch (fmt) {
        case FMT_RGBA_8888:   return y * 4;
        case FMT_RGB_565:     return y * 2;
        case FMT_NV16:        return y + uvW * 2 * h;
        case FMT_YUV_420_888:
        case FMT_NV21:
        case FMT_NV12:        return y + uvW * uvH * 2;
        default:              return y + uvW * uvH * 2;
    }
}

struct SwsCache {
    SwsContext*   ctx     = nullptr;
    int           srcW    = 0, srcH    = 0;
    int           dstW    = 0, dstH    = 0;
    AVPixelFormat srcFmt  = AV_PIX_FMT_NONE;
    AVPixelFormat dstFmt  = AV_PIX_FMT_NONE;




    ~SwsCache() {
        if (ctx) { sws_freeContext(ctx); ctx = nullptr; }
    }

    SwsCache() = default;
    SwsCache(const SwsCache&)            = delete;
    SwsCache& operator=(const SwsCache&) = delete;
};
static thread_local SwsCache g_cache;

static SwsContext* getCachedCtx(
    int srcW, int srcH, AVPixelFormat srcFmt,
    int dstW, int dstH, AVPixelFormat dstFmt)
{
    if (g_cache.ctx      &&
        g_cache.srcW == srcW && g_cache.srcH == srcH && g_cache.srcFmt == srcFmt &&
        g_cache.dstW == dstW && g_cache.dstH == dstH && g_cache.dstFmt == dstFmt) {
        return g_cache.ctx;
    }
    if (g_cache.ctx) sws_freeContext(g_cache.ctx);

    g_cache.ctx = sws_getContext(
        srcW, srcH, srcFmt,
        dstW, dstH, dstFmt,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    g_cache.srcW = srcW; g_cache.srcH = srcH; g_cache.srcFmt = srcFmt;
    g_cache.dstW = dstW; g_cache.dstH = dstH; g_cache.dstFmt = dstFmt;
    return g_cache.ctx;
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_itsme_amkush_libyuv_LibYuv_convertInto(
    JNIEnv* env, jclass,
    jobject srcYBuf, jobject srcUBuf, jobject srcVBuf,
    jint srcW, jint srcH,
    jint srcStrideY, jint srcStrideU, jint srcStrideV,
    jint dstW, jint dstH,
    jint dstFmt,
    jobject dstBuf)
{



    if (srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) {
        LOGE("convertInto: invalid dimensions %dx%d -> %dx%d", srcW, srcH, dstW, dstH);
        return ERR_INVALID_DIMS;
    }

    if (srcW > 16384 || srcH > 16384 || dstW > 16384 || dstH > 16384) {
        LOGE("convertInto: dimensions too large %dx%d -> %dx%d", srcW, srcH, dstW, dstH);
        return ERR_INVALID_DIMS;
    }

    if (srcStrideY <= 0 || srcStrideU <= 0 || srcStrideV <= 0) {
        LOGE("convertInto: invalid strides Y=%d U=%d V=%d", srcStrideY, srcStrideU, srcStrideV);
        return ERR_INVALID_STRIDE;
    }

    auto* srcY = static_cast<uint8_t*>(env->GetDirectBufferAddress(srcYBuf));
    auto* srcU = static_cast<uint8_t*>(env->GetDirectBufferAddress(srcUBuf));
    auto* srcV = static_cast<uint8_t*>(env->GetDirectBufferAddress(srcVBuf));
    auto* dst  = static_cast<uint8_t*>(env->GetDirectBufferAddress(dstBuf));

    LOGFG("SwScale convertInto: %dx%d -> %dx%d fmt=0x%x", srcW, srcH, dstW, dstH, dstFmt);

    if (!srcY || !srcU || !srcV || !dst) {
        LOGE("convertInto: null DirectByteBuffer address");
        return ERR_NULL_BUFFER;
    }

    const AVPixelFormat avDst = toAVFmt(dstFmt);
    if (avDst == AV_PIX_FMT_NONE) {
        LOGE("convertInto: unsupported format 0x%x", dstFmt);
        return ERR_UNSUPPORTED_FMT;
    }

    jlong dstCap = env->GetDirectBufferCapacity(dstBuf);
    if (dstCap < (jlong)requiredDstSize(dstW, dstH, dstFmt)) {
        LOGE("convertInto: dst buffer too small");
        return ERR_DST_TOO_SMALL;
    }





    {
        const jlong srcYCap = env->GetDirectBufferCapacity(srcYBuf);
        const jlong srcUCap = env->GetDirectBufferCapacity(srcUBuf);
        const jlong srcVCap = env->GetDirectBufferCapacity(srcVBuf);
        const int64_t needY = (int64_t)srcStrideY * srcH;
        const int64_t needU = (int64_t)srcStrideU * ((srcH + 1) / 2);
        const int64_t needV = (int64_t)srcStrideV * ((srcH + 1) / 2);
        if (srcYCap < needY || srcUCap < needU || srcVCap < needV) {
            LOGE("convertInto: src buffer too small "
                 "(Y %lld<%lld U %lld<%lld V %lld<%lld)",
                 (long long)srcYCap, (long long)needY,
                 (long long)srcUCap, (long long)needU,
                 (long long)srcVCap, (long long)needV);
            return ERR_SRC_TOO_SMALL;
        }
    }

    SwsContext* ctx = getCachedCtx(srcW, srcH, AV_PIX_FMT_YUV420P, dstW, dstH, avDst);
    if (!ctx) {
        LOGE("sws_getContext failed");
        return ERR_OOM;
    }


    const uint8_t* srcSlice[4] = { srcY, srcU, srcV, nullptr };
    const int      srcStride[4]= { srcStrideY, srcStrideU, srcStrideV, 0 };


    const int ySize  = dstW * dstH;
    const int uvW    = (dstW + 1) / 2;
    const int uvH    = (dstH + 1) / 2;

    uint8_t* dstSlice[4] = { nullptr, nullptr, nullptr, nullptr };
    int      dstStride[4]= { 0, 0, 0, 0 };

    switch (dstFmt) {
        case FMT_YUV_420_888:

            dstSlice[0] = dst;                       dstStride[0] = dstW;
            dstSlice[1] = dst + ySize;               dstStride[1] = uvW;
            dstSlice[2] = dst + ySize + uvW * uvH;   dstStride[2] = uvW;
            break;
        case FMT_NV21:
        case FMT_NV12:

            dstSlice[0] = dst;           dstStride[0] = dstW;
            dstSlice[1] = dst + ySize;   dstStride[1] = uvW * 2;
            break;
        case FMT_NV16:

            dstSlice[0] = dst;           dstStride[0] = dstW;
            dstSlice[1] = dst + ySize;   dstStride[1] = uvW * 2;
            break;
        case FMT_RGBA_8888:
            dstSlice[0] = dst;           dstStride[0] = dstW * 4;
            break;
        case FMT_RGB_565:
            dstSlice[0] = dst;           dstStride[0] = dstW * 2;
            break;
    }

    int rows = sws_scale(ctx, srcSlice, srcStride, 0, srcH, dstSlice, dstStride);
    if (rows <= 0) {
        LOGE("sws_scale returned %d", rows);
        return -10;
    }

    return 0;
}

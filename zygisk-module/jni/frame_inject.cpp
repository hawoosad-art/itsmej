

#include "frame_inject.h"
#include "include/camera3_compat.h"
#include "frame_source.h"
#include "stream_map.h"

#include <android/log.h>
#include <android/hardware_buffer.h>
#include <android/api-level.h>
#include <dlfcn.h>
#include <string.h>
#include <sys/system_properties.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <mutex>
#include <atomic>
#include <limits.h>
#include <sys/mman.h>
#include <sys/ioctl.h>

struct dma_buf_sync { uint64_t flags; };
#define DMA_BUF_SYNC_READ   (1 << 0)
#define DMA_BUF_SYNC_WRITE  (2 << 0)
#define DMA_BUF_SYNC_START  (0 << 2)
#define DMA_BUF_SYNC_END    (1 << 2)
#ifndef DMA_BUF_IOCTL_SYNC
#define DMA_BUF_IOCTL_SYNC  _IOW('b', 0, struct dma_buf_sync)
#endif

#include <libyuv.h>
#include <turbojpeg.h>

#define TAG "itsanon/frame_inject"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

// Version marker - increment on every fix so logs show which build is running
// [gstreamer.4] V9: fixed the blue tint — the 0x23 (YCbCr420-888) camera-app
// preview stream is now written NV12 (no U/V swap) on MediaTek/OPPO via a
// build-time default (-DUNISOC_23_CHROMA=nv12) + runtime override, instead of the
// hard-coded NV21 that made the OPPO camera-app face blue. Also keeps V8's
// quality: bilinear scale + native-res ring.
#define FRAME_INJECT_VERSION "V84-ITSANON.70-GZDROPBOX-20260914"

// FRAME_BUILD_ID — the exact git branch + commit SHA this .so was built from,
// injected by the CI workflow (same value as HOOK_PROXY_VERSION). It is baked
// into libhookProxy.so at compile time, so it CANNOT lie about which build is
// running. Logged in a loud banner at init AND appended to every chroma decision
// log line, so the on-device logcat you upload to Mylogs proves whether the APK
// is the fresh gstreamer.3 build or a stale one. Falls back to "dev-unknown" if
// the build did not pass it.
#ifndef FRAME_BUILD_ID
#define FRAME_BUILD_ID "dev-unknown"
#endif

// [gstreamer.4] Chroma A/B build-time default for the opaque 0x22 stream.
// Set by CMake from -DUNISOC_22_CHROMA=nv12|nv21 (see CMakeLists.txt).
// If compiled outside that CMake (e.g. a scratch build), default to nv12
// (the gstreamer.3 behavior) so the build still works.
#ifndef UNISOC_22_IS_NV21
#define UNISOC_22_IS_NV21 0
#endif
#ifndef UNISOC_22_DEFAULT_IS_NV21
#define UNISOC_22_DEFAULT_IS_NV21 UNISOC_22_IS_NV21
#endif
// [gstreamer.4 blue fix] The SAME 0x23 (YCbCr420-888) stream is consumed as NV21
// (U/V swapped) on the UNISOC/TECNO device but as NV12 (no swap) on the
// MediaTek/OPPO device. A single hard-coded order cannot be right on both, so we
// expose a build-time default (CMake -DUNISOC_23_CHROMA=nv12|nv21) AND a runtime
// override (the same chroma_override field the app drives), defaulting to nv12 so
// the MediaTek camera-app preview is no longer blue.
#ifndef UNISOC_23_IS_NV21
#define UNISOC_23_IS_NV21 0
#endif
#ifndef UNISOC_23_DEFAULT_IS_NV21
#define UNISOC_23_DEFAULT_IS_NV21 UNISOC_23_IS_NV21
#endif

// [gstreamer.4] Chroma A/B rationale. The fresh-build logs proved the SAME
// opaque 0x22 (IMPLEMENTATION_DEFINED) payload is correct on one stream (role=2
// VIDEO) and blue on another (role=1 PREVIEW), and stale builds saw it blue as
// BOTH NV21 and NV12. AOSP IMPLEMENTATION_DEFINED carries no chroma-order info,
// so the vendor gralloc decides it per stream/consumer — a single hard-coded
// order cannot be trusted across devices. We therefore expose:
//   - a build-time default (-DUNISOC_22_CHROMA=nv12|nv21, via CMake),
//   - a runtime override (frame_inject_set_chroma_override) so ONE APK tests both,
//   - per-stream logging of role+format+size+effective order ([CHROMA A/B]),
//   so a single device run decisively tells us which order each consumer wants.
// 0x11 SP -> NV12, 0x23 420_888 -> NV21 (kept, see below).


typedef int  (*fn_AHardwareBuffer_lock)(
        AHardwareBuffer *, uint64_t usage, int32_t fence,
        const ARect *fence_bounds, void **out_virtual_address);
typedef int  (*fn_AHardwareBuffer_lockPlanes)(
        AHardwareBuffer *, uint64_t usage, int32_t fence,
        const ARect *fence_bounds, AHardwareBuffer_Planes *out_planes);
typedef int  (*fn_AHardwareBuffer_unlock)(AHardwareBuffer *, int32_t *fence);
typedef void (*fn_AHardwareBuffer_describe)(
        const AHardwareBuffer *, AHardwareBuffer_Desc *out_desc);

#undef AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_REGISTER
#undef AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE
#define AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_REGISTER 2
#define AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE    3

#ifndef AHARDWAREBUFFER_USAGE_CAMERA_WRITE
#  define AHARDWAREBUFFER_USAGE_CAMERA_WRITE UINT64_C(0x00020000)
#endif

/* YV12 = YCrCb 4:2:0 planar ("YV12" FourCC). The HAL often resolves a video
 * / recording stream to this planar format; treating it as semi-planar NV12
 * corrupts the chroma. Match the define used in stream_map.cpp. */
#ifndef HAL_PIXEL_FORMAT_YV12
#  define HAL_PIXEL_FORMAT_YV12  0x32315659
#endif

typedef int (*fn_AHardwareBuffer_createFromHandle)(
        const AHardwareBuffer_Desc *desc,
        const native_handle_t *handle,
        int32_t method,
        AHardwareBuffer **out_buffer);
typedef void (*fn_AHardwareBuffer_release)(AHardwareBuffer *buffer);

static void *g_libandroid           = nullptr;
static void *g_libnativewindow      = nullptr;
static fn_AHardwareBuffer_lock             g_lock             = nullptr;
static fn_AHardwareBuffer_lockPlanes       g_lockPlanes       = nullptr;
static fn_AHardwareBuffer_unlock           g_unlock           = nullptr;
static fn_AHardwareBuffer_describe         g_describe         = nullptr;
static fn_AHardwareBuffer_createFromHandle g_createFromHandle = nullptr;
static fn_AHardwareBuffer_release          g_release          = nullptr;

// [gstreamer.4] Chroma A/B. The build-time default is baked in as
// UNISOC_22_DEFAULT_IS_NV21 (0 = NV12 via -DUNISOC_22_CHROMA=nv12, 1 = NV21).
// The app can override at runtime by writing the shared FrameSourceHeader
// chroma_override field (NativeFrameProducer.setChromaOverride), which the
// injector reads via frame_source_get_chroma_override(). The value semantics:
//   -1 = unset (fall back to kChromaDefault)
//    0 = force NV12   (no U/V swap)
//    1 = force NV21   (U/V swap)
static constexpr int kChromaDefault = UNISOC_22_DEFAULT_IS_NV21;

// [V11 PCHROMA] Device-family detection for the per-role chroma defaults.
// Reads the same properties the Kotlin side uses (ro.board.platform then
// ro.hardware). Cached after first call. 1 = UNISOC/Spreadtrum, 0 = other.
/* [V19] UBWC read-touch is a Qualcomm workaround; on MTK it BLOCKS on the
 * encoder stream (serializes to encoder latency -> ~11 fps -> slow-motion
 * recordings, 14.mediatek.5). Gate every touch lock to Qualcomm. */
static bool platform_is_qcom(void) {
    static int cached = -1;
    if (cached < 0) {
        char hw[92] = {0}, board[92] = {0};
        __system_property_get("ro.hardware", hw);
        __system_property_get("ro.board.platform", board);
        cached = (strstr(hw, "qcom") || strstr(board, "qcom") ||
                  strstr(board, "kona") || strstr(board, "lito") ||
                  strstr(board, "sm8") || strstr(board, "sdm") ||
                  strstr(board, "msm")) ? 1 : 0;
    }
    return cached == 1;
}

static bool platform_is_unisoc(void) {
    static int is_unisoc = -1;
    if (is_unisoc >= 0) return is_unisoc == 1;
    char val[PROP_VALUE_MAX + 1] = {0};
    char p[PROP_VALUE_MAX + 1] = {0};
    if (__system_property_get("ro.board.platform", val) <= 0 || val[0] == '\0') {
        __system_property_get("ro.hardware", val);
    }
    for (int i = 0; val[i] && i < PROP_VALUE_MAX; i++)
        p[i] = (char)((val[i] >= 'A' && val[i] <= 'Z') ? val[i] + 32 : val[i]);
    is_unisoc = (strstr(p, "unisoc") || strstr(p, "ums") || strstr(p, "sprd") ||
                 strstr(p, "sc986") || strstr(p, "sc983") || strstr(p, "tiger")) ? 1 : 0;
    LOGI("[build=%s] V11 PCHROMA: platform='%s' → preview chroma default %s",
         FRAME_BUILD_ID, p, is_unisoc ? "NV21 (unisoc)" : "NV12");
    return is_unisoc == 1;
}

int frame_inject_get_chroma_override(void) {
    return frame_source_get_chroma_override();
}

void frame_inject_set_chroma_override(int override_is_nv21) {
    const int v = (override_is_nv21 > 1) ? 1 : (override_is_nv21 < 0 ? -1 : override_is_nv21);
    frame_source_set_chroma_override(v);
    LOGI("[build=%s] FRAME_CHROMA_OVERRIDE set to %d (%s); build default=%d (%s)",
         FRAME_BUILD_ID, v, v == 1 ? "force NV21" : (v == 0 ? "force NV12" : "revert to default"),
         kChromaDefault, kChromaDefault ? "NV21" : "NV12");
}

/* ── [V54 zoom] app crop region (ANDROID_SCALER_CROP_REGION) ───────────────
 * The camera app zooms by shrinking the crop region inside the active array;
 * the preview stream is cropped by the framework, but our injected JPEG is
 * encoded from the FULL src frame — so photos ignored the zoom ("takes the
 * whole thing"). hook_proxy feeds the latest crop region here on every
 * capture result; inject_jpeg then encodes only the mapped sub-rectangle. */
static int32_t g_crop_region[4] = {0, 0, 0, 0};   /* x, y, w, h  (active-array coords) */
static int32_t g_active_array[4] = {0, 0, 0, 0};  /* x, y, w, h */
static bool    g_crop_valid = false;

void frame_inject_set_crop(const int32_t crop[4], const int32_t active[4]) {
    if (!crop || !active) return;
    if (crop[2] <= 0 || crop[3] <= 0 || active[2] <= 0 || active[3] <= 0) return;
    const bool changed = memcmp(g_crop_region, crop, sizeof(g_crop_region)) != 0 ||
                         memcmp(g_active_array, active, sizeof(g_active_array)) != 0 ||
                         !g_crop_valid;
    memcpy(g_crop_region, crop, sizeof(g_crop_region));
    memcpy(g_active_array, active, sizeof(g_active_array));
    g_crop_valid = true;
    if (changed) {
        LOGI("[V54 zoom] crop region -> [%d,%d %dx%d] of active [%d,%d %dx%d]",
             crop[0], crop[1], crop[2], crop[3],
             active[0], active[1], active[2], active[3]);
    }
}

/* Map the active-array crop into src pixel coordinates, snapped to even
 * boundaries (4:2:0 chroma). Returns false when not zoomed (crop ~= full
 * active array) or when the mapping is degenerate — caller then encodes the
 * full frame as before. */
static bool zoom_subrect(int src_w, int src_h, int *rx, int *ry, int *rw, int *rh) {
    if (!g_crop_valid) return false;
    const int aw = g_active_array[2], ah = g_active_array[3];
    /* Not zoomed? (crop covers >=99% of the active array) */
    if ((int64_t)g_crop_region[2] * 100 >= (int64_t)aw * 99 &&
        (int64_t)g_crop_region[3] * 100 >= (int64_t)ah * 99) return false;
    int x = (int)(((int64_t)g_crop_region[0] * src_w) / aw);
    int y = (int)(((int64_t)g_crop_region[1] * src_h) / ah);
    int w = (int)(((int64_t)g_crop_region[2] * src_w) / aw);
    int h = (int)(((int64_t)g_crop_region[3] * src_h) / ah);
    x &= ~1; y &= ~1;            /* even align */
    w = (w + 1) & ~1; h = (h + 1) & ~1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > (src_w & ~1)) w = (src_w & ~1) - x;
    if (y + h > (src_h & ~1)) h = (src_h & ~1) - y;
    if (w < 64 || h < 64) return false;   /* degenerate — keep full frame */
    *rx = x; *ry = y; *rw = w; *rh = h;
    return true;
}

// Resolve whether the destination 0x22 (IMPLEMENTATION_DEFINED) stream must be
// written NV21 (U/V swapped) or NV12. Reads the app-set value from the shared
// header (source of truth) so the app can A/B a single APK at runtime; -1 (unset)
// falls back to the build-time -DUNISOC_22_CHROMA default.
// [V11 PCHROMA] PREVIEW-role streams are consumer-specific per DEVICE FAMILY:
// the TECNO BG6 (UNISOC ums9230, 13.unisoc.1 @ 235da71) preview rendered BLUE
// while written NV12, i.e. the UNISOC preview consumer wants NV21; the OPPO
// CPH2387 (MediaTek mt6765) preview was blue as NV21 (V8) so it wants NV12.
// Video/FaceTec consumers want NV12 on both families (chrome screenshots).
static bool frame_22_is_nv21(StreamRole role, uint32_t w, uint32_t h) {
    const int o = frame_source_get_chroma_override();   // -1 unset, 0 NV12, 1 NV21
    if (o == 0)  return false;                 // explicit force NV12
    if (o == 1)  return true;                  // explicit force NV21
    if (role == STREAM_ROLE_PREVIEW) return platform_is_unisoc();
    // [V13] 13.unisoc.3: UNISOC camera-app VIDEO encoder (1280x720) rendered
    // BLUE as NV12. [V18] 13.unisoc.5 @ V17: the 1080p recording was ALSO blue
    // as NV12 (261 inj) while 720p-as-NV21 stayed correct => the UNISOC
    // recorder wants NV21 at every dim. FaceTec (browser) preferred NV12 on
    // 1080p long ago; recording wins now (override input can flip per build).
    if (platform_is_unisoc()) return true;
    return kChromaDefault != 0;                // unset -> build-time default
}

// Resolve whether the destination 0x23 (YCbCr420-888) stream must be written NV21
// (U/V swapped) or NV12. Like 0x22, this is consumer-specific (UNISOC preview
// wants NV21, MediaTek preview wants NV12, video/FaceTec want NV12 on both).
// Honour the same app-driven chroma_override (0=NV12, 1=NV21,
// -1=unset -> build-time -DUNISOC_23_CHROMA default for non-preview roles).
static bool frame_23_is_nv21(StreamRole role, uint32_t w, uint32_t h) {
    const int o = frame_source_get_chroma_override();   // -1 unset, 0 NV12, 1 NV21
    if (o == 0)  return false;                 // explicit force NV12
    if (o == 1)  return true;                  // explicit force NV21
    if (role == STREAM_ROLE_PREVIEW) return platform_is_unisoc();
    if (platform_is_unisoc() && !(w == 1920 && h == 1080)) return true;  // [V13]
    return UNISOC_23_DEFAULT_IS_NV21 != 0;     // unset -> build-time default
}

// Human-readable role label for per-stream logs.
static const char* frame_role_name(StreamRole role) {
    switch (role) {
        case STREAM_ROLE_PREVIEW:   return "PREVIEW";
        case STREAM_ROLE_VIDEO:     return "VIDEO";
        case STREAM_ROLE_SNAPSHOT:  return "SNAPSHOT";
        default:                    return "?";
    }
}

// [V10 SHARP] Light 4-neighbour unsharp mask on the Y plane. The FaceTec/browser
// stream (1920x1080) is upscaled ~2.3x from the ~816-wide source ring; bilinear
// alone reads soft ("low quality" in the chrome screenshot). A modest strength
// (0.6) restores perceived edge contrast without ringing on faces. Only called
// when the destination is a >=1.4x upscale of the (post-crop) source.
static void sharpen_y_plane(uint8_t *y, int stride, int w, int h) {
    if (!y || w < 3 || h < 3 || stride < w) return;
    std::vector<uint8_t> tmp((size_t)w * (size_t)h);
    for (int j = 1; j < h - 1; j++) {
        const uint8_t *up = y + (size_t)(j - 1) * stride;
        const uint8_t *cu = y + (size_t)j * stride;
        const uint8_t *dn = y + (size_t)(j + 1) * stride;
        uint8_t *o = tmp.data() + (size_t)j * w;
        o[0] = cu[0];
        o[w - 1] = cu[w - 1];
        for (int i = 1; i < w - 1; i++) {
            int c = cu[i];
            int blur = (up[i] + dn[i] + cu[i - 1] + cu[i + 1] + 2) >> 2;
            int v = c + ((c - blur) * 6) / 10;
            o[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    for (int j = 1; j < h - 1; j++)
        memcpy(y + (size_t)j * stride + 1, tmp.data() + (size_t)j * w + 1, (size_t)(w - 2));
}

/* ── [V26] converted-frame cache ─────────────────────────────────────────
 * The per-ROB-fire libyuv chain (NV12->I420 deinterleave, 2MP rotate, scale,
 * re-interleave) saturated one core on thermally-throttled devices and made
 * the camera pipeline stall in bursts (14.mediatek A14 V25 log: 6-frame bursts
 * then 1.5-4.8s gaps = slow-motion injection + slow recordings).  The producer
 * writes at source rate (~24fps) while the camera fires at 30fps per stream,
 * so most fires re-inject the SAME source frame: cache the finished
 * destination image per (role,layout,seq) and replay it with memcpy. */
struct ConvCache {
    std::mutex mu;
    bool     valid = false;
    uint64_t key   = 0;
    int      layout = -1;          /* 0=interleave@lower 1=flex-planar 2=semi-planar */
    std::vector<uint8_t> y, c1, c2;
    size_t y_len = 0, c1_len = 0, c2_len = 0;
    int du_stride = 0, dv_stride = 0, uv_stride = 0;
};
static ConvCache g_conv_cache[4];

static uint64_t conv_mix(uint64_t h, uint64_t v) {
    return (h * 0x9E3779B97F4A7C15ULL) ^ (v + 0x9E3779B97F4A7C15ULL);
}
static uint64_t conv_cache_key(int role, uint32_t dst_w, uint32_t dst_h,
                               int dst_stride, bool is_planar, bool is_nv21) {
    int32_t  pan_x = 0, pan_y = 0; uint32_t scale = 0;
    frame_source_get_overlay_params(&pan_x, &pan_y, &scale);
    uint64_t h = 17;
    h = conv_mix(h, frame_source_get_seq());
    h = conv_mix(h, (uint64_t)role);
    h = conv_mix(h, dst_w); h = conv_mix(h, dst_h);
    h = conv_mix(h, (uint64_t)dst_stride);
    h = conv_mix(h, is_planar ? 1 : 0); h = conv_mix(h, is_nv21 ? 1 : 0);
    h = conv_mix(h, (uint64_t)(uint32_t)pan_x); h = conv_mix(h, (uint64_t)(uint32_t)pan_y);
    h = conv_mix(h, scale);
    h = conv_mix(h, frame_source_get_total_rotation());
    return h;
}
static void conv_cache_store(int role, uint8_t *dst_y, uint8_t *dst_u, uint8_t *dst_v,
                             uint8_t *dst_uv, int dst_stride, int du_stride, int dv_stride,
                             int uv_stride, uint32_t dst_w, uint32_t dst_h,
                             bool is_planar, bool is_nv21) {
    if (role != STREAM_ROLE_PREVIEW && role != STREAM_ROLE_VIDEO) return;
    if (!dst_y) return;
    ConvCache &cc = g_conv_cache[role & 3];
    std::lock_guard<std::mutex> g(cc.mu);
    cc.key = conv_cache_key(role, dst_w, dst_h, dst_stride, is_planar, is_nv21);
    cc.y_len = (size_t)dst_stride * dst_h;
    cc.y.resize(cc.y_len);
    memcpy(cc.y.data(), dst_y, cc.y_len);
    cc.layout = (is_planar && role == STREAM_ROLE_VIDEO) ? 0
              : is_planar ? 1 : 2;
    if (cc.layout == 0) {
        uint8_t *semi = (dst_u < dst_v) ? dst_u : dst_v;
        cc.c1_len = (size_t)dst_stride * dst_h / 2;
        cc.c1.resize(cc.c1_len); memcpy(cc.c1.data(), semi, cc.c1_len);
        cc.c2_len = 0;
    } else if (cc.layout == 1) {
        cc.du_stride = du_stride; cc.dv_stride = dv_stride;
        cc.c1_len = (size_t)du_stride * dst_h / 2;
        cc.c2_len = (size_t)dv_stride * dst_h / 2;
        cc.c1.resize(cc.c1_len); memcpy(cc.c1.data(), dst_u, cc.c1_len);
        cc.c2.resize(cc.c2_len); memcpy(cc.c2.data(), dst_v, cc.c2_len);
    } else {
        cc.uv_stride = uv_stride;
        cc.c1_len = (size_t)uv_stride * ((dst_h + 1) / 2);
        cc.c1.resize(cc.c1_len); memcpy(cc.c1.data(), dst_uv, cc.c1_len);
        cc.c2_len = 0;
    }
    cc.valid = true;
}
static bool conv_cache_replay(ConvCache &cc, uint8_t *dst_y, uint8_t *dst_u, uint8_t *dst_v,
                              uint8_t *dst_uv) {
    if (!dst_y) return false;
    memcpy(dst_y, cc.y.data(), cc.y_len);
    if (cc.layout == 0) {
        uint8_t *semi = (dst_u < dst_v) ? dst_u : dst_v;
        if (!semi) return false;
        memcpy(semi, cc.c1.data(), cc.c1_len);
    } else if (cc.layout == 1) {
        if (!dst_u || !dst_v) return false;
        memcpy(dst_u, cc.c1.data(), cc.c1_len);
        memcpy(dst_v, cc.c2.data(), cc.c2_len);
    } else {
        if (!dst_uv) return false;
        memcpy(dst_uv, cc.c1.data(), cc.c1_len);
    }
    return true;
}

static int fence_wait(int fd, int timeout_ms) {
    if (fd < 0) return 0;
    if (timeout_ms <= 0) return -ETIMEDOUT;

    struct pollfd pfd = { fd, POLLIN, 0 };
    int remaining  = timeout_ms;
    int max_retry  = 3;

    struct timespec ts_start;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (max_retry-- > 0 && remaining > 0) {
        int ret = poll(&pfd, 1, remaining);
        if (ret > 0) {
            if (pfd.revents & (POLLERR | POLLNVAL)) return -EINVAL;
            return 0;
        }
        if (ret == 0) return -ETIMEDOUT;
        if (errno == EINTR) {
            struct timespec ts_now;
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            int elapsed = (int)((ts_now.tv_sec - ts_start.tv_sec) * 1000 +
                              (ts_now.tv_nsec - ts_start.tv_nsec) / 1000000);
            remaining -= elapsed;
            if (remaining <= 0) return -ETIMEDOUT;
            continue;
        }
        return -errno;
    }
    return -ETIMEDOUT;
}

static inline bool is_protected_ahb(AHardwareBuffer *ahb,
                                    const native_handle_t *nh) {
    if (g_describe) {
        AHardwareBuffer_Desc protect_check = {};
        g_describe(ahb, &protect_check);
        if (protect_check.usage & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT) {
            LOGW("resolve_ahwb: PROTECTED_CONTENT — releasing, skip inject (handle=%p)",
                 (void *)nh);
            return true;
        }
    }
    return false;
}

static inline AHardwareBuffer *resolve_ahwb(const camera3_stream_buffer_t *buf) {
    if (!buf || !buf->buffer || !(*buf->buffer)) {
        LOGE("resolve_ahwb: null buf/buffer/handle");
        return nullptr;
    }






    uintptr_t raw_addr = (uintptr_t)(*buf->buffer);
    if (raw_addr >> 56) {
        raw_addr &= 0x00FFFFFFFFFFFFFFULL;
    }
    native_handle_t *nh = (native_handle_t *)raw_addr;






    LOGD("resolve_ahwb: handle=%p (raw=%p tag=0x%02x)",
         (void *)nh, (void *)(uintptr_t)(*buf->buffer),
         (unsigned)((uintptr_t)(*buf->buffer) >> 56));

    if (!g_createFromHandle) {
        LOGE("resolve_ahwb: AHardwareBuffer_createFromHandle not available");
        return nullptr;
    }





    AHardwareBuffer_Desc descs[3] = {};
    int ndescs = 0;


    {
        AHardwareBuffer_Desc &d = descs[ndescs++];
        d.width  = buf->stream ? buf->stream->width  : 1;
        d.height = buf->stream ? buf->stream->height : 1;
        d.layers = 1;
        d.format = buf->stream ? buf->stream->format : HAL_PIXEL_FORMAT_BLOB;
        if (buf->stream && buf->stream->format == HAL_PIXEL_FORMAT_BLOB) {
            d.usage = AHARDWAREBUFFER_USAGE_CAMERA_WRITE;
        } else {
            d.usage = AHARDWAREBUFFER_USAGE_CAMERA_WRITE
                    | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE
                    | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER;
        }
        d.usage |= AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    }

    if (buf->stream) {
        AHardwareBuffer_Desc &d = descs[ndescs++];
        d = descs[0];
        d.usage = (uint64_t)buf->stream->usage | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    }

    {
        AHardwareBuffer_Desc &d = descs[ndescs++];
        d = descs[0];
        d.usage = 0;
    }

    LOGD("resolve_ahwb: desc w=%u h=%u fmt=0x%x profiles=%d",
         descs[0].width, descs[0].height, descs[0].format, ndescs);

    AHardwareBuffer *ahb = nullptr;
    int last_rc_clone = -1, last_rc_reg = -1;

    for (int pi = 0; pi < ndescs; ++pi) {

        ahb = nullptr;
        int rc = g_createFromHandle(&descs[pi], nh,
                AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE, &ahb);
        last_rc_clone = rc;
        if (rc == 0 && ahb) {
            LOGD("resolve_ahwb: CLONE OK (profile %d usage=0x%llx) → ahb=%p",
                 pi, (unsigned long long)descs[pi].usage, (void *)ahb);
            if (is_protected_ahb(ahb, nh)) { g_release(ahb); return nullptr; }
            return ahb;
        }


        ahb = nullptr;
        rc = g_createFromHandle(&descs[pi], nh,
                AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_REGISTER, &ahb);
        last_rc_reg = rc;
        if (rc == 0 && ahb) {
            LOGW("resolve_ahwb: REGISTER OK (profile %d usage=0x%llx) → ahb=%p "
                 "(handle ownership transferred)",
                 pi, (unsigned long long)descs[pi].usage, (void *)ahb);
            if (is_protected_ahb(ahb, nh)) { g_release(ahb); return nullptr; }
            return ahb;
        }

        LOGD("resolve_ahwb: profile %d CLONE rc=%d REGISTER rc=%d — trying next",
             pi, last_rc_clone, last_rc_reg);
    }

    LOGE("resolve_ahwb: ALL mapping methods failed across %d profile(s) "
         "(last CLONE rc=%d, last REGISTER rc=%d) — "
         "handle=%p may be a HIDL transport wrapper (not a real gralloc handle); "
         "RTRN_LOCKED hook path uses real GraphicBuffer handles and should succeed",
         ndescs, last_rc_clone, last_rc_reg, (void *)nh);
    return nullptr;
}

static inline void release_ahwb(AHardwareBuffer *ahb,
                                  const camera3_stream_buffer_t * ) {
    if (ahb && g_release) g_release(ahb);
}

int frame_inject_init(void) {
    LOGI("frame_inject_init: loading libraries... version=%s", FRAME_INJECT_VERSION);
    // ── FRESH-BUILD PROOF ──────────────────────────────────────────────────
    // FRAME_BUILD_ID is the git branch+sha baked into libhookProxy.so at
    // compile time (never a manual string), so this banner is authoritative.
    // Grep your Mylogs logcat for this exact line to confirm the APK running
    // on the phone is the NEW gstreamer.4 build, not a stale/cached one.
    LOGI("╔══════════════════════════════════════════════════════════════════╗");
    LOGI("║ FRESH-BUILD-CHECK  version=%s", FRAME_INJECT_VERSION);
    LOGI("║ FRESH-BUILD-CHECK  build_id=%s", FRAME_BUILD_ID);
    LOGI("║ FRESH-BUILD-CHECK  chroma_override=%d default_0x22_nv21=%d default_0x23_nv21=%d",
         frame_inject_get_chroma_override(), kChromaDefault, UNISOC_23_DEFAULT_IS_NV21);
    LOGI("║ FRESH-BUILD-CHECK  (build_id == gstreamer.4-<sha> ⇒ this IS the new build)");
    LOGI("╚══════════════════════════════════════════════════════════════════╝");


    g_libnativewindow = dlopen("libnativewindow.so", RTLD_NOW | RTLD_NOLOAD);
    if (!g_libnativewindow) g_libnativewindow = dlopen("libnativewindow.so", RTLD_NOW);

    g_libandroid = dlopen("libandroid.so", RTLD_NOW | RTLD_NOLOAD);
    if (!g_libandroid) g_libandroid = dlopen("libandroid.so", RTLD_NOW);
    if (!g_libandroid) {
        LOGE("frame_inject_init: FAILED — cannot dlopen libandroid.so: %s", dlerror());
        return -1;
    }

    g_lock       = (fn_AHardwareBuffer_lock)       dlsym(g_libandroid, "AHardwareBuffer_lock");
    g_lockPlanes = (fn_AHardwareBuffer_lockPlanes)  dlsym(g_libandroid, "AHardwareBuffer_lockPlanes");
    g_unlock     = (fn_AHardwareBuffer_unlock)      dlsym(g_libandroid, "AHardwareBuffer_unlock");
    g_describe   = (fn_AHardwareBuffer_describe)    dlsym(g_libandroid, "AHardwareBuffer_describe");
    g_release    = (fn_AHardwareBuffer_release)     dlsym(g_libandroid, "AHardwareBuffer_release");


    if (g_libnativewindow) {
        g_createFromHandle = (fn_AHardwareBuffer_createFromHandle)
                             dlsym(g_libnativewindow, "AHardwareBuffer_createFromHandle");
    }
    if (!g_createFromHandle && g_libandroid) {
        g_createFromHandle = (fn_AHardwareBuffer_createFromHandle)
                             dlsym(g_libandroid, "AHardwareBuffer_createFromHandle");
    }

    LOGI("frame_inject_init: symbols resolved:");
    LOGI("  lock=%p  lockPlanes=%p  unlock=%p  describe=%p",
         (void *)g_lock, (void *)g_lockPlanes, (void *)g_unlock, (void *)g_describe);
    LOGI("  createFromHandle=%p  release=%p",
         (void *)g_createFromHandle, (void *)g_release);

    if (!g_lock || !g_unlock || !g_describe) {
        LOGE("frame_inject_init: FAILED — missing required AHardwareBuffer symbols");
        dlclose(g_libandroid);
        if (g_libnativewindow) dlclose(g_libnativewindow);
        g_libandroid = nullptr;
        g_libnativewindow = nullptr;
        return -1;
    }

    if (!g_createFromHandle) {
        LOGW("frame_inject_init: AHardwareBuffer_createFromHandle MISSING — "
             "injection may fail on some devices.");
    }

    LOGI("frame_inject_init: OK (lockPlanes=%s createFromHandle=%s)",
         g_lockPlanes ? "YES" : "NO (will use plain lock)",
         g_createFromHandle ? "YES" : "NO (CRITICAL — injection may fail)");
    return 0;
}

void frame_inject_destroy(void) {
    if (g_libandroid) {
        dlclose(g_libandroid);
        g_libandroid = nullptr;
    }
    if (g_libnativewindow) {
        dlclose(g_libnativewindow);
        g_libnativewindow = nullptr;
    }
    g_lock = nullptr;
    g_lockPlanes = nullptr;
    g_unlock = nullptr;
    g_describe = nullptr;
    g_createFromHandle = nullptr;
    g_release = nullptr;
}

/* Apply stream rotation to a scaled NV12 frame already in dst_y/dst_uv.
 * rotation: 0=0°, 1=90°CCW, 2=180°, 3=270°CCW  (camera3_stream_t::rotation values).
 * For 180° the buffer dimensions stay the same.
 * For 90°/270° dst_w×dst_h is the POST-rotation size; the pre-rotation size is dst_h×dst_w. */
static void apply_stream_rotation(uint8_t *dst_y,  int dst_stride,
                                   uint8_t *dst_uv, int dst_uv_stride,
                                   uint32_t dst_w, uint32_t dst_h,
                                   int rotation) {
    if (rotation == 0) return;

    libyuv::RotationMode rot_mode;
    switch (rotation) {
        /* camera3_stream_rotation_t values are CCW degrees.
         * libyuv kRotateN values are CW degrees.
         * CCW 90°  → CW 270°  = kRotate270
         * CCW 180° → CW 180°  = kRotate180
         * CCW 270° → CW 90°   = kRotate90  */
        case 1:  rot_mode = libyuv::kRotate270; break;   // 90° CCW
        case 2:  rot_mode = libyuv::kRotate180; break;   // 180°
        case 3:  rot_mode = libyuv::kRotate90;  break;   // 270° CCW = 90° CW
        default: return;
    }

    /* For 90°/270° the pre-rotation plane dimensions swap:
     *   pre-rotation W = dst_h,  pre-rotation H = dst_w
     * For 180° dimensions are unchanged.                       */
    uint32_t pre_w = (rotation & 1) ? dst_h : dst_w;
    uint32_t pre_h = (rotation & 1) ? dst_w : dst_h;

    /* NV12 → I420 (split interleaved UV into separate planes) */
    size_t i420_sz = (size_t)pre_w * pre_h * 3 / 2;
    std::vector<uint8_t> src_i420(i420_sz);
    uint8_t *si_y = src_i420.data();
    uint8_t *si_u = si_y + (size_t)pre_w * pre_h;
    uint8_t *si_v = si_u + (size_t)(pre_w / 2) * (pre_h / 2);

    libyuv::NV12ToI420(dst_y,  dst_stride,
                        dst_uv, dst_uv_stride,
                        si_y, (int)pre_w,
                        si_u, (int)(pre_w / 2),
                        si_v, (int)(pre_w / 2),
                        (int)pre_w, (int)pre_h);

    /* Rotate I420 into a second buffer (dst dimensions after rotation) */
    size_t rot_sz = (size_t)dst_w * dst_h * 3 / 2;
    std::vector<uint8_t> rot_i420(rot_sz);
    uint8_t *ri_y = rot_i420.data();
    uint8_t *ri_u = ri_y + (size_t)dst_w * dst_h;
    uint8_t *ri_v = ri_u + (size_t)(dst_w / 2) * (dst_h / 2);

    libyuv::I420Rotate(si_y, (int)pre_w,
                        si_u, (int)(pre_w / 2),
                        si_v, (int)(pre_w / 2),
                        ri_y, (int)dst_w,
                        ri_u, (int)(dst_w / 2),
                        ri_v, (int)(dst_w / 2),
                        (int)pre_w, (int)pre_h, rot_mode);

    /* I420 → NV12 back into the locked destination planes */
    libyuv::I420ToNV12(ri_y, (int)dst_w,
                        ri_u, (int)(dst_w / 2),
                        ri_v, (int)(dst_w / 2),
                        dst_y,  dst_stride,
                        dst_uv, dst_uv_stride,
                        (int)dst_w, (int)dst_h);
}

/* Rotate an NV12 source frame to upright orientation using the total rotation
 * from the shared header (same source_rotation + manual_rotation used by Fix1).
 * If no rotation is needed, out_* point at the input and 0 is returned.
 * Otherwise a rotated NV12 frame is rendered into `buf` and out_* are updated
 * to point at it (dimensions swap for 90/270°). Used by both the zoom-in
 * (Fix1) path and the zoom-out letterbox path so orientation is consistent at
 * every zoom level — the letterbox path previously skipped rotation, which made
 * pressing zoom-out drop the rotation and appear to "rotate" the frames. */
static uint32_t rotate_source_upright(
        const uint8_t *src_y, const uint8_t *src_uv, int src_stride,
        int src_w, int src_h,
        std::vector<uint8_t> &buf,
        const uint8_t *&out_y, const uint8_t *&out_uv,
        int &out_stride, int &out_w, int &out_h) {
    uint32_t rot = frame_source_get_total_rotation();
    /* [V10 ROT0] legacy 180-deg first-frame-race fallback removed: it
     * flipped already-upright static media upside-down (OPPO CPH2387 logs). */
    out_y = src_y; out_uv = src_uv;
    out_stride = src_stride; out_w = src_w; out_h = src_h;
    if (!(rot == 90 || rot == 180 || rot == 270)) return 0;

    libyuv::RotationMode rm =
        (rot == 90)  ? libyuv::kRotate90  :
        (rot == 270) ? libyuv::kRotate270 : libyuv::kRotate180;
    int rw = (rot == 90 || rot == 270) ? src_h : src_w;
    int rh = (rot == 90 || rot == 270) ? src_w : src_h;
    rw = (rw + 1) & ~1; rh = (rh + 1) & ~1;
    int ruv_w = (rw + 1) / 2, ruv_h = (rh + 1) / 2;

    std::vector<uint8_t> i420((size_t)src_w * src_h +
                              (size_t)((src_w + 1) / 2) * ((src_h + 1) / 2) * 2);
    uint8_t *iy = i420.data();
    uint8_t *iu = iy + (size_t)src_w * src_h;
    uint8_t *iv = iu + (size_t)((src_w + 1) / 2) * ((src_h + 1) / 2);
    libyuv::NV12ToI420(src_y, src_stride, src_uv, src_stride,
                       iy, src_w, iu, (src_w + 1) / 2, iv, (src_w + 1) / 2,
                       src_w, src_h);

    std::vector<uint8_t> ri420((size_t)rw * rh + (size_t)ruv_w * ruv_h * 2);
    uint8_t *ri_y = ri420.data();
    uint8_t *ri_u = ri_y + (size_t)rw * rh;
    uint8_t *ri_v = ri_u + (size_t)ruv_w * ruv_h;
    libyuv::I420Rotate(iy, src_w, iu, (src_w + 1) / 2, iv, (src_w + 1) / 2,
                       ri_y, rw, ri_u, ruv_w, ri_v, ruv_w,
                       src_w, src_h, rm);

    size_t nv12_sz = (size_t)rw * rh + (size_t)rw * ((rh + 1) / 2);
    buf.resize(nv12_sz);
    uint8_t *ny = buf.data();
    uint8_t *nuv = ny + (size_t)rw * rh;
    libyuv::I420ToNV12(ri_y, rw, ri_u, ruv_w, ri_v, ruv_w,
                       ny, rw, nuv, rw, rw, rh);

    out_y = ny; out_uv = nuv; out_stride = rw; out_w = rw; out_h = rh;
    return rot;
}

static bool inject_yuv(AHardwareBuffer *hwb,
                        uint32_t dst_w, uint32_t dst_h,
                        const FrameData *src,
                        int32_t fence_fd,
                        int32_t *out_release_fence,
                        int dmabuf_fd,
                        StreamRole role) {
    if (!hwb || !src || !out_release_fence) return false;
    *out_release_fence = -1;

    AHardwareBuffer_Desc desc;
    g_describe(hwb, &desc);
    int actual_format = desc.format;

    /* [V17 speed] bilinear 1080p scaling inside the HAL return path throttled
     * the MTK A14 video pipeline to ~7 fps (14.mediatek.4) => slow-motion
     * recordings. Encoders don't need pretty scaling: nearest for VIDEO. */
    const libyuv::FilterMode filt =
        (role == STREAM_ROLE_VIDEO) ? libyuv::kFilterNone : libyuv::kFilterBilinear;

    LOGD("inject_yuv: dst=%ux%u desc.fmt=0x%x desc.stride=%u src=%ux%u stride=%u fence=%d",
         dst_w, dst_h, actual_format, desc.stride,
         src->width, src->height, src->stride, fence_fd);

    /* [V21 DIRECT] MTK VIDEO fast path — write through our own mmap of the
     * dmabuf instead of the HAL lock below. Two wins:
     *  1. SPEED: g_lockPlanes(fence_fd) blocks until the encoder releases the
     *     frame — V18 measured 10.7 fps on 720p ("soo slow"). Our MAP_SHARED
     *     mapping never waits (worst case one torn frame, never a stall).
     *  2. CHROMA: the chroma placement is selectable at RUNTIME via
     *     `persist.ecomcam.chroma`, so one build can A/B the encoder layout
     *     without reflashing:
     *       0 (default) = interleaved UV at stride*height          (R1, like V18)
     *       1           = interleaved UV at stride*align64(height) (common MTK alignment)
     *       2           = YV12 planes: V at stride*height, U at +stride*h/4
     * Falls through to the proven HAL-lock path if fd/alloc/mmap fail. */
    if (role == STREAM_ROLE_VIDEO && dmabuf_fd >= 0 &&
        !platform_is_unisoc() && !platform_is_qcom()) {
        char cprop[92] = {0};
        __system_property_get("persist.ecomcam.chroma", cprop);
        /* [V21.1] default 3 = interleaved at stride*ALIGN(h,32): both consulted
         * LLMs (and the V18 field result, where stride*h interleave stayed
         * gray) point at MTK VENC deriving chroma as stride*ALIGN(height,32)
         * and reading it as ONE interleaved CbCr plane, ignoring the gralloc
         * plane offsets (which is why the display looks right but the encoder
         * goes gray). */
        const int strat = cprop[0] ? atoi(cprop) : 3;
        const long alloc = (long)lseek(dmabuf_fd, 0, SEEK_END);
        const int d_w = (int)dst_w, d_h = (int)dst_h;
        const size_t d_stride = (desc.stride > 0) ? desc.stride : dst_w;
        const int d_chw = (d_w + 1) / 2, d_chh = (d_h + 1) / 2;
        const size_t nominal = d_stride * (size_t)d_h;
        const size_t aligned_off = d_stride * (size_t)(((d_h + 63) / 64) * 64);
        const size_t aligned32_off = d_stride * (size_t)(((d_h + 31) / 32) * 32);
        const size_t chroma_sz = d_stride * (size_t)d_chh;
        const size_t chroma_base = (strat == 1) ? aligned_off
                                 : (strat == 3) ? aligned32_off
                                                : nominal;
        const size_t need = chroma_base + chroma_sz;
        void *mm = (alloc > 0 && alloc >= (long)need)
                   ? mmap(nullptr, (size_t)alloc, PROT_READ | PROT_WRITE,
                          MAP_SHARED, dmabuf_fd, 0)
                   : nullptr;
        if (mm && mm != MAP_FAILED) {
            struct dma_buf_sync d_ss = {};
            d_ss.flags = (uint64_t)(DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
            ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &d_ss);
            uint8_t *base = static_cast<uint8_t *>(mm);

            /* Source NV12 from the ring + V21 recording rotation: a portrait
             * source into a landscape recording is pre-rotated 90CCW (=270)
             * to cancel the MTK player's 90CW metadata (V20 recorded sideways
             * per 14.mediatek.6 screenshot). Manual 90/270 totals honored. */
            uint32_t srot = frame_source_get_total_rotation();
            if ((srot % 180) == 0 && (int)src->height > (int)src->width && d_w > d_h)
                srot = 270;
            const uint8_t *sy = src->y_plane;
            const uint8_t *suv = src->uv_plane;
            int sw = (int)src->width, sh = (int)src->height, ss = (int)src->stride;
            std::vector<uint8_t> rot_i420, rot_nv12;
            if (srot == 90 || srot == 180 || srot == 270) {
                libyuv::RotationMode rm =
                    (srot == 90)  ? libyuv::kRotate90  :
                    (srot == 270) ? libyuv::kRotate270 : libyuv::kRotate180;
                int rw = ((srot == 90 || srot == 270) ? sh : sw) & ~1;
                int rh = ((srot == 90 || srot == 270) ? sw : sh) & ~1;
                const int ruv = (rw + 1) / 2, ruvh = (rh + 1) / 2;
                rot_i420.resize((size_t)sw * sh +
                                2 * (size_t)((sw + 1) / 2) * ((sh + 1) / 2));
                uint8_t *iy = rot_i420.data();
                uint8_t *iu = iy + (size_t)sw * sh;
                uint8_t *iv = iu + (size_t)((sw + 1) / 2) * ((sh + 1) / 2);
                libyuv::NV12ToI420(sy, ss, suv, ss, iy, sw, iu, (sw + 1) / 2,
                                   iv, (sw + 1) / 2, sw, sh);
                rot_nv12.resize((size_t)rw * rh + (size_t)rw * ruvh);
                uint8_t *ny = rot_nv12.data();
                uint8_t *nuv = ny + (size_t)rw * rh;
                std::vector<uint8_t> rtmp((size_t)rw * rh + 2 * (size_t)ruv * ruvh);
                uint8_t *ry = rtmp.data();
                uint8_t *ru = ry + (size_t)rw * rh;
                uint8_t *rv = ru + (size_t)ruv * ruvh;
                libyuv::I420Rotate(iy, sw, iu, (sw + 1) / 2, iv, (sw + 1) / 2,
                                   ry, rw, ru, ruv, rv, ruv, sw, sh, rm);
                libyuv::I420ToNV12(ry, rw, ru, ruv, rv, ruv, ny, rw, nuv, rw, rw, rh);
                sy = ny; suv = nuv; sw = rw; sh = rh; ss = rw;
            }
            /* Fix3 center AR-crop so the downscale stays clean */
            if (sw > 0 && sh > 0 &&
                (int64_t)sw * d_h != (int64_t)sh * d_w) {
                int cw = sw, ch = sh;
                if ((int64_t)sw * d_h < (int64_t)sh * d_w)
                    ch = (int)((int64_t)sw * d_h / d_w) & ~1;
                else
                    cw = (int)((int64_t)sh * d_w / d_h) & ~1;
                if (cw > 0 && ch > 0 && (cw < sw || ch < sh)) {
                    int ox = ((sw - cw) / 2) & ~1, oy = ((sh - ch) / 2) & ~1;
                    sy  += (size_t)oy * ss + ox;
                    suv += (size_t)(oy / 2) * ss + ox;
                    sw = cw; sh = ch;
                }
            }
            const int s_chw = (sw + 1) / 2, s_chh = (sh + 1) / 2;
            std::vector<uint8_t> tmp_u((size_t)s_chw * s_chh);
            std::vector<uint8_t> tmp_v((size_t)s_chw * s_chh);
            for (int y = 0; y < s_chh; y++) {
                const uint8_t *row = suv + (size_t)y * ss;
                uint8_t *ru2 = tmp_u.data() + (size_t)y * s_chw;
                uint8_t *rv2 = tmp_v.data() + (size_t)y * s_chw;
                for (int x = 0; x < s_chw; x++) { ru2[x] = row[2 * x]; rv2[x] = row[2 * x + 1]; }
            }
            std::vector<uint8_t> su2((size_t)d_chw * d_chh);
            std::vector<uint8_t> sv2((size_t)d_chw * d_chh);
            libyuv::I420Scale(sy, ss, tmp_u.data(), s_chw, tmp_v.data(), s_chw, sw, sh,
                              base, (int)d_stride, su2.data(), d_chw, sv2.data(), d_chw,
                              d_w, d_h, libyuv::kFilterNone);
            if (strat == 2) {
                uint8_t *vp = base + nominal;
                uint8_t *up = base + nominal + chroma_sz / 2;
                for (int y = 0; y < d_chh; y++) {
                    memcpy(vp + (size_t)y * d_stride, sv2.data() + (size_t)y * d_chw, d_chw);
                    memcpy(up + (size_t)y * d_stride, su2.data() + (size_t)y * d_chw, d_chw);
                }
            } else {
                uint8_t *semi = base + chroma_base;
                for (int y = 0; y < d_chh; y++) {
                    const uint8_t *ru3 = su2.data() + (size_t)y * d_chw;
                    const uint8_t *rv3 = sv2.data() + (size_t)y * d_chw;
                    uint8_t *drow = semi + (size_t)y * d_stride;
                    for (int x = 0; x < d_chw; x++) {
                        drow[2 * x]     = ru3[x];
                        drow[2 * x + 1] = rv3[x];
                    }
                }
            }
            __sync_synchronize();
            struct dma_buf_sync d_se = {};
            d_se.flags = (uint64_t)(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
            ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &d_se);
            munmap(mm, (size_t)alloc);
            static std::atomic<uint64_t> s_dc{0};
            const uint64_t dc = s_dc.fetch_add(1, std::memory_order_relaxed);
            if (dc < 5 || (dc % 200) == 0)
                LOGI("inject_yuv: [V21 direct] VIDEO src=%dx%d dst=%dx%d stride=%zu alloc=%ld strat=%d chroma@%zu (nom=%zu a32=%zu a64=%zu) rot=%u dc=%llu",
                     (int)src->width, (int)src->height, d_w, d_h, d_stride, alloc,
                     strat, chroma_base, nominal, aligned32_off, aligned_off,
                     srot, (unsigned long long)dc);
            return true;
        }
        static std::atomic<uint64_t> s_df{0};
        const uint64_t df = s_df.fetch_add(1, std::memory_order_relaxed);
        if (df < 5 || (df % 200) == 0)
            LOGW("inject_yuv: [V21 direct] fallback to lock path (alloc=%ld need=%zu strat=%d mmap_ok=%d) df=%llu",
                 alloc, need, strat, (mm && mm != MAP_FAILED) ? 1 : 0,
                 (unsigned long long)df);
    }

    if (g_lockPlanes) {
        AHardwareBuffer_Planes planes;

        int err = g_lockPlanes(hwb,
                               AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                               fence_fd, nullptr, &planes);
        if (err != 0) {
            LOGI("inject_yuv: lockPlanes FAILED err=%d (%s) — trying AHardwareBuffer_lock fallback",
                 err, strerror(-err));
            /* Fallback: use the simpler AHardwareBuffer_lock() which returns a
             * single CPU pointer.  This succeeds on some OPlus/Qualcomm devices
             * where lockPlanes fails after a configureStreams reconfigure (the
             * new buffer's HAL-internal layout rejects lockPlanes but accepts
             * the older lock API).  Full rotation + AR-crop + NV12Scale are
             * applied here so the output is identical to the lockPlanes path. */
            if (!g_lock) {
                LOGE("inject_yuv: lockPlanes failed and g_lock is null — aborting");
                return false;
            }
            void *fb_cpu = nullptr;
            int fb_rc = g_lock(hwb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                               -1, nullptr, &fb_cpu);
            if (fb_rc != 0 || !fb_cpu) {
                LOGE("inject_yuv: g_lock fallback also FAILED rc=%d — trying dma-buf mmap", fb_rc);
                /* Third-level fallback: mmap() the underlying dma-buf fd directly.
                 * This bypasses the HAL lock API (which fails on OPlus after many
                 * configureStreams reconfigures).  We write to the LINEAR memory
                 * region, then issue DMA_BUF_SYNC to flush, then do the existing
                 * UBWC read-touch to force the GPU to read from linear.
                 *
                 * Stride: use dst_w when desc.stride==0 (opaque/UBWC layout). */
                if (dmabuf_fd < 0) {
                    LOGE("inject_yuv: mmap fallback: no dma-buf fd — aborting");
                    return false;
                }
                int    mm_stride = (desc.stride > 0) ? (int)desc.stride : (int)dst_w;
                size_t mm_sz     = (size_t)mm_stride * (size_t)dst_h * 3u / 2u; /* NV12 */
                void  *mm_ptr    = mmap(nullptr, mm_sz, PROT_READ | PROT_WRITE,
                                        MAP_SHARED, dmabuf_fd, 0);
                if (mm_ptr == MAP_FAILED) {
                    LOGE("inject_yuv: mmap fallback FAILED errno=%d — aborting", errno);
                    return false;
                }
                /* Sync begin */
                struct dma_buf_sync mm_sync_start = {};
                mm_sync_start.flags = (uint64_t)(DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
                ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &mm_sync_start);

                uint8_t *mm_y  = (uint8_t *)mm_ptr;
                uint8_t *mm_uv = mm_y + (size_t)mm_stride * (size_t)dst_h;

                /* Duplicate source setup + rotation from g_lock path above */
                int    ms_w = (int)src->width, ms_h = (int)src->height;
                int    ms_stride = (int)src->stride;
                const uint8_t *ms_y  = src->y_plane;
                const uint8_t *ms_uv = src->uv_plane
                                       ? src->uv_plane
                                       : (ms_y + (size_t)ms_stride * (size_t)ms_h);
                if (!ms_y) {
                    struct dma_buf_sync mm_sync_end2 = {};
                    mm_sync_end2.flags = (uint64_t)(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
                    ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &mm_sync_end2);
                    munmap(mm_ptr, mm_sz);
                    LOGE("inject_yuv: mmap fallback: src->y_plane NULL");
                    return false;
                }
                uint32_t mm_rot = frame_source_get_total_rotation();
            /* [V19 rot] MTK encoders tag the recording with their own rotation
             * metadata; baking the overlay rotation into the buffer double-
             * rotates the recorded video (sideways, 14.mediatek.5). */
            if (role == STREAM_ROLE_VIDEO && !platform_is_unisoc() && !platform_is_qcom()) {
                /* [V21 rot] V19/V20 shipped rot=0 -> portrait pixels squashed into
                 * the 720p recording and the MTK player's 90CW metadata tipped the
                 * result sideways (14.mediatek.6 screenshot: head at right). The
                 * encoder tags 90CW, so pre-rotate pixels 90CCW (=270) when a
                 * portrait source meets a landscape recording: upright after the
                 * player metadata, and no more aspect squash. Manual 90/270
                 * totals (user overlay) are honored untouched. */
                if ((mm_rot % 180) == 0 && ms_h > ms_w && dst_w > dst_h) mm_rot = 270;
            }
                /* [V10 ROT0] legacy 180-deg first-frame-race fallback removed: it
                 * flipped already-upright static media upside-down (OPPO CPH2387 logs). */
                std::vector<uint8_t> mm_rot_buf;
                if (mm_rot == 90 || mm_rot == 180 || mm_rot == 270) {
                    libyuv::RotationMode rm =
                        (mm_rot == 90)  ? libyuv::kRotate90  :
                        (mm_rot == 270) ? libyuv::kRotate270 : libyuv::kRotate180;
                    int rw = (mm_rot == 90 || mm_rot == 270) ? ms_h : ms_w;
                    int rh = (mm_rot == 90 || mm_rot == 270) ? ms_w : ms_h;
                    rw = (rw + 1) & ~1; rh = (rh + 1) & ~1;
                    int ruv_w = (rw + 1) / 2, ruv_h = (rh + 1) / 2;
                    std::vector<uint8_t> i420((size_t)ms_w * ms_h +
                                              (size_t)((ms_w+1)/2) * ((ms_h+1)/2) * 2);
                    uint8_t *iy = i420.data();
                    uint8_t *iu = iy + (size_t)ms_w * ms_h;
                    uint8_t *iv = iu + (size_t)((ms_w+1)/2) * ((ms_h+1)/2);
                    libyuv::NV12ToI420(ms_y, ms_stride, ms_uv, ms_stride,
                                       iy, ms_w, iu, (ms_w+1)/2, iv, (ms_w+1)/2,
                                       ms_w, ms_h);
                    std::vector<uint8_t> ri420((size_t)rw * rh + (size_t)ruv_w * ruv_h * 2);
                    uint8_t *ri_y = ri420.data();
                    uint8_t *ri_u = ri_y + (size_t)rw * rh;
                    uint8_t *ri_v = ri_u + (size_t)ruv_w * ruv_h;
                    libyuv::I420Rotate(iy, ms_w, iu, (ms_w+1)/2, iv, (ms_w+1)/2,
                                       ri_y, rw, ri_u, ruv_w, ri_v, ruv_w,
                                       ms_w, ms_h, rm);
                    int cr_w = rw, cr_h = rh, cr_off = 0;
                    if ((mm_rot == 90 || mm_rot == 270) &&
                        rw < rh && (int)dst_w > (int)dst_h) {
                        cr_h = (int)((int64_t)rw * dst_h / dst_w);
                        cr_h = (cr_h + 1) & ~1;
                        if (cr_h > rh) cr_h = rh;
                        cr_off = ((rh - cr_h) / 2) & ~1;
                    }
                    size_t nv12_sz = (size_t)cr_w * cr_h + (size_t)cr_w * ((cr_h+1)/2);
                    mm_rot_buf.resize(nv12_sz);
                    uint8_t *rn_y  = mm_rot_buf.data();
                    uint8_t *rn_uv = rn_y + (size_t)cr_w * cr_h;
                    libyuv::I420ToNV12(ri_y + (size_t)cr_off * rw, rw,
                                       ri_u + (size_t)(cr_off/2) * ruv_w, ruv_w,
                                       ri_v + (size_t)(cr_off/2) * ruv_w, ruv_w,
                                       rn_y, cr_w, rn_uv, cr_w, cr_w, cr_h);
                    ms_y = rn_y; ms_uv = rn_uv;
                    ms_w = cr_w; ms_h = cr_h; ms_stride = cr_w;
                }
                // FIX for UNISOC BG6: 0x22 chroma is runtime-resolvable ([CHROMA A/B]),
                // 0x23=NV21, 0x11=NV12. On the fresh build 0x22 as NV12 was correct on
                // VIDEO but blue on PREVIEW, so we no longer hard-code a single order.
                bool is_nv21 = false;
                if (actual_format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
                    is_nv21 = frame_22_is_nv21(role, dst_w, dst_h);
                    LOGI("inject_yuv: [build=%s] 0x22 role=%s %ux%u -> writing as %s (override=%d default=%d) [CHROMA A/B]",
                         FRAME_BUILD_ID, frame_role_name(role), dst_w, dst_h,
                         is_nv21 ? "NV21" : "NV12", frame_inject_get_chroma_override(), kChromaDefault);
                } else if (actual_format == HAL_PIXEL_FORMAT_YCBCR_420_888) {
                    is_nv21 = frame_23_is_nv21(role, dst_w, dst_h);
                    LOGI("inject_yuv: [build=%s] 0x23 role=%s %ux%u -> writing as %s (override=%d default=%d) [CHROMA A/B]",
                         FRAME_BUILD_ID, frame_role_name(role), dst_w, dst_h,
                         is_nv21 ? "NV21" : "NV12", frame_inject_get_chroma_override(), UNISOC_23_DEFAULT_IS_NV21);
                }
                if (is_nv21) {
                    size_t uv_rows = ((size_t)dst_h + 1) / 2;
                    std::vector<uint8_t> tmp_uv((size_t)mm_stride * uv_rows);
                    libyuv::NV12Scale(ms_y, ms_stride, ms_uv, ms_stride,
                                      ms_w, ms_h,
                                      mm_y, mm_stride, tmp_uv.data(), mm_stride,
                                      (int)dst_w, (int)dst_h, filt);
                    for (size_t r = 0; r < uv_rows; r++) {
                        const uint8_t *srow = tmp_uv.data() + r * mm_stride;
                        uint8_t *drow = mm_uv + r * (size_t)mm_stride;
                        size_t cols = (size_t)((dst_w + 1) / 2) * 2;
                        for (size_t c = 0; c + 1 < cols; c += 2) {
                            drow[c]     = srow[c + 1];
                            drow[c + 1] = srow[c];
                        }
                    }
                } else {
                    libyuv::NV12Scale(ms_y, ms_stride, ms_uv, ms_stride,
                                      ms_w, ms_h,
                                      mm_y, mm_stride, mm_uv, mm_stride,
                                      (int)dst_w, (int)dst_h, filt);
                }
                __sync_synchronize();

                struct dma_buf_sync mm_sync_end = {};
                mm_sync_end.flags = (uint64_t)(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
                ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &mm_sync_end);
                munmap(mm_ptr, mm_sz);

                /* UBWC read-touch — forces GPU to read from CPU-written linear region */
                if (g_lock && platform_is_qcom()) {
                    void *touch = nullptr;
                    if (g_lock(hwb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
                               -1, nullptr, &touch) == 0 && touch) {
                        g_unlock(hwb, nullptr);
                    }
                }
                LOGI("inject_yuv: mmap fallback OK rot=%u %dx%d→%ux%u stride=%d",
                     mm_rot, ms_w, ms_h, dst_w, dst_h, mm_stride);
                *out_release_fence = -1;
                return true;
            }
            /* stride: use AHardwareBuffer_describe result; 0 means opaque/tiled,
             * assume compact linear packing (stride == width). */
            int    fb_stride = (desc.stride > 0) ? (int)desc.stride : (int)dst_w;
            uint8_t *fb_y   = (uint8_t *)fb_cpu;
            uint8_t *fb_uv  = fb_y + (size_t)fb_stride * (size_t)dst_h;

            /* Source pointers */
            int    fs_w = (int)src->width, fs_h = (int)src->height;
            int    fs_stride = (int)src->stride;
            const uint8_t *fs_y  = src->y_plane;
            const uint8_t *fs_uv = src->uv_plane
                                   ? src->uv_plane
                                   : (fs_y + (size_t)fs_stride * (size_t)fs_h);
            if (!fs_y) {
                LOGE("inject_yuv: g_lock fallback: src->y_plane is NULL");
                g_unlock(hwb, nullptr);
                return false;
            }

            /* Apply source_rotation + AR-crop (mirrors the lockPlanes path) */
            uint32_t fb_rot = frame_source_get_total_rotation();
            /* [V19 rot] MTK encoders tag the recording with their own rotation
             * metadata; baking the overlay rotation into the buffer double-
             * rotates the recorded video (sideways, 14.mediatek.5). */
            if (role == STREAM_ROLE_VIDEO && !platform_is_unisoc() && !platform_is_qcom()) {
                /* [V21 rot] V19/V20 shipped rot=0 -> portrait pixels squashed into
                 * the 720p recording and the MTK player's 90CW metadata tipped the
                 * result sideways (14.mediatek.6 screenshot: head at right). The
                 * encoder tags 90CW, so pre-rotate pixels 90CCW (=270) when a
                 * portrait source meets a landscape recording: upright after the
                 * player metadata, and no more aspect squash. Manual 90/270
                 * totals (user overlay) are honored untouched. */
                if ((fb_rot % 180) == 0 && fs_h > fs_w && dst_w > dst_h) fb_rot = 270;
            }
            /* [V10 ROT0] legacy 180-deg first-frame-race fallback removed: it
             * flipped already-upright static media upside-down (OPPO CPH2387 logs). */
            std::vector<uint8_t> fb_rot_buf;
            if (fb_rot == 90 || fb_rot == 180 || fb_rot == 270) {
                libyuv::RotationMode rm =
                    (fb_rot == 90)  ? libyuv::kRotate90  :
                    (fb_rot == 270) ? libyuv::kRotate270 : libyuv::kRotate180;
                int rw = (fb_rot == 90 || fb_rot == 270) ? fs_h : fs_w;
                int rh = (fb_rot == 90 || fb_rot == 270) ? fs_w : fs_h;
                rw = (rw + 1) & ~1; rh = (rh + 1) & ~1;
                int ruv_w = (rw + 1) / 2, ruv_h = (rh + 1) / 2;
                /* NV12 → I420 → rotate → I420 → NV12 */
                std::vector<uint8_t> i420((size_t)fs_w * fs_h +
                                          (size_t)((fs_w+1)/2) * ((fs_h+1)/2) * 2);
                uint8_t *iy = i420.data();
                uint8_t *iu = iy + (size_t)fs_w * fs_h;
                uint8_t *iv = iu + (size_t)((fs_w+1)/2) * ((fs_h+1)/2);
                libyuv::NV12ToI420(fs_y, fs_stride, fs_uv, fs_stride,
                                   iy, fs_w, iu, (fs_w+1)/2, iv, (fs_w+1)/2,
                                   fs_w, fs_h);
                std::vector<uint8_t> ri420((size_t)rw * rh +
                                           (size_t)ruv_w * ruv_h * 2);
                uint8_t *ri_y = ri420.data();
                uint8_t *ri_u = ri_y + (size_t)rw * rh;
                uint8_t *ri_v = ri_u + (size_t)ruv_w * ruv_h;
                libyuv::I420Rotate(iy, fs_w, iu, (fs_w+1)/2, iv, (fs_w+1)/2,
                                   ri_y, rw, ri_u, ruv_w, ri_v, ruv_w,
                                   fs_w, fs_h, rm);
                /* Fix3: AR-crop for 90/270 portrait-to-landscape */
                int cr_w = rw, cr_h = rh, cr_off = 0;
                if ((fb_rot == 90 || fb_rot == 270) &&
                    rw < rh && (int)dst_w > (int)dst_h) {
                    cr_h = (int)((int64_t)rw * dst_h / dst_w);
                    cr_h = (cr_h + 1) & ~1;
                    if (cr_h > rh) cr_h = rh;
                    cr_off = ((rh - cr_h) / 2) & ~1;
                }
                size_t nv12_sz = (size_t)cr_w * cr_h +
                                 (size_t)cr_w * ((cr_h + 1) / 2);
                fb_rot_buf.resize(nv12_sz);
                uint8_t *rn_y  = fb_rot_buf.data();
                uint8_t *rn_uv = rn_y + (size_t)cr_w * cr_h;
                libyuv::I420ToNV12(
                    ri_y + (size_t)cr_off * rw, rw,
                    ri_u + (size_t)(cr_off / 2) * ruv_w, ruv_w,
                    ri_v + (size_t)(cr_off / 2) * ruv_w, ruv_w,
                    rn_y, cr_w, rn_uv, cr_w, cr_w, cr_h);
                fs_y = rn_y; fs_uv = rn_uv;
                fs_w = cr_w; fs_h = cr_h; fs_stride = cr_w;
            }

            // FIX for UNISOC BG6: 0x22 chroma is runtime-resolvable ([CHROMA A/B]),
            // 0x23=NV21, 0x11=NV12.
            bool is_nv21 = false;
            if (actual_format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
                is_nv21 = frame_22_is_nv21(role, dst_w, dst_h);
                LOGI("inject_yuv: [build=%s] 0x22 role=%s %ux%u -> writing as %s (override=%d default=%d) [CHROMA A/B]",
                     FRAME_BUILD_ID, frame_role_name(role), dst_w, dst_h,
                     is_nv21 ? "NV21" : "NV12", frame_inject_get_chroma_override(), kChromaDefault);
            } else if (actual_format == HAL_PIXEL_FORMAT_YCBCR_420_888) {
                is_nv21 = frame_23_is_nv21(role, dst_w, dst_h);
                LOGI("inject_yuv: [build=%s] 0x23 role=%s %ux%u -> writing as %s (override=%d default=%d) [CHROMA A/B]",
                     FRAME_BUILD_ID, frame_role_name(role), dst_w, dst_h,
                     is_nv21 ? "NV21" : "NV12", frame_inject_get_chroma_override(), UNISOC_23_DEFAULT_IS_NV21);
            }
            // SP (0x11) is NV12 on UNISOC, so is_nv21=false
            if (is_nv21) {
                size_t uv_rows = ((size_t)dst_h + 1) / 2;
                std::vector<uint8_t> tmp_uv((size_t)fb_stride * uv_rows);
                libyuv::NV12Scale(fs_y, fs_stride, fs_uv, fs_stride,
                                  fs_w, fs_h,
                                  fb_y, fb_stride, tmp_uv.data(), fb_stride,
                                  (int)dst_w, (int)dst_h, filt);
                for (size_t r = 0; r < uv_rows; r++) {
                    const uint8_t *srow = tmp_uv.data() + r * fb_stride;
                    uint8_t *drow = fb_uv + r * (size_t)fb_stride;
                    size_t cols = (size_t)((dst_w + 1) / 2) * 2;
                    for (size_t c = 0; c + 1 < cols; c += 2) {
                        drow[c]     = srow[c + 1];
                        drow[c + 1] = srow[c];
                    }
                }
            } else {
                libyuv::NV12Scale(fs_y, fs_stride, fs_uv, fs_stride,
                                  fs_w, fs_h,
                                  fb_y, fb_stride, fb_uv, fb_stride,
                                  (int)dst_w, (int)dst_h, filt);
            }
            __sync_synchronize();
            g_unlock(hwb, nullptr);
            /* UBWC read-touch for OPlus/Qualcomm cache coherency */
            if (platform_is_qcom()) {
                void *touch = nullptr;
                if (g_lock(hwb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
                           -1, nullptr, &touch) == 0 && touch) {
                    g_unlock(hwb, nullptr);
                }
            }
            LOGI("inject_yuv: g_lock fallback OK rot=%u %dx%d→%ux%u",
                 fb_rot, fs_w, fs_h, dst_w, dst_h);
            *out_release_fence = -1;
            return true;
        }

        LOGD("inject_yuv: lockPlanes OK planeCount=%u", planes.planeCount);
        for (uint32_t p = 0; p < planes.planeCount && p < 3; p++) {
            LOGD("  plane[%u] data=%p rowStride=%u pixelStride=%u",
                 p, planes.planes[p].data,
                 planes.planes[p].rowStride, planes.planes[p].pixelStride);
        }

        if (planes.planeCount < 1) {
            LOGE("inject_yuv: planeCount=0 — cannot write");
            g_unlock(hwb, nullptr);
            return false;
        }

        uint8_t *dst_y  = (uint8_t *)planes.planes[0].data;
        int dst_stride  = (int)planes.planes[0].rowStride;

        if (dst_stride <= 0) {
            dst_stride = (desc.stride > 0) ? (int)desc.stride : (int)dst_w;
            LOGI("inject_yuv: plane[0] rowStride was 0 — defaulted to %d (desc.stride=%u dst_w=%u)",
                 dst_stride, desc.stride, dst_w);
        }

        // V5: infer stride from Y->UV pointer diff when both strides are 0 (UNISOC BG6 opaque)
        if (dst_stride == (int)dst_w && planes.planeCount == 1 && planes.planes[0].data) {
            // Try to get UV pointer if we can, will be computed later, but for now log
            LOGD("inject_yuv: V5 stride inference check dst_y=%p dst_w=%u dst_h=%u desc.stride=%u rowStride=%u",
                 dst_y, dst_w, dst_h, desc.stride, planes.planes[0].rowStride);
        }

        if (!dst_y || dst_stride <= 0) {
            LOGE("inject_yuv: plane[0] data=%p rowStride=%d — invalid",
                 (void *)dst_y, dst_stride);
            g_unlock(hwb, nullptr);
            return false;
        }

        if (actual_format == HAL_PIXEL_FORMAT_YCBCR_P010) {
            LOGW("inject_yuv: P010 not supported, skipping");
            g_unlock(hwb, nullptr);
            return false;
        }

        bool is_planar = false;
        bool is_nv21   = false;
        uint8_t *dst_uv = nullptr;
        int dst_uv_stride = 0;
        uint8_t *dst_u = nullptr, *dst_v = nullptr;
        int du_stride = 0, dv_stride = 0;

        if (planes.planeCount >= 3 &&
            planes.planes[1].pixelStride == 1 &&
            planes.planes[2].pixelStride == 1) {
            /* Planar 3-plane (I420 or YV12) */
            is_planar = true;
            if (actual_format == HAL_PIXEL_FORMAT_YV12) {
                /* [V12 YV12-ORDER] The spec says YV12 memory is Y,V,U, but this
                 * HAL's AHardwareBuffer_lockPlanes returns the NORMALIZED flex
                 * order (planes[1]=U, planes[2]=V) even for YV12: on the OPPO
                 * CPH2387 (14.mediatek.2 @ 235da71) the old spec-swap rendered
                 * the 960x720 preview BLUE and the 1280x720 recording whitish.
                 * Default to the normalized order; chroma_override==1 restores
                 * the raw spec swap for HALs that really return Y,V,U. */
                static bool yv12_order_logged = false;
                const bool yv12_raw_swap = frame_inject_get_chroma_override() == 1;
                if (!yv12_order_logged) {
                    LOGI("inject_yuv: [build=%s] V12 YV12 plane order: %s (override=%d)",
                         FRAME_BUILD_ID, yv12_raw_swap ? "raw Y,V,U swap" : "normalized planes[1]=U",
                         frame_inject_get_chroma_override());
                    yv12_order_logged = true;
                }
                if (yv12_raw_swap) {
                    dst_u = (uint8_t *)planes.planes[2].data;
                    du_stride = (int)planes.planes[2].rowStride;
                    dst_v = (uint8_t *)planes.planes[1].data;
                    dv_stride = (int)planes.planes[1].rowStride;
                } else {
                    dst_u = (uint8_t *)planes.planes[1].data;
                    du_stride = (int)planes.planes[1].rowStride;
                    dst_v = (uint8_t *)planes.planes[2].data;
                    dv_stride = (int)planes.planes[2].rowStride;
                }
            } else {
                /* Standard I420 (0x23 planar): plane 1 is U, plane 2 is V */
                dst_u = (uint8_t *)planes.planes[1].data;
                du_stride = (int)planes.planes[1].rowStride;
                dst_v = (uint8_t *)planes.planes[2].data;
                dv_stride = (int)planes.planes[2].rowStride;
            }
            if (du_stride <= 0) du_stride = (dst_stride + 1) / 2;
            if (dv_stride <= 0) dv_stride = (dst_stride + 1) / 2;
        } else {
            /* Semi-planar (NV12 or NV21) or single-plane fallback.
             * FIX for TECNO BG6 UNISOC T603 (mali_gralloc):
             *   YCbCr420-888 (0x23) -> NV21
             *   YCbCr420-SP  (0x11) -> NV12
             *   IMPLEMENTATION_DEFINED (0x22) -> NV12  (proven on-device: the same 0x22
             *     rendered correct color when written without a U/V swap on the role=2
             *     VIDEO stream, and BLUE when swapped on the role=1 PREVIEW stream.)
             * Original code forced SP=NV21 and 0x22=NV21, both wrong on this device and the
             * cause of the blue tint. 0x22 must be written as NV12 (no swap). */
            if (planes.planeCount >= 3 && planes.planes[1].data && planes.planes[2].data) {
                /* 3-plane semi-planar with shared UV buffer:
                 * Spec: plane[1]=U, plane[2]=V. If V<U => VU => NV21
                 * This matches mali_gralloc logs on TECNO BG6: YCbCr420-888 -> NV21 */
                if ((uintptr_t)planes.planes[2].data < (uintptr_t)planes.planes[1].data) {
                    is_nv21 = true;
                }
                // Heuristic for shared interleaved buffer diff=1
                uintptr_t diff = (uintptr_t)planes.planes[1].data > (uintptr_t)planes.planes[2].data ?
                                 (uintptr_t)planes.planes[1].data - (uintptr_t)planes.planes[2].data :
                                 (uintptr_t)planes.planes[2].data - (uintptr_t)planes.planes[1].data;
                if (diff == 1) {
                    LOGD("inject_yuv: shared UV buffer diff=1, is_nv21=%d (U=%p V=%p)",
                         (int)is_nv21, planes.planes[1].data, planes.planes[2].data);
                }
                // Override based on known UNISOC mappings
                if (actual_format == HAL_PIXEL_FORMAT_YCrCb_420_SP) {
                    // SP on UNISOC is NV12 per mali log, even if pointer order says NV21
                    // Keep detection result but log; for safety force NV12 on this device
                    // We will NOT force NV21 for SP anymore.
                    // If detection said NV21, it might be a different device, so keep it.
                    // For BG6 specifically, we want NV12, so if format is SP, set false
                    // unless we are sure it's NV21 from other evidence.
                    // To fix blue on BG6, we set SP to NV12.
                    is_nv21 = false;
                    LOGI("inject_yuv: fmt=0x11 SP on UNISOC BG6 -> forcing NV12 (is_nv21=0) per mali_gralloc log");
                } else if (actual_format == HAL_PIXEL_FORMAT_YCBCR_420_888) {
                    is_nv21 = frame_23_is_nv21(role, dst_w, dst_h);
                    LOGI("inject_yuv: fmt=0x23 420_888 -> writing as %s (override=%d default=%d) [CHROMA A/B]",
                         is_nv21 ? "NV21" : "NV12", frame_inject_get_chroma_override(), UNISOC_23_DEFAULT_IS_NV21);
                }
            } else {
                // planeCount 1 or 2 fallback - no pointer order to detect
                if (actual_format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
                    // UNISOC BG6: opaque 0x22 (IMPLEMENTATION_DEFINED) is consumed as
                    // NV12 (U,V) — proven by the on-device log where the same 0x22 written
                    // without swap (role=2 VIDEO 1280x720) renders CORRECT color while the
                    // 0x22 written with a U/V swap (role=1 PREVIEW 960x720) is BLUE.
                    is_nv21 = frame_22_is_nv21(role, dst_w, dst_h);
                    LOGI("inject_yuv: [build=%s] fmt=0x22 IMPLEMENTATION_DEFINED role=%s %ux%u planeCount=%u -> writing as %s (override=%d default=%d) [CHROMA A/B]",
                         FRAME_BUILD_ID, frame_role_name(role), dst_w, dst_h, planes.planeCount,
                         is_nv21 ? "NV21" : "NV12", frame_inject_get_chroma_override(), kChromaDefault);
                } else if (actual_format == HAL_PIXEL_FORMAT_YCBCR_420_888) {
                    is_nv21 = frame_23_is_nv21(role, dst_w, dst_h);
                    LOGI("inject_yuv: fmt=0x23 420_888 planeCount=%u -> writing as %s (override=%d default=%d) [CHROMA A/B]",
                         planes.planeCount, is_nv21 ? "NV21" : "NV12", frame_inject_get_chroma_override(), UNISOC_23_DEFAULT_IS_NV21);
                } else if (actual_format == HAL_PIXEL_FORMAT_YCrCb_420_SP) {
                    is_nv21 = false;
                    LOGI("inject_yuv: fmt=0x11 SP planeCount=%u -> forcing NV12 for UNISOC BG6 per mali_gralloc", planes.planeCount);
                }
            }

            if (planes.planeCount >= 3) {
                dst_uv = (uint8_t *)(is_nv21 ? planes.planes[2].data : planes.planes[1].data);
                dst_uv_stride = (int)(is_nv21 ? planes.planes[2].rowStride : planes.planes[1].rowStride);
                if (dst_uv_stride <= 0) {
                    dst_uv_stride = (int)(is_nv21 ? planes.planes[1].rowStride : planes.planes[2].rowStride);
                }
            } else if (planes.planeCount == 2) {
                dst_uv = (uint8_t *)planes.planes[1].data;
                dst_uv_stride = (int)planes.planes[1].rowStride;
            } else {
                /* planeCount == 1 - single opaque plane containing Y+UV contiguous
                 * FIX: use actual gralloc stride if available, not just width */
                dst_uv = dst_y + (size_t)dst_stride * (size_t)dst_h;
                dst_uv_stride = dst_stride;
                // For NV21 single-plane, UV is still interleaved VU after Y
                LOGD("inject_yuv: planeCount=1 fallback dst_y=%p dst_uv=%p stride=%d is_nv21=%d", 
                     dst_y, dst_uv, dst_stride, (int)is_nv21);
            }
            if (dst_uv_stride <= 0) dst_uv_stride = dst_stride;
        }

        LOGI("inject_yuv: [build=%s] format detected: fmt=0x%x is_planar=%d is_nv21=%d planeCount=%u dst_stride=%d dst_uv_stride=%d (ver=%s)",
             FRAME_BUILD_ID, actual_format, (int)is_planar, (int)is_nv21, planes.planeCount, dst_stride, dst_uv_stride,
             FRAME_INJECT_VERSION);

        /* [V26 conv-cache] replay the cached destination image when this role
         * is re-injecting the same source frame with identical layout — the
         * camera fires at 30fps per stream but the producer writes at source
         * rate, so most fires would redo the full 2MP libyuv chain for
         * nothing (the A14 slow-motion root cause). */
        if (role == STREAM_ROLE_PREVIEW || role == STREAM_ROLE_VIDEO) {
            ConvCache &cc = g_conv_cache[role & 3];
            std::lock_guard<std::mutex> cg(cc.mu);
            if (cc.valid &&
                cc.key == conv_cache_key(role, dst_w, dst_h, dst_stride, is_planar, is_nv21) &&
                conv_cache_replay(cc, dst_y, dst_u, dst_v, dst_uv)) {
                __sync_synchronize();
                if (dmabuf_fd >= 0) {
                    struct dma_buf_sync cs = {};
                    cs.flags = (uint64_t)(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
                    ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &cs);
                }
                g_unlock(hwb, nullptr);
                *out_release_fence = -1;
                return true;
            }
        }














        /* Bug3 fix: mprotect() removed — always EPERM/EACCES under modern Android
         * SELinux, costs a wasted syscall every frame.  CPU write access is
         * guaranteed by GRALLOC_USAGE_SW_WRITE_OFTEN set via
         * stream_modify_usage_for_injection() + my_setusage_proxy. */

        int src_w = (int)src->width, src_h = (int)src->height, src_stride = (int)src->stride;
        if (!src->y_plane) {
            LOGE("inject_yuv: src->y_plane is NULL");
            g_unlock(hwb, nullptr);
            return false;
        }

        /* ── Pan/zoom crop ──────────────────────────────────────────────────
         * Read overlay params written by FaceGate via nativeSetOverlayParams()
         * into the shared ashmem FrameSourceHeader.  When scale_q16 > 65536
         * (zoom in), we crop a sub-rectangle of the source frame and scale
         * that crop up to fill the destination — real-time zoom with no IPC.
         * pan_x / pan_y shift the crop centre in source pixels.
         * All reads are lock-free atomic relaxed loads. */
        const uint8_t *eff_y  = src->y_plane;
        const uint8_t *eff_uv = src->uv_plane;
        int orig_src_h        = src_h;   /* for packed-UV fallback (null uv_plane) */
        {
            int32_t  ov_pan_x  = 0;
            int32_t  ov_pan_y  = 0;
            uint32_t ov_scale  = 65536u;
            frame_source_get_overlay_params(&ov_pan_x, &ov_pan_y, &ov_scale);

            /* [V26 deadband] a zoomIn->zoomOut round trip lands at 65535
             * (72090*59578/65536^2 = 0.99998), which dragged EVERY injection
             * into the zoom-out letterbox branch (14.mediatek A14 V25 log:
             * scale_q16=65535 on every frame).  That branch's planar VIDEO
             * write used the flex layout while the MTK encoder reads the
             * lower chroma region interleaved => grayscale recordings.
             * Treat ~1.0 with no pan as exactly 1.0 (no zoom). */
            if (ov_scale >= 65500u && ov_scale <= 65536u && ov_pan_x == 0 && ov_pan_y == 0)
                ov_scale = 65536u;

            /* Fix3: apply pan at any zoom level — previously pan was silently ignored at 1x.
             * Condition now triggers whenever pan is non-zero OR zoom is active. */
            if (ov_scale >= 65536u && ov_scale <= 65536u * 32u && src_w > 0 && src_h > 0 &&
                (ov_scale > 65536u || ov_pan_x != 0 || ov_pan_y != 0)) {
                /* Fix2: pan at exactly 1× zoom — at 1×, crop_w == src_w so the
                 * clamp range [0, src_w − crop_w] collapses to [0,0] and every
                 * pan value is silently discarded.  Bumping to an effective scale
                 * of ~1.09× gives ~8% crop headroom (~106 px at 1280-wide source)
                 * so the crop window can actually shift when the user pans. */
                uint32_t effective_scale = ov_scale;
                if (effective_scale == 65536u && (ov_pan_x != 0 || ov_pan_y != 0)) {
                    effective_scale = 71663u;  /* ≈ 1.093× minimum for pan headroom */
                }
                int crop_w = (int)((int64_t)src_w * 65536 / (int64_t)effective_scale);
                int crop_h = (int)((int64_t)src_h * 65536 / (int64_t)effective_scale);
                crop_w = (crop_w < 2) ? 2 : (crop_w & ~1);
                crop_h = (crop_h < 2) ? 2 : (crop_h & ~1);

                /* [V77] Rotate the pan vector into SOURCE space. The crop
                 * runs on the un-rotated ring frame and the result is only
                 * rotated upright afterwards, so on a rotated display the
                 * buttons moved the wrong AXIS (Tab A8: press left -> frame
                 * went up) and the wrong way (crop-shift instead of
                 * content-follow). Transform: press direction (ux,uy) in
                 * screen space -> crop delta R^-1(-ux,-uy) in source space. */
                uint32_t pan_rot = frame_source_get_total_rotation();
                int32_t pan_dx, pan_dy;
                switch (pan_rot) {
                    case 90:  pan_dx = -ov_pan_y; pan_dy =  ov_pan_x; break;
                    case 180: pan_dx =  ov_pan_x; pan_dy =  ov_pan_y; break;
                    case 270: pan_dx =  ov_pan_y; pan_dy = -ov_pan_x; break;
                    default:  pan_dx = -ov_pan_x; pan_dy = -ov_pan_y; break;
                }
                int cx = (src_w - crop_w) / 2 + pan_dx;
                int cy = (src_h - crop_h) / 2 + pan_dy;
                if (cx < 0) cx = 0;
                if (cx > src_w - crop_w) cx = src_w - crop_w;
                if (cy < 0) cy = 0;
                if (cy > src_h - crop_h) cy = src_h - crop_h;
                cx &= ~1; cy &= ~1;

                eff_y = src->y_plane + (size_t)cy * src_stride + cx;
                if (src->uv_plane) {
                    eff_uv = src->uv_plane + (size_t)(cy / 2) * src_stride + cx;
                }
                /* Reduce effective source dims so libyuv scales the crop to fill dst. */
                src_w = crop_w;
                src_h = crop_h;

                LOGD("inject_yuv: pan/zoom crop=%dx%d at (%d,%d) scale_q16=%u",
                     crop_w, crop_h, cx, cy, ov_scale);

            } else if (ov_scale > 0 && ov_scale < 65536u && src_w > 0 && src_h > 0) {
                /* ── Zoom-out letterbox (scale_q16 in range [32768, 65535]) ──
                 * BUG FIX: the buffer is ALREADY locked by the outer g_lockPlanes call
                 * above. The old code called g_lockPlanes a SECOND time here (which
                 * failed — buffer already locked), skipped the write, then jumped to
                 * inject_done WITHOUT calling g_unlock → buffer stayed locked forever →
                 * HAL starved of buffers → real camera bleed-through.
                 * Fix: use the already-locked dst_y / dst_stride / planes from the
                 * outer lock. Call g_unlock once at the end before goto inject_done. */

                /* Scaled content dimensions (even-aligned) */
                int scaled_w = (int)((int64_t)dst_w * (int64_t)ov_scale / 65536LL);
                int scaled_h = (int)((int64_t)dst_h * (int64_t)ov_scale / 65536LL);
                scaled_w = (scaled_w < 2) ? 2 : (scaled_w & ~1);
                scaled_h = (scaled_h < 2) ? 2 : (scaled_h & ~1);
                if (scaled_w > (int)dst_w) scaled_w = (int)dst_w & ~1;
                if (scaled_h > (int)dst_h) scaled_h = (int)dst_h & ~1;

                /* Pan offset in destination pixels, clamped so content stays visible */
                int off_x = ((int)dst_w - scaled_w) / 2 + ov_pan_x;
                int off_y_lb = ((int)dst_h - scaled_h) / 2 + ov_pan_y;
                if (off_x < 0) off_x = 0;
                if (off_x > (int)dst_w - scaled_w) off_x = (int)dst_w - scaled_w;
                if (off_y_lb < 0) off_y_lb = 0;
                if (off_y_lb > (int)dst_h - scaled_h) off_y_lb = (int)dst_h - scaled_h;
                off_x &= ~1; off_y_lb &= ~1;

                /* Use the already-locked planes from the outer g_lockPlanes call. */
                if (dst_y && dst_stride > 0) {
                    /* Fill Y plane with black (BT.601 value 16) */
                    for (uint32_t r = 0; r < dst_h; r++)
                        memset(dst_y + (size_t)r * dst_stride, 16, dst_w);

                    const uint8_t *src_uv2 = src->uv_plane
                        ? src->uv_plane
                        : (src->y_plane + (size_t)src_stride * src->height);
                    uint8_t *sub_y = dst_y + (size_t)off_y_lb * dst_stride + off_x;

                    /* Fix V4.8.2: apply the same source rotation here that the
                     * zoom-in / 1x path applies in Fix1. Rotate the full source
                     * into a temp NV12 buffer, then letterbox-scale it. */
                    const uint8_t *rot_y  = eff_y;
                    const uint8_t *rot_uv = src_uv2;
                    int   rot_stride     = src_stride;
                    int   rot_w          = src_w;
                    int   rot_h          = src_h;
                    std::vector<uint8_t> rot_buf_lb;
                    uint32_t rot_lb = rotate_source_upright(
                        eff_y, src_uv2, src_stride, src_w, src_h,
                        rot_buf_lb, rot_y, rot_uv, rot_stride, rot_w, rot_h);

                    int uv_w = (rot_w + 1) / 2, uv_h = (rot_h + 1) / 2;
                    std::vector<uint8_t> tmp_u((size_t)uv_w * uv_h), tmp_v((size_t)uv_w * uv_h);
                    for (int y = 0; y < uv_h; y++) {
                        const uint8_t *row = rot_uv + (size_t)y * rot_stride;
                        uint8_t *ru = tmp_u.data() + (size_t)y * uv_w;
                        uint8_t *rv = tmp_v.data() + (size_t)y * uv_w;
                        for (int x = 0; x < uv_w; x++) {
                            ru[x] = row[x * 2];
                            rv[x] = row[x * 2 + 1];
                        }
                    }
                    if (is_planar && dst_u && dst_v && role == STREAM_ROLE_VIDEO) {
                        /* [V26 gray fix] the MTK A14 HW encoder reads the FIRST
                         * chroma region as interleaved Cb/Cr even when lockPlanes
                         * reports a flex 3-plane layout — the pre-V26 flex write
                         * left it interpreting V-plane bytes as UV pairs => the
                         * grayscale recordings in the V25 retest.  Same fix the
                         * non-letterbox VIDEO path has shipped since V17. */
                        uint8_t *semi = (dst_u < dst_v) ? dst_u : dst_v;
                        for (uint32_t r = 0; r < dst_h / 2; r++)
                            memset(semi + (size_t)r * dst_stride, 128, dst_w & ~1u);
                        int chw = ((int)dst_w + 1) / 2, chh = ((int)dst_h + 1) / 2;
                        std::vector<uint8_t> su2((size_t)chw * chh), sv2((size_t)chw * chh);
                        libyuv::I420Scale(rot_y, rot_stride,
                                          tmp_u.data(), uv_w, tmp_v.data(), uv_w,
                                          rot_w, rot_h,
                                          sub_y, dst_stride,
                                          su2.data(), chw, sv2.data(), chw,
                                          scaled_w, scaled_h, filt);
                        for (int y = 0; y < scaled_h / 2; y++) {
                            uint8_t *drow = semi + ((size_t)(off_y_lb / 2) + y) * dst_stride + off_x;
                            const uint8_t *ru = su2.data() + (size_t)y * chw;
                            const uint8_t *rv = sv2.data() + (size_t)y * chw;
                            for (int x = 0; x < scaled_w / 2; x++) {
                                drow[x * 2]     = ru[x];
                                drow[x * 2 + 1] = rv[x];
                            }
                        }
                    } else if (is_planar && dst_u && dst_v) {
                        for (uint32_t r = 0; r < dst_h / 2; r++) {
                            memset(dst_u + (size_t)r * du_stride, 128, (dst_w + 1) / 2);
                            memset(dst_v + (size_t)r * dv_stride, 128, (dst_w + 1) / 2);
                        }
                        uint8_t *sub_u = dst_u + (size_t)(off_y_lb / 2) * du_stride + (off_x / 2);
                        uint8_t *sub_v = dst_v + (size_t)(off_y_lb / 2) * dv_stride + (off_x / 2);
                        libyuv::I420Scale(rot_y, rot_stride,
                                          tmp_u.data(), uv_w, tmp_v.data(), uv_w,
                                          rot_w, rot_h,
                                          sub_y, dst_stride,
                                          sub_u, du_stride, sub_v, dv_stride,
                                          scaled_w, scaled_h, filt);
                    } else if (dst_uv && dst_uv_stride > 0) {
                        for (uint32_t r = 0; r < dst_h / 2; r++)
                            memset(dst_uv + (size_t)r * dst_uv_stride, 128, (dst_w + 1) & ~1u);

                        uint8_t *sub_uv = dst_uv + (size_t)(off_y_lb / 2) * dst_uv_stride + off_x;
                        if (is_nv21) {
                            size_t scaled_uv_rows = ((size_t)scaled_h + 1) / 2;
                            std::vector<uint8_t> tmp_uv((size_t)dst_uv_stride * scaled_uv_rows);
                            libyuv::NV12Scale(rot_y, rot_stride, rot_uv, rot_stride,
                                              rot_w, rot_h,
                                              sub_y, dst_stride, tmp_uv.data(), dst_uv_stride,
                                              scaled_w, scaled_h, filt);
                            for (size_t r = 0; r < scaled_uv_rows; r++) {
                                const uint8_t *srow = tmp_uv.data() + r * dst_uv_stride;
                                uint8_t *drow = sub_uv + r * (size_t)dst_uv_stride;
                                size_t cols = (size_t)((scaled_w + 1) / 2) * 2;
                                for (size_t c = 0; c + 1 < cols; c += 2) {
                                    drow[c]     = srow[c + 1];
                                    drow[c + 1] = srow[c];
                                }
                            }
                        } else {
                            libyuv::NV12Scale(rot_y, rot_stride, rot_uv, rot_stride,
                                              rot_w, rot_h,
                                              sub_y, dst_stride, sub_uv, dst_uv_stride,
                                              scaled_w, scaled_h, filt);
                        }
                    }
                    LOGD("inject_yuv: zoom-out letterbox scale_q16=%u scaled=%dx%d off=(%d,%d) rot=%u is_nv21=%d is_planar=%d",
                         ov_scale, scaled_w, scaled_h, off_x, off_y_lb, rot_lb, (int)is_nv21, (int)is_planar);
                }
                /* [V26] cache the finished destination image for replay. */
                conv_cache_store(role, dst_y, dst_u, dst_v, dst_uv, dst_stride,
                                 du_stride, dv_stride, dst_uv_stride,
                                 dst_w, dst_h, is_planar, is_nv21);

                /* Unlock the outer lock before returning — this was the root cause
                 * of the "real camera on zoom-out" bug (buffer left locked). */
                g_unlock(hwb, nullptr);
                return true;
            }
        }


        /* Fix1: Apply source rotation written by frame_producer into the ring header.
         * V5 REWRITE: avoid rot_buf.insert reallocation that invalidated pointers and
         * could cause right-side striped artifact. Use separate final NV12 buffer. */
        std::vector<uint8_t> rot_buf;
        /* [V57b] when source rotation is applied the rotated frame is kept as
         * I420 directly (fused NV12ToI420Rotate) — planar write paths consume
         * these planes and skip the NV12 deinterleave; the semi-planar path
         * merges U/V on demand. */
        bool post_rot_i420 = false;
        const uint8_t *rot_u_ptr = nullptr;
        const uint8_t *rot_v_ptr = nullptr;
        int rot_uv_stride = 0;
        {
            /* [V10 ROT0] honor total==0 (upright); the legacy 180° first-frame
             * fallback flipped already-upright static media upside-down. */
            uint32_t src_rot = frame_source_get_total_rotation();
            /* [V19 rot] MTK encoders tag the recording with their own rotation
             * metadata; baking the overlay rotation into the buffer double-
             * rotates the recorded video (sideways, 14.mediatek.5). */
            if (role == STREAM_ROLE_VIDEO && !platform_is_unisoc() && !platform_is_qcom()) {
                /* [V21 rot] V19/V20 shipped rot=0 -> portrait pixels squashed into
                 * the 720p recording and the MTK player's 90CW metadata tipped the
                 * result sideways (14.mediatek.6 screenshot: head at right). The
                 * encoder tags 90CW, so pre-rotate pixels 90CCW (=270) when a
                 * portrait source meets a landscape recording: upright after the
                 * player metadata, and no more aspect squash. Manual 90/270
                 * totals (user overlay) are honored untouched. */
                if ((src_rot % 180) == 0 && src_h > src_w && dst_w > dst_h) src_rot = 270;
            }
            if (src_rot == 90 || src_rot == 180 || src_rot == 270) {
                libyuv::RotationMode rot_mode =
                    (src_rot == 90)  ? libyuv::kRotate90  :
                    (src_rot == 270) ? libyuv::kRotate270 : libyuv::kRotate180;

                int pre_w = src_w, pre_h = src_h;
                int post_w = (src_rot == 90 || src_rot == 270) ? pre_h : pre_w;
                int post_h = (src_rot == 90 || src_rot == 270) ? pre_w : pre_h;
                post_w = (post_w + 1) & ~1;
                post_h = (post_h + 1) & ~1;

                int r_uv_w = (post_w + 1) / 2, r_uv_h = (post_h + 1) / 2;
                /* [V57 SPEED] single-pass NV12Rotate replaces the old
                 * NV12ToI420 -> I420Rotate -> I420ToNV12 triple: three
                 * full-frame passes removed per video frame. On the mt6765
                 * (8×A53) the old chain cost ~40 ms/frame extra — the direct
                 * cause of the "video is slow" complaint on CPH2387. */
                size_t i420_rot_sz = (size_t)post_w * post_h
                                   + 2 * (size_t)r_uv_w * r_uv_h;
                rot_buf.resize(i420_rot_sz);

                uint8_t *ri_y = rot_buf.data();
                uint8_t *ri_u = ri_y + (size_t)post_w * post_h;
                uint8_t *ri_v = ri_u + (size_t)r_uv_w * r_uv_h;

                const uint8_t *uv = eff_uv ? eff_uv : (eff_y + (size_t)src_stride * src_h);
                // V5: ensure we don't read beyond src bounds — clamp src_w/h to stride
                int safe_pre_w = std::min(pre_w, src_stride);
                if (safe_pre_w != pre_w) {
                    LOGW("inject_yuv: Fix1 clamping pre_w %d -> %d to match stride %d", pre_w, safe_pre_w, src_stride);
                    pre_w = safe_pre_w;
                }
                pre_w &= ~1; pre_h &= ~1;   // rotation needs even dims
                libyuv::NV12ToI420Rotate(eff_y, src_stride,
                                         uv,    src_stride,
                                         ri_y,  post_w,
                                         ri_u,  r_uv_w,
                                         ri_v,  r_uv_w,
                                         pre_w, pre_h, rot_mode);
                eff_y    = ri_y;
                eff_uv   = ri_u;   // non-null; NV12 consumers gated on post_rot_i420
                post_rot_i420 = true;
                rot_u_ptr = ri_u;
                rot_v_ptr = ri_v;
                rot_uv_stride = r_uv_w;
                src_w    = post_w;
                src_h    = post_h;
                src_stride = post_w;
                LOGI("inject_yuv: Fix1 V57b source_rotation=%u applied %dx%d->%dx%d swap=%d final_buf=%p [NV12ToI420Rotate fused]",
                     src_rot, pre_w, pre_h, post_w, post_h,
                     (int)(src_rot == 90 || src_rot == 270), (void*)eff_y);

                /* Fix3: center-crop rotated portrait to match destination AR.
                 * After 90°/270°: src is 720×1280 portrait, dst is 640×480 landscape.
                 * Scaling 720×1280 → 640×480 compresses 1280 rows to 480 (factor 2.67)
                 * causing severe squish and UV green-stripe artifacts in the top portion.
                 * Center-crop the portrait to the dst AR first (720×540 for a 4:3 dst)
                 * so NV12Scale only does 720×540 → 640×480 (clean 0.89× downscale).    */
                if (dst_w > 0 && dst_h > 0 && src_w > 0 && src_h > 0) {
                    if ((int64_t)src_w * (int64_t)dst_h !=
                        (int64_t)src_h * (int64_t)dst_w) {
                        int crop_w2 = src_w, crop_h2 = src_h;
                        if ((int64_t)src_w * (int64_t)dst_h <
                            (int64_t)src_h * (int64_t)dst_w) {
                            /* src narrower than dst AR — crop height */
                            crop_h2 = (int)((int64_t)src_w * (int64_t)dst_h
                                            / (int64_t)dst_w) & ~1;
                        } else {
                            /* src wider than dst AR — crop width */
                            crop_w2 = (int)((int64_t)src_h * (int64_t)dst_w
                                            / (int64_t)dst_h) & ~1;
                        }
                        if (crop_w2 > 0 && crop_h2 > 0 &&
                            (crop_w2 < src_w || crop_h2 < src_h)) {
                            int off_x2 = ((src_w - crop_w2) / 2) & ~1;
                            int off_y2 = ((src_h - crop_h2) / 2) & ~1;
                            eff_y  += (size_t)off_y2 * src_stride + off_x2;
                            if (post_rot_i420) {
                                size_t uoff = (size_t)(off_y2 / 2) * rot_uv_stride
                                            + (size_t)(off_x2 / 2);
                                rot_u_ptr += uoff;
                                rot_v_ptr += uoff;
                            } else {
                                eff_uv += (size_t)(off_y2 / 2) * src_stride + off_x2;
                            }
                            LOGD("inject_yuv: Fix3 AR-crop rot=%u "
                                 "(%dx%d)→(%dx%d) off=(%d,%d) dst=%ux%u",
                                 src_rot, src_w, src_h, crop_w2, crop_h2,
                                 off_x2, off_y2, dst_w, dst_h);
                            src_w = crop_w2;
                            src_h = crop_h2;
                        }
                    }
                }
            }
        }



        bool ok = false;
        if (is_planar && dst_u && dst_v) {
            int uv_w = (src_w + 1) / 2, uv_h = (src_h + 1) / 2;
            std::vector<uint8_t> tmp_u, tmp_v;
            const uint8_t *pu; const uint8_t *pv; int pu_stride;
            if (post_rot_i420) {
                /* [V57b] rotated source is already I420 — planes used
                 * directly; the deinterleave loop is skipped entirely. */
                pu = rot_u_ptr; pv = rot_v_ptr; pu_stride = rot_uv_stride;
            } else {
                tmp_u.assign((size_t)uv_w * uv_h, 0);
                tmp_v.assign((size_t)uv_w * uv_h, 0);
                const uint8_t *suv = eff_uv ? eff_uv : (eff_y + (size_t)src_stride * src_h);
                for (int y = 0; y < uv_h; y++) {
                    const uint8_t *row = suv + (size_t)y * src_stride;
                    uint8_t *ru = tmp_u.data() + (size_t)y * uv_w;
                    uint8_t *rv = tmp_v.data() + (size_t)y * uv_w;
                    for (int x = 0; x < uv_w; x++) {
                        ru[x] = row[x * 2];
                        rv[x] = row[x * 2 + 1];
                    }
                }
                pu = tmp_u.data(); pv = tmp_v.data(); pu_stride = uv_w;
            }
            if (role == STREAM_ROLE_VIDEO) {
                /* [V17 MTK-A14 gray-video fix] The HW encoder ignores the flex
                 * plane pointers and reads the FIRST chroma region as
                 * interleaved Cb/Cr pairs (semi-planar). Contiguous planar
                 * writes there give Cb==Cr -> grayscale recordings
                 * (14.mediatek.4 screenshot). Interleave NV12-style into the
                 * first chroma region instead; preview-mode YV12 (role=1)
                 * keeps the contiguous flex layout that renders correctly. */
                int chw = ((int)dst_w + 1) / 2, chh = ((int)dst_h + 1) / 2;
                std::vector<uint8_t> su2((size_t)chw * chh), sv2((size_t)chw * chh);
                libyuv::I420Scale(eff_y, src_stride,
                                  pu, pu_stride, pv, pu_stride,
                                  src_w, src_h,
                                  dst_y, dst_stride,
                                  su2.data(), chw, sv2.data(), chw,
                                  (int)dst_w, (int)dst_h, filt);
                /* [V20 crash revert] V19 wrote NV12 (w*h/2) into the HIGHER
                 * chroma region — but that region is only w*h/4 bytes, so the
                 * loop ran off the mapped buffer (cameraserver SIGSEGV,
                 * 14.mediatek.1 tombstone_00). R1+R2 are contiguous, so the
                 * V18 layout (NV12 across both, starting at the lower region)
                 * is the only provably in-bounds placement. */
                uint8_t *semi = (dst_u < dst_v) ? dst_u : dst_v;  // R1 (safe)
                /* [V57 diag] PERIODIC buffer layout dump (every 300th frame —
                 * the V20 one-shot fired before the log-upload window and was
                 * lost). Compares lockPlanes chroma offsets against the spec
                 * YV12 layout (Y, then V, then U with ALIGN16 strides) so the
                 * gray-video mystery is solved with measured offsets. */
                static int layout_count = 0;
                if ((layout_count++ % 300) == 0) {
                    long alloc_sz = (dmabuf_fd >= 0) ? (long)lseek(dmabuf_fd, 0, SEEK_END) : -1;
                    uint32_t y_stride_spec = ((dst_w + 15u) & ~15u);
                    uint32_t c_stride_spec = (((y_stride_spec / 2u) + 15u) & ~15u);
                    long spec_v_off = (long)y_stride_spec * dst_h;
                    long spec_u_off = spec_v_off + (long)c_stride_spec * ((dst_h + 1) / 2);
                    LOGI("inject_yuv: [V57 layout] dst=%ux%u stride=%u y=%p u=%p v=%p "
                         "u_off=%ld v_off=%ld du_stride=%d dv_stride=%d alloc=%ld "
                         "nominal_chroma_off=%u spec_v_off=%ld spec_u_off=%ld",
                         dst_w, dst_h, dst_stride, (void *)dst_y, (void *)dst_u, (void *)dst_v,
                         (long)(dst_u - dst_y), (long)(dst_v - dst_y),
                         du_stride, dv_stride, alloc_sz,
                         dst_stride * dst_h, spec_v_off, spec_u_off);
                }
                for (int y = 0; y < chh; y++) {
                    uint8_t *drow = semi + (size_t)y * dst_stride;
                    const uint8_t *ru = su2.data() + (size_t)y * chw;
                    const uint8_t *rv = sv2.data() + (size_t)y * chw;
                    for (int x = 0; x < chw; x++) {
                        drow[x * 2]     = ru[x];
                        drow[x * 2 + 1] = rv[x];
                    }
                }
                ok = true;
                LOGI("inject_yuv: Planar VIDEO interleave+fast-scale OK (%dx%d → %ux%u)",
                     src_w, src_h, dst_w, dst_h);
            } else {
                /* [V57 diag] periodic preview-planar layout dump — the gray
                 * PREVIEW mystery: are dst_u/dst_v (lockPlanes) where the
                 * display actually reads chroma? Compare with spec offsets. */
                static int pv_layout_count = 0;
                if ((pv_layout_count++ % 300) == 0) {
                    long pv_alloc = (dmabuf_fd >= 0) ? (long)lseek(dmabuf_fd, 0, SEEK_END) : -1;
                    uint32_t y_stride_spec = ((dst_w + 15u) & ~15u);
                    uint32_t c_stride_spec = (((y_stride_spec / 2u) + 15u) & ~15u);
                    LOGI("inject_yuv: [V57 pv-layout] dst=%ux%u y=%p u_off=%ld v_off=%ld "
                         "du=%d dv=%d spec_v_off=%ld spec_u_off=%ld alloc=%ld",
                         dst_w, dst_h, (void *)dst_y,
                         (long)(dst_u - dst_y), (long)(dst_v - dst_y),
                         du_stride, dv_stride,
                         (long)y_stride_spec * dst_h,
                         (long)y_stride_spec * dst_h + (long)c_stride_spec * ((dst_h + 1) / 2),
                         pv_alloc);
                }
                libyuv::I420Scale(eff_y, src_stride,
                                  pu, pu_stride,
                                  pv, pu_stride,
                                  src_w, src_h,
                                  dst_y, dst_stride,
                                  dst_u, du_stride,
                                  dst_v, dv_stride,
                                  (int)dst_w, (int)dst_h,
                                  filt);
                ok = true;
                LOGD("inject_yuv: Planar I420/YV12 scale OK (%dx%d → %ux%u)", src_w, src_h, dst_w, dst_h);
            }
        } else if (dst_uv && dst_uv_stride > 0) {
            const uint8_t *src_uv = eff_uv ? eff_uv : (eff_y + (size_t)src_stride * src_h);
            int src_uv_stride = src_stride;
            std::vector<uint8_t> rot_uv_buf;
            if (post_rot_i420) {
                /* [V57b] semi-planar dst needs interleaved UV: merge the
                 * rotated I420 planes (only rotated frames land here). */
                rot_uv_buf.assign((size_t)src_w * ((src_h + 1) / 2), 0);
                libyuv::MergeUVPlane(rot_u_ptr, rot_uv_stride,
                                     rot_v_ptr, rot_uv_stride,
                                     rot_uv_buf.data(), src_w,
                                     (src_w + 1) / 2, (src_h + 1) / 2);
                src_uv = rot_uv_buf.data();
                src_uv_stride = src_w;
            }
            if (is_nv21) {
                size_t uv_rows    = ((size_t)dst_h + 1) / 2;
                size_t tmp_stride = (size_t)dst_uv_stride;
                std::vector<uint8_t> tmp_uv(tmp_stride * uv_rows);

                libyuv::NV12Scale(eff_y, src_stride, src_uv, src_uv_stride,
                                  src_w, src_h,
                                  dst_y, dst_stride, tmp_uv.data(), (int)tmp_stride,
                                  (int)dst_w, (int)dst_h, filt);

                for (size_t r = 0; r < uv_rows; r++) {
                    const uint8_t *srow = tmp_uv.data() + r * tmp_stride;
                    uint8_t *drow = dst_uv + r * (size_t)dst_uv_stride;
                    size_t cols = (size_t)((dst_w + 1) / 2) * 2;
                    for (size_t c = 0; c + 1 < cols; c += 2) {
                        drow[c]     = srow[c + 1]; // V (from NV12's V)
                        drow[c + 1] = srow[c];     // U (from NV12's U)
                    }
                }
                ok = true;
                LOGD("inject_yuv: NV21 scale+swap OK (%dx%d → %ux%u)", src_w, src_h, dst_w, dst_h);
            } else {
                libyuv::NV12Scale(eff_y, src_stride, src_uv, src_uv_stride,
                                  src_w, src_h,
                                  dst_y, dst_stride, dst_uv, dst_uv_stride,
                                  (int)dst_w, (int)dst_h, filt);
                ok = true;
                LOGD("inject_yuv: NV12 scale OK (%dx%d → %ux%u)", src_w, src_h, dst_w, dst_h);
            }
        } else {
            LOGE("inject_yuv: unable to resolve UV destination pointer (planes=%u fmt=0x%x)",
                 planes.planeCount, actual_format);
        }

        // [V10 SHARP] sharpen upscaled destinations (FaceTec 1920x1080 from the
        // ~816-wide ring) so the injected selfie no longer looks soft.
        if (ok && dst_y && src_w > 0 && (int)dst_w > src_w * 7 / 5) {
            sharpen_y_plane(dst_y, dst_stride, (int)dst_w, (int)dst_h);
            LOGD("inject_yuv: V10 sharpen applied (%dx%d -> %ux%u)", src_w, src_h, dst_w, dst_h);
        }

        // V5: clear right/bottom padding that would show as striped artifact
        // If dst_stride > dst_w, the HAL may display the full stride width, leaving
        // garbage on the right edge (exactly what screenshot shows). Clear it to black.
        if (ok && dst_y && dst_stride > (int)dst_w) {
            for (uint32_t r = 0; r < dst_h; r++) {
                memset(dst_y + (size_t)r * dst_stride + dst_w, 16, (size_t)(dst_stride - dst_w));
            }
            LOGD("inject_yuv: V5 cleared Y padding stride=%d w=%u", dst_stride, dst_w);
        }
        if (ok && dst_uv && dst_uv_stride > (int)dst_w) {
            uint32_t uv_rows = (dst_h + 1) / 2;
            for (uint32_t r = 0; r < uv_rows; r++) {
                memset(dst_uv + (size_t)r * dst_uv_stride + dst_w, 128, (size_t)(dst_uv_stride - dst_w));
            }
            LOGD("inject_yuv: V5 cleared UV padding stride=%d w=%u", dst_uv_stride, dst_w);
        }
        if (ok && is_planar && dst_u && dst_v) {
            if (du_stride > (int)((dst_w + 1) / 2)) {
                for (uint32_t r = 0; r < dst_h / 2; r++) {
                    memset(dst_u + (size_t)r * du_stride + (dst_w + 1) / 2, 128, (size_t)(du_stride - (dst_w + 1) / 2));
                    memset(dst_v + (size_t)r * dv_stride + (dst_w + 1) / 2, 128, (size_t)(dv_stride - (dst_w + 1) / 2));
                }
            }
        }

        if (ok) {
            /* [V26] cache the finished destination image for replay. */
            conv_cache_store(role, dst_y, dst_u, dst_v, dst_uv, dst_stride,
                             du_stride, dv_stride, dst_uv_stride,
                             dst_w, dst_h, is_planar, is_nv21);
        }

        if (ok && dmabuf_fd >= 0) {
            struct dma_buf_sync sync = {};
            sync.flags = (uint64_t)(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
            int r = ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
            /* errno=25 (ENOTTY) is expected on Qualcomm/OPlus — the dma-buf fd
             * on these SoCs does not support this ioctl.  AHardwareBuffer_unlock
             * + the UBWC read-touch below handle cache coherency instead. */
            if (r != 0 && errno != 25 /* ENOTTY */) {
                LOGD("inject_yuv: DMA_BUF_IOCTL_SYNC(END|WRITE) fd=%d ret=%d errno=%d",
                     dmabuf_fd, r, errno);
            }
        }

        /* ── ARM cache-coherency flush ─────────────────────────────────────
         * On Qualcomm SoCs, DMA_BUF_IOCTL_SYNC is often ENOTTY (fd doesn't
         * support it).  AHardwareBuffer_unlock() should handle coherency via
         * the gralloc HAL, but some OPlus gralloc builds don't flush the CPU
         * write-back cache before signalling the release fence.
         *
         * __sync_synchronize() emits a DMB SY (full data-memory barrier) on
         * AArch64, ensuring all preceding CPU stores are visible to the
         * memory subsystem before we call unlock. */
        if (ok && dst_y) {
            __sync_synchronize();
        }

        int32_t release_fence_fd = -1;
        g_unlock(hwb, &release_fence_fd);

        /* ── Qualcomm UBWC metadata invalidation workaround ────────────────
         * Qualcomm camera buffers may be allocated in UBWC (Unified Bandwidth
         * Compression) format.  CPU writes via lockPlanes go to the LINEAR
         * memory region; the GPU reads from UBWC-compressed tiles which still
         * contain the original ISP data.
         *
         * A CPU_READ_OFTEN lock/unlock cycle AFTER a CPU_WRITE cycle forces
         * the Qualcomm gralloc to invalidate stale UBWC tile metadata, causing
         * the GPU's texture sampler to fall back to reading from the linear
         * (CPU-written) region on its next access.  Without this, the display
         * and encoder always show the original camera frame despite a
         * successful CPU write. */
        if (ok && g_lock && platform_is_qcom()) {
            void *ubwc_touch = nullptr;
            int touch_rc = g_lock(hwb,
                                  AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
                                  -1, nullptr, &ubwc_touch);
            if (touch_rc == 0 && ubwc_touch) {
                int32_t touch_fence = -1;
                g_unlock(hwb, &touch_fence);
                /* Merge the touch fence with our write fence so the consumer
                 * waits for BOTH the write AND the UBWC invalidation. */
                if (touch_fence >= 0) {
                    if (release_fence_fd >= 0) close(release_fence_fd);
                    release_fence_fd = touch_fence;
                }
                LOGD("inject_yuv: UBWC read-touch lock OK (touch_fence=%d)",
                     touch_fence);
            } else {
                LOGD("inject_yuv: UBWC read-touch lock rc=%d (non-fatal)",
                     touch_rc);
            }
        }

        if (ok && release_fence_fd >= 0) {
            *out_release_fence = release_fence_fd;
        } else if (release_fence_fd >= 0) {
            close(release_fence_fd);
        }
        return ok;

    } else {
        LOGE("inject_yuv: lockPlanes unavailable — cannot safely determine UV offset");
        return false;
    }
}

bool inject_jpeg(AHardwareBuffer *hwb, const camera3_stream_buffer_t *buf,
                        const FrameData *src, int32_t fence_fd,
                        int32_t *out_release_fence,
                        int dmabuf_fd) {
    /* [V18 photo] hwb may be null when called from the PCR hook on ROMs where
     * the BLOB AHardwareBuffer cannot be resolved (HIDL wrapper handles) —
     * the dmabuf mmap path then does the whole job. */
    if (!buf || !src || !out_release_fence) return false;
    if (!hwb && dmabuf_fd < 0) return false;
    *out_release_fence = -1;

    AHardwareBuffer_Desc desc = {};
    if (hwb) g_describe(hwb, &desc);



    size_t framework_blob_size = buf->stream ? (size_t)buf->stream->width : 0;
    if (framework_blob_size == 0) {
        LOGE("inject_jpeg: stream->width is zero, cannot determine blob size");
        return false;
    }

    int src_w = (int)src->width, src_h = (int)src->height, src_stride = (int)src->stride;
    if (src_w <= 0 || src_h <= 0 || src_stride <= 0) {
        LOGE("inject_jpeg: invalid src dims %dx%d stride=%d", src_w, src_h, src_stride);
        return false;
    }
    if (!src->y_plane || !src->uv_plane) {
        LOGE("inject_jpeg: src planes null (y=%p uv=%p)", (void *)src->y_plane, (void *)src->uv_plane);
        return false;
    }

    /* [V22 PHOTO] On this Unisoc HAL the JPEG stream reports width=4160 as
     * its "max JPEG size", but our 704x880 encode is ~46KB — every injection
     * died at "JPEG > max". The dmabuf allocation is the REAL capacity; use
     * it when it is larger. (Trailer placement below derives from this, so
     * the framework finds the blob trailer at the true end of the buffer.) */
    if (dmabuf_fd >= 0) {
        const long fd_sz = (long)lseek(dmabuf_fd, 0, SEEK_END);
        if (fd_sz > (long)framework_blob_size) {
            LOGI("inject_jpeg: [V22] blob capacity from dmabuf alloc=%ld (stream width said %zu)",
                 fd_sz, framework_blob_size);
            framework_blob_size = (size_t)fd_sz;
        }
    }
    LOGI("inject_jpeg: ENTRY framework_blob_size=%zu src=%dx%d stride=%d fence=%d (V11 snapshot trace)",
         framework_blob_size, src_w, src_h, src_stride, fence_fd);

    tjhandle tj = tjInitCompress();
    if (!tj) {
        LOGE("inject_jpeg: tjInitCompress failed");
        return false;
    }


    /* [V54 zoom] encode the zoomed sub-rectangle when the app is zoomed in —
     * zero-copy crop: shift the plane origins, keep strides, shrink w/h. */
    int enc_x = 0, enc_y = 0, enc_w = src_w, enc_h = src_h;
    const uint8_t *enc_y_plane  = src->y_plane;
    const uint8_t *enc_uv_plane = src->uv_plane;
    if (zoom_subrect(src_w, src_h, &enc_x, &enc_y, &enc_w, &enc_h)) {
        enc_y_plane  = src->y_plane  + (size_t)enc_y * src_stride + enc_x;
        enc_uv_plane = src->uv_plane + (size_t)(enc_y / 2) * src_stride + enc_x;
        LOGI("inject_jpeg: [V54] zoom applied — encoding [%d,%d %dx%d] of %dx%d",
             enc_x, enc_y, enc_w, enc_h, src_w, src_h);
    }

    int uv_w = (enc_w + 1) / 2;
    int uv_h = (enc_h + 1) / 2;
    std::vector<uint8_t> u_plane((size_t)uv_w * uv_h);
    std::vector<uint8_t> v_plane((size_t)uv_w * uv_h);

    for (int y = 0; y < uv_h; y++) {
        for (int x = 0; x < uv_w; x++) {
            u_plane[y * uv_w + x] = enc_uv_plane[y * src_stride + x * 2];
            v_plane[y * uv_w + x] = enc_uv_plane[y * src_stride + x * 2 + 1];
        }
    }

    const unsigned char *planes_in[3] = {
        enc_y_plane,
        u_plane.data(),
        v_plane.data()
    };
    int strides_in[3] = { src_stride, uv_w, uv_w };

    unsigned char *jpeg_buf = nullptr;
    unsigned long  jpeg_sz  = 0;

    int rc = tjCompressFromYUVPlanes(tj, planes_in, enc_w, strides_in, enc_h,
                                     TJSAMP_420, &jpeg_buf, &jpeg_sz, 85, TJFLAG_FASTDCT);
    tjDestroy(tj);

    if (rc != 0 || !jpeg_buf || jpeg_sz == 0) {
        LOGE("inject_jpeg: tjCompress FAILED: %s", tjGetErrorStr2(nullptr));
        if (jpeg_buf) tjFree(jpeg_buf);
        return false;
    }
    LOGD("inject_jpeg: JPEG encoded %lu bytes", jpeg_sz);

    void *vaddr = nullptr;

    int err = hwb ? g_lock(hwb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, fence_fd, nullptr, &vaddr) : -1;
    bool mm_jpeg = false;
    void *mm_jpeg_ptr = nullptr;
    size_t mm_jpeg_sz = 0;
    if ((err != 0 || !vaddr) && dmabuf_fd >= 0) {
        /* [V17 photo fix] MTK A14: AHB lock of the BLOB fails (HIDL transport
         * wrapper handle — resolve_ahwb EINVAL). mmap the dmabuf directly, the
         * same way the YUV mmap fallback does. */
        mm_jpeg_sz = (framework_blob_size + 4095) & ~(size_t)4095;
        mm_jpeg_ptr = mmap(nullptr, mm_jpeg_sz, PROT_READ | PROT_WRITE,
                           MAP_SHARED, dmabuf_fd, 0);
        if (mm_jpeg_ptr != MAP_FAILED) {
            struct dma_buf_sync js = {};
            js.flags = (uint64_t)(DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE);
            ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &js);
            vaddr = mm_jpeg_ptr;
            mm_jpeg = true;
            err = 0;   /* [V22 PHOTO] fallback succeeded — stop reporting failure */
            LOGI("inject_jpeg: [V17] AHB lock failed — dmabuf mmap fallback OK (%zu bytes)",
                 mm_jpeg_sz);
        } else {
            LOGE("inject_jpeg: dmabuf mmap fallback FAILED: %s", strerror(errno));
            mm_jpeg_ptr = nullptr;
        }
    }
    if (err != 0 || !vaddr) {
        LOGE("inject_jpeg: BLOB buffer lock FAILED err=%d", err);
        tjFree(jpeg_buf);
        return false;
    }

    const size_t trailer_size = sizeof(struct camera3_jpeg_blob);
    if (framework_blob_size < trailer_size) {
        LOGE("inject_jpeg: blob too small for trailer (%zu < %zu)", framework_blob_size, trailer_size);
        g_unlock(hwb, nullptr);
        tjFree(jpeg_buf);
        return false;
    }

    size_t max_jpeg = framework_blob_size - trailer_size;
    if (jpeg_sz > max_jpeg) {
        LOGW("inject_jpeg: JPEG %lu bytes > max %zu — skipping", jpeg_sz, max_jpeg);
        g_unlock(hwb, nullptr);
        tjFree(jpeg_buf);
        return false;
    }

    memcpy(vaddr, jpeg_buf, jpeg_sz);


    struct camera3_jpeg_blob blob = {};
    blob.jpeg_blob_id = CAMERA3_JPEG_BLOB_ID;
    blob.reserved     = 0;
    blob.jpeg_size    = (uint32_t)jpeg_sz;

    uint8_t *trailer_ptr = (uint8_t *)vaddr + max_jpeg;
    memcpy(trailer_ptr, &blob, sizeof(blob));

    if (dmabuf_fd >= 0) {
        struct dma_buf_sync sync = {};
        sync.flags = (uint64_t)(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        int r = ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
        if (r != 0 && errno != 25 /* ENOTTY — expected on Qualcomm/OPlus */) {
            LOGD("inject_jpeg: DMA_BUF_IOCTL_SYNC(END|WRITE) fd=%d ret=%d errno=%d",
                 dmabuf_fd, r, errno);
        }
    }

    /* ARM cache-coherency flush — same as inject_yuv path */
    __sync_synchronize();

    int32_t release_fence_fd = -1;
    if (mm_jpeg) {
        struct dma_buf_sync je = {};
        je.flags = (uint64_t)(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
        ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &je);
        munmap(mm_jpeg_ptr, mm_jpeg_sz);
    } else {
        g_unlock(hwb, &release_fence_fd);
    }

    /* Qualcomm UBWC metadata invalidation workaround — same as inject_yuv */
    if (g_lock && hwb && platform_is_qcom()) {
        void *ubwc_touch = nullptr;
        int touch_rc = g_lock(hwb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,
                              -1, nullptr, &ubwc_touch);
        if (touch_rc == 0 && ubwc_touch) {
            int32_t touch_fence = -1;
            g_unlock(hwb, &touch_fence);
            if (touch_fence >= 0) {
                if (release_fence_fd >= 0) close(release_fence_fd);
                release_fence_fd = touch_fence;
            }
            LOGD("inject_jpeg: UBWC read-touch lock OK (touch_fence=%d)", touch_fence);
        }
    }

    if (release_fence_fd >= 0) {
        *out_release_fence = release_fence_fd;
    }

    tjFree(jpeg_buf);

    LOGI("inject_jpeg: OK — JPEG %lu bytes written to framework_blob_size=%zu (V11 snapshot trace)", jpeg_sz, framework_blob_size);
    return true;
}

bool frame_inject_one(const camera3_stream_buffer_t *buf,
                       StreamRole                     role,
                       const FrameData               *src) {
    if (!buf || !src || !g_libandroid) {
        LOGE("frame_inject_one: null param — buf=%p src=%p libandroid=%p",
             (void *)buf, (void *)src, g_libandroid);
        return false;
    }
    if (buf->status != CAMERA3_BUFFER_STATUS_OK) {
        LOGW("frame_inject_one: buf->status=%d (not OK), skipping", buf->status);
        return false;
    }

    uint32_t dst_w = buf->stream ? buf->stream->width  : 0;
    uint32_t dst_h = buf->stream ? buf->stream->height : 0;
    int      fmt   = buf->stream ? buf->stream->format : HAL_PIXEL_FORMAT_BLOB;

    /* [V20 photo fix] BLOB (JPEG) buffers carry HIDL transport wrapper handles
     * on OPlus/MTK A14 — resolve_ahwb ALWAYS fails there (EINVAL x3 profiles)
     * and aborted the whole injection before inject_jpeg ran, even though
     * inject_jpeg only needs the dmabuf fd (null hwb is supported since the
     * V18 PCR path). Fast-path BLOB straight to inject_jpeg. */
    if (fmt == HAL_PIXEL_FORMAT_BLOB) {
        uintptr_t nh_raw_b = (uintptr_t)(*buf->buffer);
        if (nh_raw_b >> 56) nh_raw_b &= 0x00FFFFFFFFFFFFFFULL;
        const native_handle_t *nhb = (const native_handle_t *)nh_raw_b;
        /* [V22 PHOTO] Unisoc A13 (13.unisoc.1): data[0] of the BLOB handle is
         * an 8KB METADATA blob — mmapping it "succeeded" then the write was
         * abandoned (saved photo stayed real). Scan every fd in the handle and
         * take the one with the LARGEST allocation: that is the JPEG data
         * region (MBs), regardless of vendor fd ordering. */
        int blob_fd = -1;
        long blob_alloc = -1;
        for (int fi = 0; nhb && fi < nhb->numFds; fi++) {
            const int cand = nhb->data[fi];
            if (cand < 0) continue;
            const long sz = (long)lseek(cand, 0, SEEK_END);
            if (sz > blob_alloc) { blob_alloc = sz; blob_fd = cand; }
        }
        if (blob_fd < 0) {
            LOGE("frame_inject_one: BLOB has no dmabuf fd — cannot inject JPEG");
            return false;
        }
        LOGI("frame_inject_one: [V22] BLOB handle fds=%d -> data fd=%d alloc=%ld",
             nhb ? nhb->numFds : 0, blob_fd, blob_alloc);
        camera3_stream_buffer_t *mb = const_cast<camera3_stream_buffer_t *>(buf);
        int32_t in_fence = mb->release_fence;
        mb->release_fence = -1;
        int32_t out_fence = -1;
        LOGI("frame_inject_one: BLOB fast-path → inject_jpeg fd=%d %ux%u fence=%d (V20)",
             blob_fd, dst_w, dst_h, in_fence);
        bool jok = inject_jpeg(nullptr, buf, src, in_fence, &out_fence, blob_fd);
        if (out_fence >= 0) mb->release_fence = out_fence;
        return jok;
    }

    AHardwareBuffer *hwb = resolve_ahwb(buf);
    if (!hwb) {
        return false;
    }

    if (dst_w == 0 || dst_h == 0) {
        LOGE("frame_inject_one: stream has zero dimensions (%ux%u)", dst_w, dst_h);
        release_ahwb(hwb, buf);
        return false;
    }

    LOGD("frame_inject_one: buf=%p role=%d fmt=0x%x %ux%u acquire_fence=%d release_fence=%d",
         (void *)buf, (int)role, fmt, dst_w, dst_h,
         buf->acquire_fence, buf->release_fence);


    if (buf->acquire_fence >= 0) {
        LOGW("frame_inject_one: acquire_fence=%d (expected -1), ignoring", buf->acquire_fence);
    }



    camera3_stream_buffer_t *mutable_buf = const_cast<camera3_stream_buffer_t *>(buf);
    int32_t incoming_fence = mutable_buf->release_fence;
    mutable_buf->release_fence = -1;

    uintptr_t nh_raw = (uintptr_t)(*buf->buffer);
    if (nh_raw >> 56) nh_raw &= 0x00FFFFFFFFFFFFFFULL;
    const native_handle_t *nh_for_fd = (const native_handle_t *)nh_raw;
    int dmabuf_fd = (nh_for_fd && nh_for_fd->numFds > 0) ? nh_for_fd->data[0] : -1;

    int32_t downstream_fence = -1;
    bool ok = false;
    switch (fmt) {
        case HAL_PIXEL_FORMAT_BLOB:
            LOGI("frame_inject_one: injecting JPEG (blob) stream %ux%u (V11 snapshot trace)", dst_w, dst_h);
            ok = inject_jpeg(hwb, buf, src, incoming_fence, &downstream_fence, dmabuf_fd);
            break;
        case HAL_PIXEL_FORMAT_RAW16:
            LOGD("frame_inject_one: RAW16 stream — skipped per policy");
            ok = false;
            break;
        case HAL_PIXEL_FORMAT_YCBCR_P010:
            LOGW("frame_inject_one: P010/HDR stream — not supported, skipping");
            ok = false;
            break;
        case HAL_PIXEL_FORMAT_YCBCR_420_888:
        case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
        case HAL_PIXEL_FORMAT_YCrCb_420_SP:
        default:
            LOGD("frame_inject_one: injecting YUV stream role=%d fmt=0x%x %ux%u",
                 (int)role, fmt, dst_w, dst_h);
            ok = inject_yuv(hwb, dst_w, dst_h, src, incoming_fence, &downstream_fence, dmabuf_fd, role);
            break;
    }



    if (ok) {
        mutable_buf->release_fence = downstream_fence;
        LOGD("frame_inject_one: SUCCESS fmt=0x%x %ux%u release_fence=%d",
             fmt, dst_w, dst_h, downstream_fence);
    } else {
        if (downstream_fence >= 0) {
            close(downstream_fence);
        }
        LOGW("frame_inject_one: FAILED fmt=0x%x %ux%u role=%d", fmt, dst_w, dst_h, (int)role);
    }

    release_ahwb(hwb, buf);
    return ok;
}



#include "stream_map.h"
#include "include/camera3_compat.h"

#include <android/log.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

#ifndef GRALLOC_USAGE_HW_CAMERA_WRITE
#define GRALLOC_USAGE_HW_CAMERA_WRITE  0x00020000
#endif
#ifndef GRALLOC_USAGE_HW_CAMERA_READ
#define GRALLOC_USAGE_HW_CAMERA_READ   0x00040000
#endif
#ifndef GRALLOC_USAGE_HW_VIDEO_ENCODER
#define GRALLOC_USAGE_HW_VIDEO_ENCODER 0x00010000
#endif
#ifndef GRALLOC_USAGE_HW_TEXTURE
#define GRALLOC_USAGE_HW_TEXTURE       0x00000100
#endif
#ifndef GRALLOC_USAGE_HW_COMPOSER
#define GRALLOC_USAGE_HW_COMPOSER      0x00000800
#endif
#ifndef GRALLOC_USAGE_SW_WRITE_OFTEN
#define GRALLOC_USAGE_SW_WRITE_OFTEN   0x00000030
#endif
#ifndef GRALLOC_USAGE_SW_READ_OFTEN
#define GRALLOC_USAGE_SW_READ_OFTEN    0x00000003
#endif
#ifndef GRALLOC_USAGE_PROTECTED
#define GRALLOC_USAGE_PROTECTED        0x00004000
#endif

#define TAG "amkush/stream_map"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* HAL_PIXEL_FORMAT_YV12 = YCrCb 4:2:0 planar ("YV12" FourCC).
 * The HAL is allowed to resolve IMPLEMENTATION_DEFINED streams to this
 * format after configureStreams returns.  Without explicit handling, the
 * stream is misclassified as UNKNOWN and injection is silently skipped. */
#ifndef HAL_PIXEL_FORMAT_YV12
#define HAL_PIXEL_FORMAT_YV12  0x32315659
#endif

#define MAX_STREAMS 32

typedef struct {
    const camera3_stream_t *ptr;
    StreamRole              role;
} StreamEntry;

static StreamEntry      g_map[MAX_STREAMS];
static uint32_t         g_count = 0;
static pthread_rwlock_t g_lock  = PTHREAD_RWLOCK_INITIALIZER;

static StreamRole classify_stream(const camera3_stream_t *s) {
    if (!s) return STREAM_ROLE_UNKNOWN;
























    /* Bug2 fix: PROTECTED streams are NOT excluded from classification.
     * The setUsage hook (my_setusage_proxy) strips GRALLOC_USAGE_PROTECTED at
     * buffer allocation time, so the gralloc buffer is actually writable.
     * Excluding PROTECTED here caused zero injection on the video-selfie
     * recording session (second camera open) where streams carry the flag.
     * If a buffer is genuinely protected, AHardwareBuffer_lockPlanes will
     * fail and frame_inject_one() returns false — a safe, silent fallback. */

    int      fmt   = s->format;
    uint32_t usage = s->usage;
    uint32_t w     = s->width;

    if (fmt == HAL_PIXEL_FORMAT_RAW16)       return STREAM_ROLE_RAW;
    if (fmt == HAL_PIXEL_FORMAT_YCBCR_P010)  return STREAM_ROLE_HDR;

    /* YV12 (0x32315659): planar YCrCb 4:2:0. The HAL often resolves
     * IMPLEMENTATION_DEFINED to this after configureStreams returns.
     * Treat the same as YUV_420_888 — injectable at any size. */
    if (fmt == HAL_PIXEL_FORMAT_YV12) {
        return (w >= 1280) ? STREAM_ROLE_VIDEO : STREAM_ROLE_PREVIEW;
    }




    if (fmt == HAL_PIXEL_FORMAT_BLOB) {
        return (w > 100) ? STREAM_ROLE_SNAPSHOT : STREAM_ROLE_ML_THUMB;
    }





    if (fmt == HAL_PIXEL_FORMAT_YCBCR_420_888) {
        return (w <= 64) ? STREAM_ROLE_YUV_ANALYSIS : STREAM_ROLE_PREVIEW;
    }

    /* [V96 NV21] YCrCb_420_SP / NV21 (0x11). The MediaTek HAL on the TECNO CE9
     * (log11) hands the app an NV21 stream alongside the YUV_420_888 one; with
     * no case here those buffers classified UNKNOWN and were skipped — that is
     * exactly the skip_role=553 / frames_inject_fail=553 in the log — even
     * though frame_inject_one() already writes 0x11 buffers (its switch lists
     * HAL_PIXEL_FORMAT_YCrCb_420_SP). Classify them as injectable. */
    if (fmt == HAL_PIXEL_FORMAT_YCrCb_420_SP) {
        return (w <= 64) ? STREAM_ROLE_YUV_ANALYSIS : STREAM_ROLE_PREVIEW;
    }

    if (fmt == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {

        if (usage == 0 && s->data_space != 0) {

            static const int DS_STANDARD_MASK = 0x3F;
            static const int DS_BT709      = 1;
            static const int DS_BT601_625  = 2;
            static const int DS_BT601_525  = 4;
            static const int DS_BT2020     = 6;
            int ds = (int)s->data_space;

            if (ds == 0x101 || ds == 0x100 || ds == 0x08C20000 ||
                (ds & 0xFF) == 0x21) {
                return STREAM_ROLE_SNAPSHOT;
            }
            int std = (ds >> 16) & DS_STANDARD_MASK;
            if (std == DS_BT601_625 || std == DS_BT601_525 ||
                std == DS_BT709     || std == DS_BT2020) {

                return (w >= 1280) ? STREAM_ROLE_VIDEO : STREAM_ROLE_PREVIEW;
            }
        }

        if (usage & GRALLOC_USAGE_HW_VIDEO_ENCODER) {
            return STREAM_ROLE_VIDEO;
        }
        if (usage & GRALLOC_USAGE_HW_TEXTURE) {
            return STREAM_ROLE_PREVIEW;
        }

        return (w >= 1280) ? STREAM_ROLE_VIDEO : STREAM_ROLE_PREVIEW;
    }

    return STREAM_ROLE_UNKNOWN;
}

void stream_map_rebuild(camera3_stream_configuration_t *cfg) {
    if (!cfg) return;

    pthread_rwlock_wrlock(&g_lock);

    memset(g_map, 0, sizeof(g_map));
    g_count = 0;

    for (uint32_t i = 0; i < cfg->num_streams && i < MAX_STREAMS; i++) {
        camera3_stream_t *s = cfg->streams[i];
        if (!s) continue;


        if (s->width == 0 || s->height == 0) {
            LOGW("stream[%u] has invalid dimensions %ux%u, skipping", i, s->width, s->height);
            continue;
        }


        uint32_t captured_fmt   = s->format;
        uint32_t captured_w     = s->width;
        uint32_t captured_h     = s->height;
        uint32_t captured_usage = s->usage;



        if (captured_usage & GRALLOC_USAGE_PROTECTED) {
            LOGW("stream[%u] has GRALLOC_USAGE_PROTECTED — will attempt injection "
                 "(Bug2 fix: classify_stream no longer excludes PROTECTED). "
                 "my_setusage_proxy strips PROTECTED at alloc; lockPlanes is the safety net.",
                 i);
        }

        StreamRole role = classify_stream(s);

        g_map[g_count].ptr  = s;
        g_map[g_count].role = role;
        g_count++;

        LOGI("stream_map[%u] ptr=%p fmt=0x%02x %ux%u usage=0x%x → role=%d%s",
             i, (void *)s, captured_fmt, captured_w, captured_h, captured_usage, (int)role,
             (captured_usage & GRALLOC_USAGE_PROTECTED) ? " [PROTECTED/will-attempt-inject]" : "");
    }

    pthread_rwlock_unlock(&g_lock);
}

void stream_map_clear(void) {
    pthread_rwlock_wrlock(&g_lock);
    memset(g_map, 0, sizeof(g_map));
    g_count = 0;
    pthread_rwlock_unlock(&g_lock);
}

StreamRole stream_map_get_role(const camera3_stream_t *stream) {
    if (!stream) return STREAM_ROLE_UNKNOWN;

    pthread_rwlock_rdlock(&g_lock);
    StreamRole role = STREAM_ROLE_UNKNOWN;
    bool found = false;
    for (uint32_t i = 0; i < g_count; i++) {
        if (g_map[i].ptr == stream) {
            role = g_map[i].role;
            found = true;
            break;
        }
    }
    pthread_rwlock_unlock(&g_lock);

    /* Fallback: if the stream pointer was not seen in configureStreams (e.g.
     * HAL-internal streams, or the target app was re-opened before the map
     * was rebuilt), classify it directly from its format/usage fields.
     * This prevents streams from permanently staying UNKNOWN → skipped. */
    if (!found) {
        role = classify_stream(stream);
        if (role != STREAM_ROLE_UNKNOWN) {
            LOGW("stream_map_get_role: ptr=%p not in map — direct classify → role=%d "
                 "fmt=0x%x %ux%u", (void *)stream, (int)role,
                 stream->format, stream->width, stream->height);
        }
    }
    return role;
}

bool stream_map_should_inject(StreamRole role, int buffer_status) {
    if (buffer_status != CAMERA3_BUFFER_STATUS_OK) return false;

    /* Hard exclusions — cannot or must not inject into these. */
    if (role == STREAM_ROLE_UNKNOWN) return false;  // can't determine format
    if (role == STREAM_ROLE_RAW)     return false;  // RAW16 — no YUV→RAW converter

    /* Everything else — PREVIEW, VIDEO, SNAPSHOT, YUV_ANALYSIS, ML_THUMB, HDR —
     * should receive injected frames.  The per-format logic in frame_inject_one()
     * will skip unsupported pixel formats (e.g. P010/HDR) and log a warning; we
     * must not silently drop streams here because:
     *   • YUV_ANALYSIS (ImageReader-backed, usage=SW_READ)  is often the ONLY
     *     stream configured in a capture session on OPlus — skipping it means
     *     zero injection even though lockPlanes works correctly on it.
     *   • ML_THUMB (tiny YUV slices, w≤64) can be faked cheaply.
     *   • HDR/P010: frame_inject_one already returns false+LOGW, not a hard crash. */
    return true;
}

int stream_modify_usage_for_injection(camera3_stream_configuration_t *cfg) {
    if (!cfg) return -1;

    int modified_count = 0;
    int protected_skip_count = 0;

    for (uint32_t i = 0; i < cfg->num_streams && i < MAX_STREAMS; i++) {
        camera3_stream_t *s = cfg->streams[i];
        if (!s || s->width == 0 || s->height == 0) continue;

        uint32_t old_usage = s->usage;






























        uint32_t new_usage = old_usage;

        /* Bug2 fix: strip GRALLOC_USAGE_PROTECTED so the gralloc driver
         * allocates a CPU-accessible buffer.  The original guard ("leave
         * untouched") cited a MediaTek mt6765 SIGSEGV, but that crash
         * occurred on a specific SoC that is not the target device here.
         * On Qualcomm (OPlus), my_setusage_proxy already strips PROTECTED
         * at buffer allocation; doing it here at stream-configure time as
         * well ensures HAL-side path choices also use the non-protected path. */
        if (old_usage & GRALLOC_USAGE_PROTECTED) {
            new_usage &= ~(uint32_t)GRALLOC_USAGE_PROTECTED;
            LOGI("stream[%u] usage=0x%08x has PROTECTED — stripping for injection. fmt=0x%x %ux%u",
                 i, old_usage, s->format, s->width, s->height);
            protected_skip_count++;
        }


















        new_usage |= GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_OFTEN;

        if (new_usage == old_usage) continue;

        s->usage = new_usage;

        LOGI("stream[%u] usage: 0x%08x → 0x%08x "
             "(add SW_WRITE+READ_OFTEN) fmt=0x%x %ux%u",
             i, old_usage, new_usage,
             s->format, s->width, s->height);
        modified_count++;
    }

    if (protected_skip_count > 0) {
        LOGW("stream_modify: stripped PROTECTED from %d stream(s) for injection.",
             protected_skip_count);
    }

    return modified_count;
}



#include "hook_proxy.h"
#include "symbol_resolver.h"
#include "stream_map.h"
#include "frame_source.h"
#include "frame_inject.h"
#include "crash_guard.h"
#include "include/camera3_compat.h"

#include <shadowhook.h>
#include <android/log.h>
#include <atomic>
#include <dlfcn.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <string.h>

#define TAG "amkush/hook_proxy"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

#define MAX_PCR_VARIANTS 12
#define MAX_ROB_VARIANTS 4
#define MAX_CS_VARIANTS  6

static void *g_pcr_stubs[MAX_PCR_VARIANTS];
static int   g_pcr_variants[MAX_PCR_VARIANTS];
static int   g_pcr_stub_count = 0;

/* Metadata-only observer hook for the OUTPUTUTILS processCaptureResult variant.
 * Separate from the injection PCR hooks because it only reads+logs metadata. */
static void *g_pcr_meta_stub = nullptr;
static bool  g_pcr_meta_active = false;
static std::atomic<uint32_t> g_pcr_meta_fires{0};

static void *g_rob_stubs[MAX_ROB_VARIANTS];
static int   g_rob_stub_count = 0;

static void *g_cs_stubs[MAX_CS_VARIANTS];
static int   g_cs_variants[MAX_CS_VARIANTS];
static int   g_cs_stub_count = 0;

static std::atomic<bool> g_enabled{true};
static std::atomic<int>  g_init_done{0};

static std::atomic<uint32_t> g_pcr_fire_count[MAX_PCR_VARIANTS];
static std::atomic<uint32_t> g_pcr_inject_attempt[MAX_PCR_VARIANTS];
static std::atomic<uint32_t> g_rob_fire_count[MAX_ROB_VARIANTS];
static std::atomic<uint32_t> g_rob_inject_attempt[MAX_ROB_VARIANTS];

static std::atomic<uint32_t> g_skip_disabled{0};
static std::atomic<uint32_t> g_skip_no_source{0};
static std::atomic<uint32_t> g_skip_no_frame{0};
static std::atomic<uint32_t> g_skip_zero_bufs{0};
static std::atomic<uint32_t> g_inject_ok{0};
static std::atomic<uint32_t> g_inject_fail{0};
static std::atomic<uint32_t> g_buf_ok{0};
static std::atomic<uint32_t> g_buf_skip_role{0};
static std::atomic<uint32_t> g_buf_fail{0};

static std::atomic<uint32_t> g_cs_fire_count[MAX_CS_VARIANTS];

static void *g_setusage_stub         = nullptr;
static void *g_getendpointusage_stub = nullptr;
static std::atomic<uint32_t> g_setusage_fires{0};
static std::atomic<uint32_t> g_getendpointusage_fires{0};

#define MAX_RTRN_VARIANTS 4
static void *g_rtrn_stubs[MAX_RTRN_VARIANTS];
static int   g_rtrn_stub_count = 0;
static std::atomic<uint32_t> g_rtrn_fire_count[MAX_RTRN_VARIANTS];
static std::atomic<uint32_t> g_rtrn_inject_attempt[MAX_RTRN_VARIANTS];

#define MAX_RTRN_LOCKED_VARIANTS 4
static void *g_rtrn_locked_stubs[MAX_RTRN_LOCKED_VARIANTS];
static int   g_rtrn_locked_stub_count = 0;
static std::atomic<uint32_t> g_rtrn_locked_fire_count[MAX_RTRN_LOCKED_VARIANTS];
static std::atomic<uint32_t> g_rtrn_locked_inject_attempt[MAX_RTRN_LOCKED_VARIANTS];

#define DIAG_EVERY_N_CALLS 200
static std::atomic<uint32_t> g_total_pcr_calls{0};
static std::atomic<uint32_t> g_total_rob_calls{0};

static void dump_diagnostics(void) {
    LOGI("═══════════════ AMKUSH INJECTION DIAGNOSTICS ═══════════════");
    LOGI("Injection enabled: %s", g_enabled.load() ? "YES" : "NO");
    bool _fs_init  = frame_source_initialized();
    bool _fs_ready = frame_source_ready();
    LOGI("Frame source: %s%s",
         _fs_init  ? "initialized (IPC connected)"
                   : "NOT initialized (IPC not connected)",
         _fs_init  ? (_fs_ready ? ", ring has new frame waiting"
                                : ", holding last frame (ring caught up with camera rate)")
                   : "");
    LOGI("─── PCR hook fire counts ───");
    for (int i = 0; i < g_pcr_stub_count; i++) {
        uint32_t fires   = g_pcr_fire_count[i].load();
        uint32_t injects = g_pcr_inject_attempt[i].load();
        LOGI("  PCR[%d] variant=%d fires=%u  inject_attempts=%u  %s",
             i, g_pcr_variants[i], fires, injects,
             injects > 0 ? "← ACTIVE (carries buffers)" : "");
    }
    LOGI("─── ROB hook fire counts (returnOutputBuffers — primary injection) ───");
    for (int i = 0; i < g_rob_stub_count; i++) {
        uint32_t fires   = g_rob_fire_count[i].load();
        uint32_t injects = g_rob_inject_attempt[i].load();
        LOGI("  ROB[%d] fires=%u  inject_attempts=%u  %s",
             i, fires, injects,
             injects > 0 ? "← ACTIVE (carries resolved handles)" : "");
    }
    LOGI("─── CS hook fire counts ───");
    for (int i = 0; i < g_cs_stub_count; i++) {
        LOGI("  CS[%d] variant=%d fires=%u",
             i, g_cs_variants[i], g_cs_fire_count[i].load());
    }
    LOGI("─── Injection pipeline ───");
    LOGI("  skip_disabled=%u  skip_no_source=%u  skip_no_frame=%u  skip_zero_bufs=%u",
         g_skip_disabled.load(), g_skip_no_source.load(),
         g_skip_no_frame.load(), g_skip_zero_bufs.load());
    LOGI("  frames_injected_ok=%u  frames_inject_fail=%u",
         g_inject_ok.load(), g_inject_fail.load());
    LOGI("  buffers: ok=%u  skip_role=%u  fail=%u",
         g_buf_ok.load(), g_buf_skip_role.load(), g_buf_fail.load());
    LOGI("─── Usage-strip hooks (BUG-09 fix: PROTECTED → CPU-writable) ───");
    LOGI("  Camera3Stream::setUsage fires=%u  (producer-side: adds SW_WRITE, strips PROTECTED)",
         g_setusage_fires.load());
    LOGI("  Camera3OutputStream::getEndpointUsage fires=%u  (consumer-side: strips PROTECTED)",
         g_getendpointusage_fires.load());
    if (g_setusage_stub == nullptr)
        LOGW("  ⚠ setUsage hook NOT installed — producer PROTECTED strip inactive");
    if (g_getendpointusage_stub == nullptr)
        LOGW("  ⚠ getEndpointUsage hook NOT installed — consumer PROTECTED will survive allocation");
    LOGI("─── Camera3OutputStream::returnBuffer hooks (OPlus RTRN — 8-param) ───");
    if (g_rtrn_stub_count == 0) {
        LOGW("  ⚠ returnBuffer hook NOT installed (symbol absent/inlined on this ROM)");
    }
    for (int i = 0; i < g_rtrn_stub_count; i++) {
        uint32_t fires   = g_rtrn_fire_count[i].load();
        uint32_t injects = g_rtrn_inject_attempt[i].load();
        LOGI("  RTRN[%d] fires=%u  inject_attempts=%u  %s",
             i, fires, injects,
             injects > 0 ? "← ACTIVE (frames injected per-buffer)" :
             fires  > 0 ? "← firing but no inject attempts yet" :
                          "← hook installed but not yet called");
    }
    LOGI("─── Camera3OutputStream::returnBufferLocked hooks (OPlus RTRN_LOCKED — 6-param, fence-wait) ───");
    if (g_rtrn_locked_stub_count == 0) {
        LOGW("  ⚠ returnBufferLocked hook NOT installed — no injection point on this device");
        if (g_rtrn_stub_count == 0 && g_rob_stub_count == 0) {
            LOGW("  ⚠ CRITICAL: ROB=0 RTRN=0 RTRN_LOCKED=0 — frames will NEVER be injected");
        }
    }
    for (int i = 0; i < g_rtrn_locked_stub_count; i++) {
        uint32_t fires   = g_rtrn_locked_fire_count[i].load();
        uint32_t injects = g_rtrn_locked_inject_attempt[i].load();
        LOGI("  RTRN_LOCKED[%d] fires=%u  inject_attempts=%u  %s",
             i, fires, injects,
             injects > 0 ? "← ACTIVE (fence-waited + injected per-buffer)" :
             fires  > 0 ? "← firing but no inject attempts yet" :
                          "← hook installed but not yet called");
    }
    LOGI("═══════════════════════════════════════════════════════════");
}

/* ── Original metadata logger (metadata-only observer) ─────────────────────
 * The camera's per-frame metadata (AF/AE/face/liveness/etc.) is carried on
 * processCaptureResult's `result` (camera_metadata_t*). On this device injection
 * goes through ROB (buffers only), so we hook the OUTPUTUTILS variant of
 * processCaptureResult in a metadata-ONLY mode: we read + log the ORIGINAL
 * metadata (tag names + values) and call through WITHOUT injecting any buffer.
 *
 * We resolve the metadata API dynamically (dlopen/dlsym) so we don't need to
 * link against libcameraservice's private metadata symbols. If resolution
 * fails, we fall back to logging the raw pointer/count only. */
typedef int    (*meta_entry_count_fn)(const camera_metadata_t*);
typedef int    (*meta_tag_count_fn)(const camera_metadata_t*);
typedef const char* (*meta_tag_name_fn)(uint32_t tag);
typedef uint32_t (*meta_tag_type_fn)(uint32_t tag);
typedef int    (*meta_get_entry_fn)(const camera_metadata_t*, size_t, void*);

/* camera_metadata_entry layout (camera_metadata.h, stable ABI). */
typedef struct camera_metadata_entry_t {
    uint32_t index;
    uint32_t type;
    uint32_t tag;
    uint32_t count;
    union {
        uint8_t  *u8;
        int32_t  *i32;
        float    *f;
        int64_t  *i64;
        double   *d;
    } data;
} camera_metadata_entry_t;

static void log_original_metadata(const camera3_capture_result_t *result, int hook_idx,
                                  uint32_t frame_number) {
    if (!result || !result->result) {
        LOGD("PCR[hook%d] frm#%u — no metadata (result=%p)", hook_idx, frame_number,
             result ? (void*)result->result : nullptr);
        return;
    }

    static void *g_libcam_meta = nullptr;
    static meta_entry_count_fn f_entry_count = nullptr;
    static meta_tag_count_fn   f_tag_count   = nullptr;
    static meta_tag_name_fn    f_tag_name    = nullptr;
    static meta_tag_type_fn    f_tag_type    = nullptr;
    static meta_get_entry_fn   f_get_entry   = nullptr;

    if (!g_libcam_meta) {
        // libcameraservice.so is loaded in this process (cameraserver) — it links
        // libcamera_metadata or exports the metadata helpers directly.
        g_libcam_meta = dlopen("libcamera_metadata.so", RTLD_NOW | RTLD_NOLOAD);
        if (!g_libcam_meta) g_libcam_meta = dlopen("libcameraservice.so", RTLD_NOW | RTLD_NOLOAD);
        if (!g_libcam_meta) {
            LOGW("meta: dlopen(libcamera_metadata/libcameraservice) failed — dlerror=%s", dlerror());
            return;
        }
        f_entry_count = (meta_entry_count_fn)dlsym(g_libcam_meta, "get_camera_metadata_entry_count");
        f_tag_count   = (meta_tag_count_fn)  dlsym(g_libcam_meta, "get_camera_metadata_tag_count");
        f_tag_name    = (meta_tag_name_fn)   dlsym(g_libcam_meta, "get_camera_metadata_tag_name");
        f_tag_type    = (meta_tag_type_fn)   dlsym(g_libcam_meta, "get_camera_metadata_tag_type");
        f_get_entry   = (meta_get_entry_fn)  dlsym(g_libcam_meta, "get_camera_metadata_entry");
        LOGI("meta: resolved entry_count=%p tag_count=%p tag_name=%p tag_type=%p get_entry=%p",
             (void*)f_entry_count, (void*)f_tag_count, (void*)f_tag_name,
             (void*)f_tag_type, (void*)f_get_entry);
    }

    if (!f_entry_count) {
        LOGD("PCR[hook%d] frm#%u — metadata API unavailable; raw result=%p",
             hook_idx, frame_number, (void*)result->result);
        return;
    }

    int n_entry = f_entry_count(result->result);
    int n_tag   = f_tag_count ? f_tag_count(result->result) : 0;
    LOGI("PCR[hook%d] frm#%u — ORIGINAL metadata: %d tag(s) / %d entry(ies) @%p",
         hook_idx, frame_number, n_tag, n_entry, (void*)result->result);

    /* Dump the metadata entries: tag name, type, and up to a few values.
     * Throttled by the caller (only on early / periodic frames). */
    if (f_get_entry && f_tag_name && n_entry > 0 && n_entry <= 512) {
        for (int i = 0; i < n_entry && i < 128; i++) {
            camera_metadata_entry_t e;
            memset(&e, 0, sizeof(e));
            if (f_get_entry(result->result, (size_t)i, &e) != 0) continue;
            const char *name = f_tag_name(e.tag);
            uint32_t type = e.type;
            int cnt = (int)e.count;
            // Log the tag name + type + count and the first 2 values by type.
            switch (type) {
                case 0: /* BYTE */
                    LOGI("  [%d] %s (byte) x%d = %u,%u",
                         i, name ? name : "?", cnt,
                         e.data.u8 && cnt > 0 ? (unsigned)e.data.u8[0] : 0,
                         e.data.u8 && cnt > 1 ? (unsigned)e.data.u8[1] : 0);
                    break;
                case 1: /* INT32 */
                    LOGI("  [%d] %s (int32) x%d = %d,%d",
                         i, name ? name : "?", cnt,
                         e.data.i32 && cnt > 0 ? e.data.i32[0] : 0,
                         e.data.i32 && cnt > 1 ? e.data.i32[1] : 0);
                    break;
                case 2: /* FLOAT */
                    LOGI("  [%d] %s (float) x%d = %.2f,%.2f",
                         i, name ? name : "?", cnt,
                         e.data.f && cnt > 0 ? (double)e.data.f[0] : 0.0,
                         e.data.f && cnt > 1 ? (double)e.data.f[1] : 0.0);
                    break;
                case 3: /* INT64 */
                    LOGI("  [%d] %s (int64) x%d = %lld,%lld",
                         i, name ? name : "?", cnt,
                         e.data.i64 && cnt > 0 ? (long long)e.data.i64[0] : 0LL,
                         e.data.i64 && cnt > 1 ? (long long)e.data.i64[1] : 0LL);
                    break;
                case 4: /* DOUBLE */
                    LOGI("  [%d] %s (double) x%d = %.2f,%.2f",
                         i, name ? name : "?", cnt,
                         e.data.d && cnt > 0 ? e.data.d[0] : 0.0,
                         e.data.d && cnt > 1 ? e.data.d[1] : 0.0);
                    break;
                default:
                    LOGI("  [%d] %s (type=%u) x%d", i, name ? name : "?", type, cnt);
                    break;
            }
        }
    } else if (f_tag_name && n_tag > 0 && n_tag <= 64) {
        LOGI("PCR[hook%d] frm#%u — metadata present (%d tags); entries not enumerable",
             hook_idx, frame_number, n_tag);
    }
}

/* Metadata-only observer proxy for android::camera3::processCaptureResult
 * (OUTPUTUTILS variant): signature is
 *   void(CaptureOutputStates& states, camera_capture_result const* result)
 * The 2nd arg is a struct whose first fields match camera3_capture_result_t
 * (frame_number, result, num_output_buffers, ...). We only READ + LOG metadata
 * and call through — we never write/inject buffers, so we avoid the SEGV_ACCERR
 * that injection here used to cause. */
static void my_pcr_meta_observer_proxy(void *states, const void *raw_result) {
    SHADOWHOOK_STACK_SCOPE();
    /* Count only. We do NOT dereference `raw_result` here: on some OPPO/Android 14
     * builds the OUTPUTUTILS processCaptureResult receives a struct whose layout
     * differs from camera3_capture_result_t (it's reached from the HIDL
     * processOneCaptureResultLockedT chain), so reading result->result /
     * frame_number / metadata dereferences a misaligned pointer and SIGSEGVs at
     * offset ~0x9, crashing cameraserver. The real injection runs through the ROB
     * hook, so the metadata observer is purely diagnostic — keeping it deref-free
     * lets it exist without crashing the camera. */
    g_pcr_meta_fires.fetch_add(1);
    SHADOWHOOK_CALL_PREV(my_pcr_meta_observer_proxy, states, raw_result);
}


static void pcr_inject_frames(const camera3_capture_result_t *result, int hook_idx) {
    if (!result) return;

    uint32_t total = g_total_pcr_calls.fetch_add(1);
    if (hook_idx >= 0 && hook_idx < MAX_PCR_VARIANTS) {
        g_pcr_fire_count[hook_idx].fetch_add(1);
    }

    if ((total % DIAG_EVERY_N_CALLS) == (DIAG_EVERY_N_CALLS - 1)) {
        dump_diagnostics();
    }





































    if (hook_idx >= 0 && hook_idx < g_pcr_stub_count) {
        int v = g_pcr_variants[hook_idx];
        if (v == PCR_VARIANT_OUTPUTUTILS) {





















            uint32_t fires = g_pcr_fire_count[hook_idx].load();
            if (fires == 1 || (fires % 200) == 0) {
                LOGI("PCR[hook%d] variant=OUTPUTUTILS fires=%u — metadata-only observer "
                     "(injecting here would SEGV_ACCERR)",
                     hook_idx, fires);
            }
            return;
        } else if (v != PCR_VARIANT_MEMBER) {


            uint32_t fires = g_pcr_fire_count[hook_idx].load();
            if (fires == 1 || (fires % 200) == 0) {
                LOGI("PCR[hook%d] variant=%d fires=%u — diagnostics-only "
                     "(HIDL/AIDL: result* is not camera3_capture_result_t, not injectable)",
                     hook_idx, v, fires);
            }
            return;
        }
    }


    if (!g_enabled.load(std::memory_order_relaxed)) {
        g_skip_disabled.fetch_add(1);
        return;
    }

    /* Log the ORIGINAL per-frame metadata (no forging) so we can inspect what
     * the real camera reports. Throttled to avoid spamming the log. */
    {
        uint32_t fires = g_pcr_fire_count[hook_idx].load();
        if (fires <= 5 || (fires % 200) == 0) {
            log_original_metadata(result, hook_idx, result->frame_number);
        }
    }

    /* BUG-FIX: use frame_source_initialized() (IPC connected?) not
     * frame_source_ready() (ring has new frame?).  frame_source_ready()
     * returns false whenever the producer's write rate is slower than the
     * camera's capture rate, causing 98%+ of frames to be skipped entirely.
     * frame_source_get_latest() returns the most recently written frame
     * without consuming it, giving "hold last frame" semantics. */
    if (!frame_source_initialized()) {
        uint32_t cnt = g_skip_no_source.fetch_add(1);
        if (cnt == 0 || (cnt % 100) == 99) {
            LOGW("PCR[hook%d] frm#%u — IPC not connected (skip_no_source=%u)",
                 hook_idx, result->frame_number, cnt + 1);
        }
        return;
    }

    if (result->num_output_buffers == 0) {
        uint32_t cnt = g_skip_zero_bufs.fetch_add(1) + 1;
        if (cnt <= 5 || (cnt % 100) == 0) {
            LOGW("PCR[hook%d] frm#%u — num_output_buffers=0, skipping injection (skip_zero_bufs=%u). "
                 "On OPlus, the HAL delivers buffers via returnOutputBuffers (inlined). "
                 "Camera3OutputStream::returnBuffer hook should handle this frame.",
                 hook_idx, result ? result->frame_number : 0u, cnt);
        }
        return;
    }

    FrameData src;
    if (!frame_source_get_latest(&src)) {
        /* No frame written to ring yet at all */
        uint32_t cnt = g_skip_no_frame.fetch_add(1);
        if ((cnt % 50) == 0) {
            LOGD("PCR[hook%d] frm#%u — no frame written yet (skip_no_frame=%u)",
                 hook_idx, result->frame_number, cnt + 1);
        }
        return;
    }

    if (!src.y_plane || src.width == 0 || src.height == 0 || src.stride == 0) {
        LOGE("PCR[hook%d] frm#%u — INVALID FrameData y=%p w=%u h=%u stride=%u",
             hook_idx, result->frame_number,
             (void *)src.y_plane, src.width, src.height, src.stride);
        return;
    }

    if (hook_idx >= 0 && hook_idx < MAX_PCR_VARIANTS) {
        g_pcr_inject_attempt[hook_idx].fetch_add(1);
    }

    int injected = 0, skipped_role = 0, failed = 0;
    for (uint32_t i = 0; i < result->num_output_buffers; i++) {
        const camera3_stream_buffer_t *buf = &result->output_buffers[i];
        if (!buf || !buf->stream) { failed++; continue; }

        StreamRole role = stream_map_get_role(buf->stream);
        if (!stream_map_should_inject(role, buf->status)) {
            skipped_role++;
            g_buf_skip_role.fetch_add(1);
            continue;
        }

        bool ok = frame_inject_one(buf, role, &src);
        if (ok) { injected++; g_buf_ok.fetch_add(1); }
        else    { failed++;   g_buf_fail.fetch_add(1); }
    }

    if (injected > 0) {
        uint32_t total_ok = g_inject_ok.fetch_add(1) + 1;
        if ((total_ok % 30) == 1) {
            LOGI("PCR[hook%d] ✓ frm#%u: %d/%u buf(s) injected (skip=%d fail=%d) total_ok=%u",
                 hook_idx, result->frame_number, injected,
                 result->num_output_buffers, skipped_role, failed, total_ok);
        }
    } else {
        uint32_t total_fail = g_inject_fail.fetch_add(1) + 1;
        if ((total_fail % 20) == 1) {
            LOGW("PCR[hook%d] ✗ frm#%u: 0/%u injected (skip=%d fail=%d) total_fail=%u",
                 hook_idx, result->frame_number,
                 result->num_output_buffers, skipped_role, failed, total_fail);
        }
    }
}

static void rob_inject_frames(const camera3_stream_buffer_t *outputBuffers,
                               uint32_t numBuffers, int rob_idx) {
    if (!outputBuffers || numBuffers == 0) return;

    uint32_t total = g_total_rob_calls.fetch_add(1);
    if (rob_idx >= 0 && rob_idx < MAX_ROB_VARIANTS) {
        g_rob_fire_count[rob_idx].fetch_add(1);
    }

    if ((total % DIAG_EVERY_N_CALLS) == (DIAG_EVERY_N_CALLS - 1)) {
        dump_diagnostics();
    }

    if (!g_enabled.load(std::memory_order_relaxed)) {
        g_skip_disabled.fetch_add(1);
        return;
    }

    /* Log stream info on early fires and periodically so we can see exactly
     * which streams the ROB hook is catching (format, usage, classified role).
     * This is critical for diagnosing "wrong stream" injection failures. */
    if (numBuffers > 0 && (total <= 5 || (total % 100) == 0)) {
        for (uint32_t _bi = 0; _bi < numBuffers && _bi < 4; _bi++) {
            const camera3_stream_t *_s = outputBuffers[_bi].stream;
            StreamRole _r = stream_map_get_role(_s);
            LOGI("ROB[%d] buf[%u]: fmt=0x%x %ux%u usage=0x%08x role=%d(%s) status=%d",
                 rob_idx, _bi,
                 _s ? _s->format : 0,
                 _s ? (uint32_t)_s->width  : 0,
                 _s ? (uint32_t)_s->height : 0,
                 _s ? (uint32_t)_s->usage  : 0,
                 (int)_r,
                 _r==STREAM_ROLE_PREVIEW?"PREVIEW":
                 _r==STREAM_ROLE_VIDEO  ?"VIDEO"  :
                 _r==STREAM_ROLE_SNAPSHOT?"SNAP"  :
                 _r==STREAM_ROLE_YUV_ANALYSIS?"YUV_ANAL":
                 _r==STREAM_ROLE_ML_THUMB?"ML_THUMB":
                 _r==STREAM_ROLE_RAW    ?"RAW"    :
                 _r==STREAM_ROLE_HDR    ?"HDR"    :"UNKNOWN",
                 outputBuffers[_bi].status);
        }
    }

    /* BUG-FIX: use frame_source_initialized() + frame_source_get_latest()
     * instead of frame_source_ready() + frame_source_get().
     * frame_source_ready() = "ring has new frame" so it fires false every
     * time the camera rate exceeds the producer rate — causing the massive
     * skip_no_source=196/200 seen in logs.  get_latest() returns the most
     * recently written slot WITHOUT consuming it, injecting the same frame
     * at camera rate when no newer frame is available yet ("hold last frame"). */
    if (!frame_source_initialized()) {
        uint32_t cnt = g_skip_no_source.fetch_add(1);
        if (cnt == 0 || (cnt % 100) == 99) {
            LOGW("ROB[%d] fires=%u — IPC not connected (skip_no_source=%u)",
                 rob_idx, g_rob_fire_count[rob_idx].load(), cnt + 1);
        }
        return;
    }

    FrameData src;
    if (!frame_source_get_latest(&src)) {
        /* write_slot still 0: no frame produced yet */
        uint32_t cnt = g_skip_no_frame.fetch_add(1);
        if ((cnt % 50) == 0) {
            LOGD("ROB[%d] — no frame written yet (skip_no_frame=%u)", rob_idx, cnt + 1);
        }
        return;
    }

    if (!src.y_plane || src.width == 0 || src.height == 0 || src.stride == 0) {
        LOGE("ROB[%d] — INVALID FrameData y=%p w=%u h=%u stride=%u",
             rob_idx, (void *)src.y_plane, src.width, src.height, src.stride);
        return;
    }

    if (rob_idx >= 0 && rob_idx < MAX_ROB_VARIANTS) {
        g_rob_inject_attempt[rob_idx].fetch_add(1);
    }


    {
        uint32_t fires = g_rob_fire_count[rob_idx].load();
        if (fires <= 5 || (fires % 50) == 0) {
            LOGI("ROB[%d] inject fires=%u numBufs=%u src=%ux%u",
                 rob_idx, fires, numBuffers, src.width, src.height);
        }
    }

    int injected = 0, skipped_role = 0, failed = 0;
    for (uint32_t i = 0; i < numBuffers && i < 64; i++) {
        const camera3_stream_buffer_t *buf = &outputBuffers[i];
        if (!buf || !buf->stream) { failed++; continue; }



        if (!buf->buffer || !(*buf->buffer)) {
            LOGW("ROB[%d] buf[%u] — null gralloc handle, skipping (status=%d)",
                 rob_idx, i, buf->status);
            failed++;
            continue;
        }

        StreamRole role = stream_map_get_role(buf->stream);
        if (!stream_map_should_inject(role, buf->status)) {

            uint32_t fires = g_rob_fire_count[rob_idx].load();
            if (fires <= 5) {
                LOGD("ROB[%d] buf[%u] SKIP: role=%d status=%d fmt=0x%x %ux%u",
                     rob_idx, i, (int)role, buf->status,
                     buf->stream ? buf->stream->format : 0,
                     buf->stream ? buf->stream->width  : 0,
                     buf->stream ? buf->stream->height : 0);
            }
            skipped_role++;
            g_buf_skip_role.fetch_add(1);
            continue;
        }

        LOGD("ROB[%d] buf[%u] injecting: role=%d fmt=0x%x %ux%u",
             rob_idx, i, (int)role,
             buf->stream->format, buf->stream->width, buf->stream->height);

        bool ok = frame_inject_one(buf, role, &src);
        if (ok) { injected++; g_buf_ok.fetch_add(1); }
        else    { failed++;   g_buf_fail.fetch_add(1); }
    }

    if (injected > 0) {
        uint32_t total_ok = g_inject_ok.fetch_add(1) + 1;
        if ((total_ok % 30) == 1) {
            LOGI("ROB[%d] ✓ %d/%u buf(s) injected (skip=%d fail=%d) total_ok=%u",
                 rob_idx, injected, numBuffers, skipped_role, failed, total_ok);
        }
    } else {
        uint32_t total_fail = g_inject_fail.fetch_add(1) + 1;
        if ((total_fail % 20) == 1) {
            LOGW("ROB[%d] ✗ 0/%u injected (skip=%d fail=%d) total_fail=%u",
                 rob_idx, numBuffers, skipped_role, failed, total_fail);
        }
    }
}

#define DEFINE_PCR_PROXY(IDX)                                                           \
static void my_pcr_proxy_##IDX(void *arg0, const camera3_capture_result_t *result) {   \
    SHADOWHOOK_STACK_SCOPE();                                                           \
     \
           \
    pcr_inject_frames(result, IDX);                                                     \
    SHADOWHOOK_CALL_PREV(my_pcr_proxy_##IDX, arg0, result);                             \
}

DEFINE_PCR_PROXY(0)
DEFINE_PCR_PROXY(1)
DEFINE_PCR_PROXY(2)
DEFINE_PCR_PROXY(3)
DEFINE_PCR_PROXY(4)
DEFINE_PCR_PROXY(5)
DEFINE_PCR_PROXY(6)
DEFINE_PCR_PROXY(7)
DEFINE_PCR_PROXY(8)
DEFINE_PCR_PROXY(9)
DEFINE_PCR_PROXY(10)
DEFINE_PCR_PROXY(11)

static void *const g_pcr_proxy_table[MAX_PCR_VARIANTS] = {
    (void *)my_pcr_proxy_0,  (void *)my_pcr_proxy_1,
    (void *)my_pcr_proxy_2,  (void *)my_pcr_proxy_3,
    (void *)my_pcr_proxy_4,  (void *)my_pcr_proxy_5,
    (void *)my_pcr_proxy_6,  (void *)my_pcr_proxy_7,
    (void *)my_pcr_proxy_8,  (void *)my_pcr_proxy_9,
    (void *)my_pcr_proxy_10, (void *)my_pcr_proxy_11,
};

#define DEFINE_ROB_PROXY(IDX)                                                            \
static void my_rob_proxy_##IDX(                                                          \
        bool      useHalBufManager,                                                      \
        void     *listener,                                                              \
        const camera3_stream_buffer_t *outputBuffers,                                    \
        size_t    numBuffers,                                                             \
        int64_t   timestamp,                                                              \
        int64_t   readoutTimestamp,                                                       \
        bool      requested,                                                              \
        int64_t   requestTimeNs,                                                          \
        void     *sessionStatsBuilder,                                                   \
        bool      timestampIncreasing,                                                    \
        void     *outputSurfaces,                                                        \
        void     *resultExtras,                                                          \
        int       errorBufStrategy,                                                       \
        int32_t   transform) {                                                            \
    SHADOWHOOK_STACK_SCOPE();                                                            \
    if (outputBuffers && numBuffers > 0 && numBuffers <= 64) {                           \
        rob_inject_frames(outputBuffers, (uint32_t)numBuffers, IDX);                     \
    }                                                                                    \
    SHADOWHOOK_CALL_PREV(my_rob_proxy_##IDX,                                             \
                         useHalBufManager, listener, outputBuffers, numBuffers,          \
                         timestamp, readoutTimestamp, requested, requestTimeNs,          \
                         sessionStatsBuilder, timestampIncreasing,                       \
                         outputSurfaces, resultExtras, errorBufStrategy, transform);     \
}

DEFINE_ROB_PROXY(0)
DEFINE_ROB_PROXY(1)
DEFINE_ROB_PROXY(2)
DEFINE_ROB_PROXY(3)

static void *const g_rob_proxy_table[MAX_ROB_VARIANTS] = {
    (void *)my_rob_proxy_0, (void *)my_rob_proxy_1,
    (void *)my_rob_proxy_2, (void *)my_rob_proxy_3,
};

typedef int (*CsHalFn)(void *self, const camera_metadata_t *meta,
                        camera3_stream_configuration_t *cfg,
                        const void *offlineIds, int64_t handle);
typedef int (*CsMemberFn)(void *dev, camera3_stream_configuration_t *cfg);

#define DEFINE_CS_PROXY_HAL(IDX)                                                                    \
static int my_cs_proxy_hal_##IDX(void *self, const camera_metadata_t *meta,                        \
                                   camera3_stream_configuration_t *cfg,                              \
                                   const void *offlineIds, int64_t handle) {                         \
    SHADOWHOOK_STACK_SCOPE();                                                                        \
    g_cs_fire_count[IDX].fetch_add(1);                                                              \
    LOGI("CS[%d] configureStreams HAL fired (fires=%u)",                                            \
         IDX, g_cs_fire_count[IDX].load());                                                         \
    if (cfg) {                                                                                       \
        LOGI("CS[%d] HAL configureStreams: %u stream(s) — logging originals:", IDX, cfg->num_streams); \
        for (uint32_t _si = 0; _si < cfg->num_streams && _si < 32u; _si++) {                       \
            camera3_stream_t *_s = cfg->streams[_si];                                               \
            if (_s) LOGI("CS[%d]  stream[%u] fmt=0x%x %ux%u usage=0x%08" PRIx64 "%s",             \
                         IDX, _si, _s->format, _s->width, _s->height,                              \
                         (uint64_t)_s->usage,                                                       \
                         (_s->usage & GRALLOC_USAGE_PROTECTED) ? " [PROTECTED]" : "");              \
        }                                                                                           \
        int mod = stream_modify_usage_for_injection(cfg);                                            \
        if (mod > 0) LOGI("CS[%d] modified %d stream(s) — SW_WRITE_OFTEN added for injection", IDX, mod); \
        LOGI("CS[%d] pre-call: num_streams=%u", IDX, cfg->num_streams);                            \
    }                                                                                               \
    int ret = SHADOWHOOK_CALL_PREV(my_cs_proxy_hal_##IDX, self, meta, cfg, offlineIds, handle);    \
    LOGI("CS[%d] original returned %d", IDX, ret);                                                  \
    if (ret == 0 && cfg) {                                                                           \
        stream_map_rebuild(cfg);                                                                     \
        LOGI("CS[%d] stream map rebuilt: %u stream(s)", IDX, cfg->num_streams);                     \
    } else if (ret != 0) {                                                                           \
        LOGW("CS[%d] original failed (ret=%d) — stream map NOT rebuilt", IDX, ret);                \
    }                                                                                               \
    return ret;                                                                                      \
}

#define DEFINE_CS_PROXY_MEMBER(IDX)                                                                 \
static int my_cs_proxy_member_##IDX(void *dev, camera3_stream_configuration_t *cfg) {              \
    SHADOWHOOK_STACK_SCOPE();                                                                        \
    g_cs_fire_count[IDX].fetch_add(1);                                                              \
    LOGI("CS[%d] configureStreams MEMBER fired (fires=%u)",                                         \
         IDX, g_cs_fire_count[IDX].load());                                                         \
    if (cfg) {                                                                                       \
        LOGI("CS[%d] MEMBER configureStreams: %u stream(s) — logging originals:", IDX, cfg->num_streams); \
        for (uint32_t _si = 0; _si < cfg->num_streams && _si < 32u; _si++) {                       \
            camera3_stream_t *_s = cfg->streams[_si];                                               \
            if (_s) LOGI("CS[%d]  stream[%u] fmt=0x%x %ux%u usage=0x%08" PRIx64 "%s",             \
                         IDX, _si, _s->format, _s->width, _s->height,                              \
                         (uint64_t)_s->usage,                                                       \
                         (_s->usage & GRALLOC_USAGE_PROTECTED) ? " [PROTECTED]" : "");              \
        }                                                                                           \
        int mod = stream_modify_usage_for_injection(cfg);                                            \
        if (mod > 0) LOGI("CS[%d] modified %d stream(s) — SW_WRITE_OFTEN added for injection", IDX, mod); \
        LOGI("CS[%d] pre-call: num_streams=%u", IDX, cfg->num_streams);                            \
    }                                                                                               \
    int ret = SHADOWHOOK_CALL_PREV(my_cs_proxy_member_##IDX, dev, cfg);                            \
    LOGI("CS[%d] original returned %d", IDX, ret);                                                  \
    if (ret == 0 && cfg) {                                                                           \
        stream_map_rebuild(cfg);                                                                     \
        LOGI("CS[%d] stream map rebuilt: %u stream(s)", IDX, cfg->num_streams);                     \
    } else if (ret != 0) {                                                                           \
        LOGW("CS[%d] original failed (ret=%d) — stream map NOT rebuilt", IDX, ret);                \
    }                                                                                               \
    return ret;                                                                                      \
}

DEFINE_CS_PROXY_HAL(0)
DEFINE_CS_PROXY_HAL(1)
DEFINE_CS_PROXY_HAL(2)
DEFINE_CS_PROXY_MEMBER(3)
DEFINE_CS_PROXY_MEMBER(4)
DEFINE_CS_PROXY_MEMBER(5)

static void my_setusage_proxy(void *thiz, uint64_t usage) {
    SHADOWHOOK_STACK_SCOPE();
    const uint32_t fires = g_setusage_fires.fetch_add(1) + 1;

    const uint64_t orig = usage;
    usage &= ~(uint64_t)GRALLOC_USAGE_PROTECTED;
    usage |=  (uint64_t)(GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_OFTEN);

    if (fires <= 6 || (fires % 50) == 0) {
        LOGI("Camera3Stream::setUsage fires=%u: 0x%08" PRIx64 " → 0x%08" PRIx64 "%s",
             fires, orig, usage,
             (orig & GRALLOC_USAGE_PROTECTED)
                 ? " (PROTECTED stripped; SW_WRITE added)"
                 : " (SW_WRITE added; PROTECTED was absent)");
    }

    SHADOWHOOK_CALL_PREV(my_setusage_proxy, thiz, usage);
}

static int my_getendpointusage_proxy(void *thiz, uint64_t *out_usage) {
    SHADOWHOOK_STACK_SCOPE();
    const uint32_t fires = g_getendpointusage_fires.fetch_add(1) + 1;

    const int ret = SHADOWHOOK_CALL_PREV(my_getendpointusage_proxy, thiz, out_usage);

    if (ret == 0 && out_usage) {
        const uint64_t orig = *out_usage;
        *out_usage &= ~(uint64_t)GRALLOC_USAGE_PROTECTED;
        *out_usage |=  (uint64_t)(GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_OFTEN);

        if (fires <= 6 || (fires % 50) == 0) {
            LOGI("Camera3OutputStream::getEndpointUsage fires=%u: "
                 "consumer 0x%08" PRIx64 " → 0x%08" PRIx64 "%s",
                 fires, orig, *out_usage,
                 (orig & GRALLOC_USAGE_PROTECTED)
                     ? " (PROTECTED stripped from consumer; SW_WRITE added)"
                     : " (SW_WRITE added to consumer; PROTECTED was absent)");
        }
    }
    return ret;
}

static void *const g_cs_proxy_hal_table[MAX_CS_VARIANTS] = {
    (void *)my_cs_proxy_hal_0,    (void *)my_cs_proxy_hal_1,
    (void *)my_cs_proxy_hal_2,    (void *)my_cs_proxy_member_3,
    (void *)my_cs_proxy_member_4, (void *)my_cs_proxy_member_5,
};

#define DEFINE_RTRN_PROXY(IDX)                                                               \
static int32_t my_rtrn_proxy_##IDX(                                                          \
        void                        *thiz,                                                   \
        const camera3_stream_buffer_t *buf,                       \
        int64_t                      timestamp,                                              \
        int64_t                      readoutTimestamp,                                       \
        uint32_t                     timestampIncreasing,                          \
        void                        *surface_ids,                 \
        uint64_t                     frameNumber,                                            \
        int32_t                      transform) {                                            \
    SHADOWHOOK_STACK_SCOPE();                                                                \
    const uint32_t fires = g_rtrn_fire_count[IDX].fetch_add(1) + 1;                        \
    if (fires <= 5 || (fires % DIAG_EVERY_N_CALLS) == 0) {                                  \
        LOGI("RTRN[%d] returnBuffer fires=%u frameNum=%" PRIu64 " buf=%p stream=%p",        \
             IDX, fires, frameNumber, (void *)buf,                                           \
             buf ? (void *)buf->stream : nullptr);                                           \
    }                                                                                        \
    if (buf && buf->buffer && *buf->buffer &&                                                \
        buf->status == CAMERA3_BUFFER_STATUS_OK &&                                           \
        g_enabled.load(std::memory_order_relaxed) &&                                        \
        frame_source_initialized()) {                                                        \
        StreamRole role = stream_map_get_role(buf->stream);                                  \
        if (stream_map_should_inject(role, buf->status)) {                                   \
            FrameData src;                                                                   \
            if (frame_source_get_latest(&src)) {                                             \
                g_rtrn_inject_attempt[IDX].fetch_add(1);                                    \
                bool ok = frame_inject_one(buf, role, &src);                                 \
                if (ok) {                                                                    \
                    uint32_t total_ok = g_inject_ok.fetch_add(1) + 1;                       \
                    if (fires <= 5 || (total_ok % 30) == 1) {                               \
                        LOGI("RTRN[%d] ✓ inject OK fires=%u fmt=0x%x %ux%u total=%u",      \
                             IDX, fires,                                                     \
                             buf->stream ? buf->stream->format : 0,                          \
                             buf->stream ? (uint32_t)buf->stream->width  : 0u,              \
                             buf->stream ? (uint32_t)buf->stream->height : 0u,              \
                             total_ok);                                                      \
                    }                                                                        \
                } else {                                                                     \
                    uint32_t fail = g_inject_fail.fetch_add(1) + 1;                         \
                    if ((fail % 20) == 1) {                                                  \
                        LOGW("RTRN[%d] ✗ inject FAILED fires=%u role=%d",                  \
                             IDX, fires, (int)role);                                        \
                    }                                                                        \
                }                                                                            \
            }                                                                               \
        } else {                                                                             \
            g_buf_skip_role.fetch_add(1);                                                    \
            if (fires <= 5) {                                                                \
                LOGD("RTRN[%d] skip buf fires=%u role=%d status=%d",                        \
                     IDX, fires, (int)role, buf->status);                                    \
            }                                                                               \
        }                                                                                   \
    }                                                                                       \
    return SHADOWHOOK_CALL_PREV(my_rtrn_proxy_##IDX,                                        \
                                thiz, buf, timestamp, readoutTimestamp,                      \
                                timestampIncreasing, surface_ids,                            \
                                frameNumber, transform);                                     \
}

DEFINE_RTRN_PROXY(0)
DEFINE_RTRN_PROXY(1)
DEFINE_RTRN_PROXY(2)
DEFINE_RTRN_PROXY(3)

static void *const g_rtrn_proxy_table[MAX_RTRN_VARIANTS] = {
    (void *)my_rtrn_proxy_0, (void *)my_rtrn_proxy_1,
    (void *)my_rtrn_proxy_2, (void *)my_rtrn_proxy_3,
};

#define DEFINE_RTRN_LOCKED_PROXY(IDX)                                                          \
static int32_t my_rtrn_locked_proxy_##IDX(                                                     \
        void                          *thiz,                                                   \
        const camera3_stream_buffer_t *buf,                                                    \
        int64_t                        timestamp,                                               \
        int64_t                        readoutTimestamp,                                        \
        int32_t                        timestampIncreasing,                                     \
        void                          *surface_ids) {                                          \
    SHADOWHOOK_STACK_SCOPE();                                                                   \
    const uint32_t fires = g_rtrn_locked_fire_count[IDX].fetch_add(1) + 1;                    \
    if (fires <= 5 || (fires % DIAG_EVERY_N_CALLS) == 0) {                                    \
        LOGI("RTRN_LOCKED[%d] returnBufferLocked fires=%u buf=%p stream=%p fence=%d",         \
             IDX, fires, (void *)buf,                                                          \
             buf ? (void *)buf->stream : nullptr,                                              \
             buf ? buf->release_fence : -2);                                                   \
    }                                                                                          \
    if (buf && buf->release_fence >= 0) {                                                      \
        struct pollfd _pfd;                                                                     \
        _pfd.fd     = buf->release_fence;                                                      \
        _pfd.events = POLLIN;                                                                   \
        int _pr = poll(&_pfd, 1, 500);                                                         \
        if (_pr <= 0 && fires <= 3) {                                                          \
            LOGW("RTRN_LOCKED[%d] poll(release_fence=%d) ret=%d — "                          \
                 "timeout or error, proceeding anyway",                                        \
                 IDX, buf->release_fence, _pr);                                                \
        } else if (fires <= 5) {                                                               \
            LOGI("RTRN_LOCKED[%d] fence waited OK (fence=%d poll_ret=%d)",                    \
                 IDX, buf->release_fence, _pr);                                                \
        }                                                                                      \
    }                                                                                          \
    if (buf && buf->buffer && *buf->buffer &&                                                  \
        buf->status == CAMERA3_BUFFER_STATUS_OK &&                                             \
        g_enabled.load(std::memory_order_relaxed) &&                                          \
        frame_source_initialized()) {                                                          \
        StreamRole role = stream_map_get_role(buf->stream);                                    \
        if (stream_map_should_inject(role, buf->status)) {                                     \
            FrameData src;                                                                     \
            if (frame_source_get_latest(&src)) {                                               \
                g_rtrn_locked_inject_attempt[IDX].fetch_add(1);                               \
                bool ok = frame_inject_one(buf, role, &src);                                   \
                if (ok) {                                                                      \
                    uint32_t total_ok = g_inject_ok.fetch_add(1) + 1;                         \
                    if (fires <= 5 || (total_ok % 30) == 1) {                                 \
                        LOGI("RTRN_LOCKED[%d] ✓ inject OK fires=%u fmt=0x%x %ux%u total=%u", \
                             IDX, fires,                                                       \
                             buf->stream ? buf->stream->format : 0,                           \
                             buf->stream ? (uint32_t)buf->stream->width  : 0u,               \
                             buf->stream ? (uint32_t)buf->stream->height : 0u,               \
                             total_ok);                                                        \
                    }                                                                          \
                } else {                                                                       \
                    uint32_t fail = g_inject_fail.fetch_add(1) + 1;                           \
                    if ((fail % 20) == 1) {                                                    \
                        LOGW("RTRN_LOCKED[%d] ✗ inject FAILED fires=%u role=%d",            \
                             IDX, fires, (int)role);                                          \
                    }                                                                          \
                }                                                                             \
            }                                                                                 \
        } else {                                                                              \
            g_buf_skip_role.fetch_add(1);                                                     \
            if (fires <= 5) {                                                                 \
                LOGD("RTRN_LOCKED[%d] skip buf fires=%u role=%d status=%d",                  \
                     IDX, fires, (int)role, buf->status);                                     \
            }                                                                                 \
        }                                                                                     \
    }                                                                                         \
    return SHADOWHOOK_CALL_PREV(my_rtrn_locked_proxy_##IDX,                                   \
                                thiz, buf, timestamp, readoutTimestamp,                        \
                                timestampIncreasing, surface_ids);                             \
}

DEFINE_RTRN_LOCKED_PROXY(0)
DEFINE_RTRN_LOCKED_PROXY(1)
DEFINE_RTRN_LOCKED_PROXY(2)
DEFINE_RTRN_LOCKED_PROXY(3)

static void *const g_rtrn_locked_proxy_table[MAX_RTRN_LOCKED_VARIANTS] = {
    (void *)my_rtrn_locked_proxy_0, (void *)my_rtrn_locked_proxy_1,
    (void *)my_rtrn_locked_proxy_2, (void *)my_rtrn_locked_proxy_3,
};

int hook_proxy_install(void) {
    int expected = 0;
    if (!g_init_done.compare_exchange_strong(expected, 1)) {
        LOGI("hook_proxy_install: already initialized");
        return 0;
    }

    for (int i = 0; i < MAX_PCR_VARIANTS; i++) {
        g_pcr_fire_count[i].store(0);
        g_pcr_inject_attempt[i].store(0);
    }
    for (int i = 0; i < MAX_ROB_VARIANTS; i++) {
        g_rob_fire_count[i].store(0);
        g_rob_inject_attempt[i].store(0);
    }
    for (int i = 0; i < MAX_CS_VARIANTS; i++) {
        g_cs_fire_count[i].store(0);
    }
    for (int i = 0; i < MAX_RTRN_VARIANTS; i++) {
        g_rtrn_fire_count[i].store(0);
        g_rtrn_inject_attempt[i].store(0);
    }
    for (int i = 0; i < MAX_RTRN_LOCKED_VARIANTS; i++) {
        g_rtrn_locked_fire_count[i].store(0);
        g_rtrn_locked_inject_attempt[i].store(0);
    }


    int r = shadowhook_init(SHADOWHOOK_MODE_SHARED, false);
    if (r != 0) {
        int err = shadowhook_get_errno();
        if (err == 1) {
            LOGI("shadowhook_init deferred (pending ELF load)");
        } else if (err == 12) {
            LOGW("shadowhook_init: linker monitor unavailable (errno=12, memfd/ptrace) "
                 "— addr-based hooks unaffected, continuing");
        } else {
            LOGE("shadowhook_init failed: %s (errno=%d)", shadowhook_to_errmsg(err), err);
            g_init_done.store(0);
            return -1;
        }
    }


    LOGI("hook_proxy_install: resolving PCR+ROB variants (dynamic ELF scan primary)...");
    ResolvedSymbol pcr_all[MAX_PCR_VARIANTS];
    int pcr_count = resolve_all_pcr(pcr_all, MAX_PCR_VARIANTS);

    if (pcr_count == 0) {
        LOGE("Cannot resolve ANY processCaptureResult/returnOutputBuffers variant — hook aborted");
        g_init_done.store(0);
        return -1;
    }

    g_pcr_stub_count = 0;
    for (int i = 0; i < pcr_count && i < MAX_PCR_VARIANTS; i++) {
        ResolvedSymbol &pcr = pcr_all[i];
        LOGI("PCR[%d]: variant=%d addr=%p size=%zu source=%s",
             i, pcr.variant, pcr.ptr, pcr.size, pcr.source);





















        if (pcr.variant == PCR_VARIANT_HIDL_3_4 ||
            pcr.variant == PCR_VARIANT_HIDL_3_2 ||
            pcr.variant == PCR_VARIANT_AIDL) {
            LOGI("PCR[%d]: variant=%d — skipping hook (PAC-trampoline SIGILL on "
                 "OPlus Android 14; HIDL/AIDL variants are never injectable)",
                 i, pcr.variant);
            continue;
        }

        /* OUTPUTUTILS: metadata-only observer. This function
         * (android::camera3::processCaptureResult) carries the camera_metadata_t*.
         * We hook it ONLY to log the original metadata (AF/AE/face/...) and call
         * through WITHOUT injecting buffers (injecting here caused SEGV_ACCERR).
         * We follow the thunk and skip if the real impl starts with a PAC insn. */
        if (pcr.variant == PCR_VARIANT_OUTPUTUTILS) {
            void *obs_addr = pcr.ptr;
            if (pcr.size > 0 && pcr.size <= 16 && pcr.ptr) {
                const uint32_t insn = *((const uint32_t *)pcr.ptr);
                if ((insn >> 26) == 0x05u) {
                    uint32_t imm26  = insn & 0x03FFFFFFu;
                    int32_t  simm26 = (imm26 & 0x02000000u)
                                      ? (int32_t)(imm26 | 0xFC000000u)
                                      : (int32_t)imm26;
                    void *real_target = (uint8_t *)pcr.ptr + simm26 * 4;
                    LOGI("PCR[%d]: OUTPUTUTILS meta-observer thunk B→%p, following to %p",
                         i, pcr.ptr, real_target);
                    obs_addr = real_target;
                } else {
                    LOGW("PCR[%d]: OUTPUTUTILS size=%zu insn=0x%08x not a B — hooking as-is",
                         i, pcr.size, insn);
                }
            }
            if (obs_addr) {
                uint32_t insn0 = *((const uint32_t *)obs_addr);
                if (insn0 == 0xD503237Fu || insn0 == 0xD503257Fu || insn0 == 0xD503213Fu) {
                    LOGW("PCR[%d]: OUTPUTUTILS meta-observer %p starts with PAC insn 0x%08x "
                         "— skipping to prevent SIGILL (OPlus Android 14)",
                         i, obs_addr, insn0);
                    continue;
                }
            }
            void *stub = shadowhook_hook_sym_addr(obs_addr,
                                                  (void *)my_pcr_meta_observer_proxy,
                                                  nullptr);
            if (!stub) {
                LOGW("PCR[%d]: OUTPUTUTILS meta-observer hook FAILED (%s) — skipping",
                     i, shadowhook_to_errmsg(shadowhook_get_errno()));
            } else {
                g_pcr_meta_stub   = stub;
                g_pcr_meta_active = true;
                LOGI("PCR[%d]: OUTPUTUTILS metadata observer hooked OK — addr=%p stub=%p "
                     "(metadata-only, no injection)",
                     i, obs_addr, stub);
            }
            continue;
        }


        if (pcr.variant == PCR_VARIANT_ROB) {
            if (g_rob_stub_count >= MAX_ROB_VARIANTS) {
                LOGW("ROB[%d]: too many ROB variants (%d already), skipping", i, g_rob_stub_count);
                continue;
            }
            void *rob_hook = g_rob_proxy_table[g_rob_stub_count];






            void *rob_addr = pcr.ptr;
            if (pcr.size > 0 && pcr.size <= 16 && pcr.ptr) {
                const uint32_t insn = *((const uint32_t *)pcr.ptr);
                if ((insn >> 26) == 0x05u) {
                    uint32_t imm26  = insn & 0x03FFFFFFu;
                    int32_t  simm26 = (imm26 & 0x02000000u)
                                      ? (int32_t)(imm26 | 0xFC000000u)
                                      : (int32_t)imm26;
                    void *real_target = (uint8_t *)pcr.ptr + simm26 * 4;
                    LOGI("PCR[%d]: ROB thunk B→0x%08x at %p: following to real impl %p",
                         i, insn, pcr.ptr, real_target);
                    rob_addr = real_target;
                } else {
                    LOGW("PCR[%d]: ROB size=%zu but insn=0x%08x not a B — hooking addr as-is",
                         i, pcr.size, insn);
                }
            }


            if (rob_addr) {
                uint32_t insn0 = *((const uint32_t *)rob_addr);
                if (insn0 == 0xD503237Fu || insn0 == 0xD503257Fu || insn0 == 0xD503213Fu) {
                    LOGI("ROB[%d]: addr=%p (after thunk-follow) starts with PAC insn 0x%08x "
                         "— skipping to prevent SIGILL on OPlus Android 14",
                         i, rob_addr, insn0);
                    continue;
                }
            }

            void *stub = shadowhook_hook_sym_addr(rob_addr, rob_hook, nullptr);
            if (!stub) {
                int err = shadowhook_get_errno();
                LOGW("ROB[%d] hook FAILED (addr=%p): %s — skipping",
                     i, pcr.ptr, shadowhook_to_errmsg(err));
            } else {
                LOGI("ROB slot[%d] hooked OK: sym_idx=%d addr=%p stub=%p",
                     g_rob_stub_count, i, pcr.ptr, stub);
                g_rob_stubs[g_rob_stub_count] = stub;
                g_rob_stub_count++;
            }
            continue;
        }

        void *hook_fn   = g_pcr_proxy_table[g_pcr_stub_count];
        void *hook_addr = pcr.ptr;


        if (pcr.variant == PCR_VARIANT_OUTPUTUTILS &&
            pcr.size > 0 && pcr.size <= 16 && pcr.ptr) {
            const uint32_t insn = *((const uint32_t *)pcr.ptr);
            if ((insn >> 26) == 0x05u) {
                uint32_t imm26  = insn & 0x03FFFFFFu;
                int32_t  simm26 = (imm26 & 0x02000000u)
                                  ? (int32_t)(imm26 | 0xFC000000u)
                                  : (int32_t)imm26;
                void *real_target = (uint8_t *)pcr.ptr + simm26 * 4;
                LOGI("PCR[%d]: OutputUtils thunk B→0x%08x at %p: following to real impl %p",
                     i, insn, pcr.ptr, real_target);
                hook_addr = real_target;
            } else {
                LOGW("PCR[%d]: OutputUtils size=%zu but insn=0x%08x not B — hooking addr as-is",
                     i, pcr.size, insn);
            }
        }

        if (g_pcr_stub_count >= MAX_PCR_VARIANTS) {
            LOGW("PCR[%d]: PCR table full, skipping", i);
            continue;
        }







        if (hook_addr) {
            uint32_t insn0 = *((const uint32_t *)hook_addr);
            if (insn0 == 0xD503237Fu || insn0 == 0xD503257Fu || insn0 == 0xD503213Fu) {
                LOGI("PCR[%d]: variant=%d hook_addr=%p starts with PAC instruction "
                     "0x%08x — skipping to prevent SIGILL (OPlus Android 14 PAC trampoline bug)",
                     i, pcr.variant, hook_addr, insn0);
                continue;
            }
        }

        void *stub = shadowhook_hook_sym_addr(hook_addr, hook_fn, nullptr);
        if (!stub) {
            int err = shadowhook_get_errno();
            LOGW("PCR[%d] hook FAILED (variant=%d addr=%p): %s — skipping",
                 i, pcr.variant, pcr.ptr, shadowhook_to_errmsg(err));
        } else {
            LOGI("PCR slot[%d] hooked OK: sym_idx=%d variant=%d stub=%p fn=%p",
                 g_pcr_stub_count, i, pcr.variant, stub, hook_fn);
            g_pcr_stubs[g_pcr_stub_count]    = stub;
            g_pcr_variants[g_pcr_stub_count] = pcr.variant;
            g_pcr_stub_count++;
        }
    }

    int total_hooks = g_pcr_stub_count + g_rob_stub_count;
    if (total_hooks == 0) {
        LOGE("All PCR/ROB hook attempts failed — frame injection will NOT work");
        g_init_done.store(0);
        return -1;
    }
    LOGI("PCR: %d hook(s) installed  ROB: %d hook(s) installed",
         g_pcr_stub_count, g_rob_stub_count);

    if (g_rob_stub_count == 0) {
        LOGW("WARNING: returnOutputBuffers hook not installed. "
             "Injection may not work on HIDL Android 14 devices. "
             "Check if returnOutputBuffers is exported in libcameraservice.so.");
    } else {
        LOGI("returnOutputBuffers hooked — HIDL/AIDL injection active.");
    }

    LOGI("NOTE: Watch ROB fire counts in diagnostics — ROB fires = injection is running.");


    LOGI("hook_proxy_install: resolving CS variants (dynamic ELF scan)...");
    ResolvedSymbol cs_all[MAX_CS_VARIANTS];
    int cs_count = resolve_all_cs(cs_all, MAX_CS_VARIANTS);

    if (cs_count == 0) {
        LOGW("configureStreams: no variants found — using format/data_space classification only");
    }

    g_cs_stub_count = 0;
    for (int i = 0; i < cs_count && i < MAX_CS_VARIANTS; i++) {
        ResolvedSymbol &cs = cs_all[i];
        LOGI("CS[%d]: variant=%d addr=%p source=%s",
             i, cs.variant, cs.ptr, cs.source);

        if (cs.variant == CS_VARIANT_MEMBER_CFG || cs.variant == CS_VARIANT_UNKNOWN) {
            LOGI("CS[%d]: SKIPPING variant=%d (Camera3Device — no camera3_stream_configuration_t*, "
                 "hooking with HAL ABI crashes cameraserver)", i, cs.variant);
            continue;
        }

        if (g_cs_stub_count >= 3) {
            LOGW("CS[%d]: too many HAL CS variants (%d already hooked), skipping", i, g_cs_stub_count);
            continue;
        }
        void *cs_hook = g_cs_proxy_hal_table[g_cs_stub_count];


        void *stub = shadowhook_hook_sym_addr(cs.ptr, cs_hook, nullptr);
        if (!stub) {
            int err = shadowhook_get_errno();
            LOGW("CS[%d] hook FAILED (variant=%d): %s — skipping",
                 i, cs.variant, shadowhook_to_errmsg(err));
        } else {
            LOGI("CS[%d] hooked OK: stub=%p variant=%d",
                 g_cs_stub_count, stub, cs.variant);
            g_cs_stubs[g_cs_stub_count]    = stub;
            g_cs_variants[g_cs_stub_count] = cs.variant;
            g_cs_stub_count++;
        }
    }

    LOGI("CS: %d/%d variant(s) hooked", g_cs_stub_count, cs_count);






    LOGI("hook_proxy_install: resolving Camera3Stream::setUsage + "
         "Camera3OutputStream::getEndpointUsage (BUG-09 fix)...");
    {
        ResolvedSymbol su_sym = {}, gep_sym = {};
        resolve_camera3_usage_hooks(&su_sym, &gep_sym);

        if (su_sym.ptr) {
            g_setusage_stub = shadowhook_hook_sym_addr(
                su_sym.ptr, (void *)my_setusage_proxy, nullptr);
            if (g_setusage_stub) {
                LOGI("Camera3Stream::setUsage hooked OK — addr=%p stub=%p sym=%s",
                     su_sym.ptr, g_setusage_stub, su_sym.symbol ? su_sym.symbol : "?");
            } else {
                LOGW("Camera3Stream::setUsage hook FAILED: %s",
                     shadowhook_to_errmsg(shadowhook_get_errno()));
            }
        } else {
            LOGW("Camera3Stream::setUsage NOT FOUND — producer PROTECTED strip inactive");
        }

        if (gep_sym.ptr) {
            g_getendpointusage_stub = shadowhook_hook_sym_addr(
                gep_sym.ptr, (void *)my_getendpointusage_proxy, nullptr);
            if (g_getendpointusage_stub) {
                LOGI("Camera3OutputStream::getEndpointUsage hooked OK — "
                     "addr=%p stub=%p sym=%s",
                     gep_sym.ptr, g_getendpointusage_stub,
                     gep_sym.symbol ? gep_sym.symbol : "?");
            } else {
                LOGW("Camera3OutputStream::getEndpointUsage hook FAILED: %s",
                     shadowhook_to_errmsg(shadowhook_get_errno()));
            }
        } else {
            LOGW("Camera3OutputStream::getEndpointUsage NOT FOUND — "
                 "consumer PROTECTED will survive buffer allocation");
        }
    }











    LOGI("hook_proxy_install: resolving Camera3OutputStream::returnBuffer "
         "(OPlus primary per-buffer injection point)...");
    if (g_rob_stub_count > 0) {
        LOGI("Camera3OutputStream::returnBuffer: skipping — ROB hook(s) installed (%d). "
             "returnBuffer fires WITHIN returnOutputBuffers; having both active causes "
             "double-injection of the same buffer (two different frames written to one buffer). "
             "ROB handles all injection on this device.",
             g_rob_stub_count);
    } else {
        ResolvedSymbol rtrn_syms[MAX_RTRN_VARIANTS];
        int rtrn_count = resolve_camera3_returnbuffer(rtrn_syms, MAX_RTRN_VARIANTS);

        for (int i = 0; i < rtrn_count && g_rtrn_stub_count < MAX_RTRN_VARIANTS; i++) {
            void *stub = shadowhook_hook_sym_addr(
                rtrn_syms[i].ptr,
                g_rtrn_proxy_table[g_rtrn_stub_count],
                nullptr);
            if (stub) {
                LOGI("Camera3OutputStream::returnBuffer[%d] hooked OK — addr=%p stub=%p",
                     g_rtrn_stub_count, rtrn_syms[i].ptr, stub);
                g_rtrn_stubs[g_rtrn_stub_count] = stub;
                g_rtrn_stub_count++;
            } else {
                LOGW("Camera3OutputStream::returnBuffer[%d] hook FAILED: %s",
                     i, shadowhook_to_errmsg(shadowhook_get_errno()));
            }
        }

        if (g_rtrn_stub_count > 0) {
            LOGI("Camera3OutputStream::returnBuffer: %d hook(s) installed — "
                 "OPlus per-buffer injection active", g_rtrn_stub_count);
        } else {
            LOGW("Camera3OutputStream::returnBuffer: NOT FOUND or hook FAILED — "
                 "trying returnBufferLocked fallback next");
        }
    }














    LOGI("hook_proxy_install: resolving Camera3OutputStream::returnBufferLocked "
         "(OPlus fallback per-buffer injection point, fence-wait before write)...");
    if (g_rob_stub_count > 0 || g_rtrn_stub_count > 0) {
        LOGI("Camera3OutputStream::returnBufferLocked: skipping — higher-priority "
             "injection hook(s) already installed (ROB=%d RTRN=%d). "
             "returnBufferLocked fires WITHIN the returnBuffer/returnOutputBuffers call "
             "chain; both active = double-injection of the same buffer.",
             g_rob_stub_count, g_rtrn_stub_count);
    } else {
        ResolvedSymbol rtrn_locked_syms[MAX_RTRN_LOCKED_VARIANTS];
        int rtrn_locked_count = resolve_camera3_returnbufferlocked(
            rtrn_locked_syms, MAX_RTRN_LOCKED_VARIANTS);

        for (int i = 0; i < rtrn_locked_count && g_rtrn_locked_stub_count < MAX_RTRN_LOCKED_VARIANTS; i++) {
            void *stub = shadowhook_hook_sym_addr(
                rtrn_locked_syms[i].ptr,
                g_rtrn_locked_proxy_table[g_rtrn_locked_stub_count],
                nullptr);
            if (stub) {
                LOGI("Camera3OutputStream::returnBufferLocked[%d] hooked OK — "
                     "addr=%p stub=%p sym=%s",
                     g_rtrn_locked_stub_count,
                     rtrn_locked_syms[i].ptr, stub,
                     rtrn_locked_syms[i].symbol ? rtrn_locked_syms[i].symbol : "?");
                g_rtrn_locked_stubs[g_rtrn_locked_stub_count] = stub;
                g_rtrn_locked_stub_count++;
            } else {
                LOGW("Camera3OutputStream::returnBufferLocked[%d] hook FAILED: %s",
                     i, shadowhook_to_errmsg(shadowhook_get_errno()));
            }
        }

        if (g_rtrn_locked_stub_count > 0) {
            LOGI("Camera3OutputStream::returnBufferLocked: %d hook(s) installed — "
                 "OPlus fallback fence-waited injection active", g_rtrn_locked_stub_count);
        } else {
            if (g_rtrn_stub_count == 0 && g_rob_stub_count == 0) {
                LOGE("Camera3OutputStream::returnBufferLocked: NOT FOUND or hook FAILED. "
                     "ROB=0, RTRN=0, RTRN_LOCKED=0 — NO injection point available on this device.");
            } else {
                LOGW("Camera3OutputStream::returnBufferLocked: NOT FOUND (OK if RTRN or ROB is active).");
            }
        }
    }

    LOGI("═══════════════ HOOK INSTALL COMPLETE ═══════════════");
    LOGI("  PCR hooks:        %d  (HIDL/AIDL/OUTPUTUTILS skipped — PAC-protected or never injectable)", g_pcr_stub_count);
    LOGI("  ROB hooks:        %d  (primary; fires 0 if returnOutputBuffers is inlined by OPlus LTO)", g_rob_stub_count);
    LOGI("  RTRN hooks:       %d  (OPlus — Camera3OutputStream::returnBuffer, 8-param; skipped if ROB active)", g_rtrn_stub_count);
    LOGI("  RTRN_LOCKED hooks:%d  (OPlus fallback — returnBufferLocked, 6-param + fence wait; skipped if ROB/RTRN active)", g_rtrn_locked_stub_count);
    {
        int active = g_rob_stub_count + g_rtrn_stub_count + g_rtrn_locked_stub_count;
        if (active == 0) {
            LOGE("  ⚠ CRITICAL: no per-buffer injection hook installed — frames WILL NOT be injected");
        } else if (active > 1) {
            LOGW("  ⚠ Multiple injection hooks active (%d) — check for double-injection. "
                 "Expected: exactly ONE of ROB / RTRN / RTRN_LOCKED should be active.", active);
        } else {
            LOGI("  ✓ %d per-buffer injection hook active (single-path, no double-injection)", active);
        }
    }
    LOGI("  CS hooks:         %d", g_cs_stub_count);
    LOGI("  setUsage hook:         %s (BUG-09 producer strip)", g_setusage_stub ? "OK" : "MISSING");
    LOGI("  getEndpointUsage hook: %s (BUG-09 consumer strip)", g_getendpointusage_stub ? "OK" : "MISSING");
    LOGI("  Metadata observer: %s (OUTPUTUTILS processCaptureResult — logs original camera metadata)",
         g_pcr_meta_active ? "ACTIVE" : "inactive");
    LOGI("  Injection diagnostics will be logged every %d calls", DIAG_EVERY_N_CALLS);
    LOGI("  FIXES applied: PAC-trampoline SIGILL (V2), double-injection prevention (V2)");
    LOGI("═════════════════════════════════════════════════════");

    return 0;
}

void hook_proxy_uninstall(void) {
    for (int i = 0; i < g_pcr_stub_count; i++) {
        if (g_pcr_stubs[i]) {
            shadowhook_unhook(g_pcr_stubs[i]);
            g_pcr_stubs[i] = nullptr;
        }
    }
    if (g_pcr_stub_count > 0) {
        LOGI("processCaptureResult: %d hook(s) removed", g_pcr_stub_count);
        g_pcr_stub_count = 0;
    }

    for (int i = 0; i < g_rob_stub_count; i++) {
        if (g_rob_stubs[i]) {
            shadowhook_unhook(g_rob_stubs[i]);
            g_rob_stubs[i] = nullptr;
        }
    }
    if (g_rob_stub_count > 0) {
        LOGI("returnOutputBuffers: %d hook(s) removed", g_rob_stub_count);
        g_rob_stub_count = 0;
    }

    for (int i = 0; i < g_cs_stub_count; i++) {
        if (g_cs_stubs[i]) {
            shadowhook_unhook(g_cs_stubs[i]);
            g_cs_stubs[i] = nullptr;
        }
    }
    if (g_cs_stub_count > 0) {
        LOGI("configureStreams: %d hook(s) removed", g_cs_stub_count);
        g_cs_stub_count = 0;
    }

    if (g_setusage_stub) {
        shadowhook_unhook(g_setusage_stub);
        g_setusage_stub = nullptr;
        LOGI("Camera3Stream::setUsage unhooked");
    }
    if (g_getendpointusage_stub) {
        shadowhook_unhook(g_getendpointusage_stub);
        g_getendpointusage_stub = nullptr;
        LOGI("Camera3OutputStream::getEndpointUsage unhooked");
    }

    for (int i = 0; i < g_rtrn_stub_count; i++) {
        if (g_rtrn_stubs[i]) {
            shadowhook_unhook(g_rtrn_stubs[i]);
            g_rtrn_stubs[i] = nullptr;
        }
    }
    if (g_rtrn_stub_count > 0) {
        LOGI("Camera3OutputStream::returnBuffer: %d hook(s) removed", g_rtrn_stub_count);
        g_rtrn_stub_count = 0;
    }

    for (int i = 0; i < g_rtrn_locked_stub_count; i++) {
        if (g_rtrn_locked_stubs[i]) {
            shadowhook_unhook(g_rtrn_locked_stubs[i]);
            g_rtrn_locked_stubs[i] = nullptr;
        }
    }
    if (g_rtrn_locked_stub_count > 0) {
        LOGI("Camera3OutputStream::returnBufferLocked: %d hook(s) removed", g_rtrn_locked_stub_count);
        g_rtrn_locked_stub_count = 0;
    }

    if (g_pcr_meta_stub) {
        shadowhook_unhook(g_pcr_meta_stub);
        g_pcr_meta_stub = nullptr;
        g_pcr_meta_active = false;
        LOGI("processCaptureResult metadata-only observer removed");
    }

    stream_map_clear();
    g_init_done.store(0);
}

bool hook_proxy_is_active(void) {
    return g_pcr_stub_count > 0 || g_rob_stub_count > 0 ||
           g_rtrn_stub_count > 0 || g_rtrn_locked_stub_count > 0;
}

void hook_proxy_set_enabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);
    LOGI("Injection %s", enabled ? "ENABLED" : "DISABLED");
    dump_diagnostics();
}

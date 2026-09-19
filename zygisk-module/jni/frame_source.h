#pragma once
#include <stdint.h>
#include <stdbool.h>

#define FRAME_SOURCE_MAGIC  0xAB7C5801U
#define FRAME_RING_SLOTS    3

/* scale_q16 sentinel: 65536 = 1.0 (no zoom). Values > 65536 = zoom in.
 * pan_x / pan_y are signed pixel offsets in source-frame coordinates.
 * All three fields are written by the FaceGate app via nativeSetOverlayParams()
 * and read lock-free by the cameraserver Zygisk module inside inject_yuv(). */
typedef struct {
    uint32_t         magic;
    _Atomic uint32_t write_slot;
    _Atomic uint32_t read_slot;
    uint32_t         master_width;
    uint32_t         master_height;
    uint32_t         slot_stride;
    uint32_t         slot_format;
    /* Pan/zoom overlay control — written by FaceGate, read by cameraserver */
    _Atomic int32_t  pan_x;
    _Atomic int32_t  pan_y;
    _Atomic uint32_t scale_q16;       /* Q16 fixed-point: 65536 = 1.0 */
    _Atomic uint32_t source_rotation; /* CW degrees from media metadata (0/90/180/270) */
    _Atomic uint32_t manual_rotation; /* User-controlled extra CW degrees (0/90/180/270) */
    /* [gstreamer.4] Chroma A/B override for the opaque 0x22 (IMPLEMENTATION_DEFINED)
     * stream. Written by the app via NativeFrameProducer.setChromaOverride() and read
     * lock-free by cameraserver's injector. -1 = unset (use build default), 0 = NV12,
     * 1 = NV21. Reads -1 by default so the build-time -DUNISOC_22_CHROMA still applies. */
    _Atomic int32_t  chroma_override;
    /* [V26] incremented by the producer on every ring write. The injector keys
     * its converted-frame cache on this so repeated injections of the same
     * source frame are memcpys, not full libyuv rotate+scale chains. */
    _Atomic uint32_t frame_seq;
    /* [V27] CLOCK_BOOTTIME ms (mod 2^32) refreshed ~2x/s while the producer is
     * alive. The hook stops injecting (real camera passes through) when this
     * goes stale — i.e. the app was closed and there is no media/RTSP left to
     * pull frames from. Wrap-safe: compare as unsigned difference. */
    _Atomic uint32_t heartbeat_ms32;
    uint8_t          _pad[4];
} FrameSourceHeader;

#ifdef __cplusplus
#include <atomic>
static_assert(sizeof(FrameSourceHeader) == 64, "FrameSourceHeader must be 64 bytes");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "std::atomic<uint32_t> must be lock-free for shared-memory IPC");
#else
#include <stdatomic.h>
_Static_assert(sizeof(FrameSourceHeader) == 64, "FrameSourceHeader must be 64 bytes");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "_Atomic uint32_t must be lock-free for shared-memory IPC");
#endif

typedef struct {
    uint8_t  *y_plane;
    uint8_t  *uv_plane;
    uint32_t  width;
    uint32_t  height;
    uint32_t  stride;
    uint32_t  format;
} FrameData;

#ifdef __cplusplus
extern "C" {
#endif

int  frame_source_init(int ashmem_fd);
void frame_source_destroy(void);
bool frame_source_ready(void);
bool frame_source_get(FrameData *out);

/* Returns true when the IPC channel is connected and the shared-memory ring
 * has been successfully mmap'd.  Does NOT check whether new frames are
 * waiting — use this as the "is source connected?" gate in injection paths. */
bool frame_source_initialized(void);

/* Returns the most-recently-written frame WITHOUT consuming (advancing)
 * the read pointer.  Gives "hold last frame" semantics: every camera hook
 * call gets a valid frame at camera rate even when the producer writes at
 * a slower rate (e.g. 30 fps producer vs. 120 fps camera hook fires).
 * Returns false only when no frame has ever been written (write_slot == 0)
 * or the source is not yet initialized. */
bool frame_source_get_latest(FrameData *out);

/* Read the current overlay control params atomically from the mapped header.
 * All three out-pointers may be NULL (the corresponding field is skipped).
 * Returns false (and leaves outputs unchanged) when not initialized. */
bool frame_source_get_overlay_params(int32_t *out_pan_x,
                                     int32_t *out_pan_y,
                                     uint32_t *out_scale_q16);

/* [V26] Current producer frame sequence (0 until the first ring write). */
uint32_t frame_source_get_seq(void);

/* [V27] True while the producer process is alive (heartbeat fresh < 2500 ms).
 * False after the app is closed/killed, or before the producer ever started.
 * Injection must be skipped when this is false so the REAL camera shows. */
bool frame_source_live(void);

/* Return auto-detected source rotation (CW degrees: 0/90/180/270) from media metadata.
 * Returns 0 when not initialized or source has no rotation metadata. */
uint32_t frame_source_get_rotation(void);

/* Return user-controlled manual rotation offset (CW degrees: 0/90/180/270).
 * Returns 0 when not initialized. */
uint32_t frame_source_get_manual_rotation(void);

/* Return the combined (source + manual) rotation that inject_yuv should apply.
 * This is the single canonical rotation value — always use this in inject_yuv. */
uint32_t frame_source_get_total_rotation(void);

/* [gstreamer.4] Chroma A/B override for the opaque 0x22 stream, read from the shared
 * header. Returns -1 (unset -> use build default), 0 (NV12), or 1 (NV21). Returns -1
 * when not initialized. */
int32_t frame_source_get_chroma_override(void);

/* Write the chroma A/B override into the shared header (used by the app-side
 * NativeFrameProducer.setChromaOverride()). Values: -1 unset, 0 NV12, 1 NV21. */
void frame_source_set_chroma_override(int32_t override_is_nv21);

#ifdef __cplusplus
}
#endif

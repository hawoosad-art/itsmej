#pragma once
#include <stdbool.h>
#include "include/camera3_compat.h"
#include "stream_map.h"
#include "frame_source.h"
#ifdef __cplusplus
#include <android/hardware_buffer.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

int  frame_inject_init(void);
void frame_inject_destroy(void);
/* [V18 photo] direct JPEG injection for the PCR hook path (null hwb allowed
 * when only a dmabuf fd is available). */
bool inject_jpeg(AHardwareBuffer *hwb, const struct camera3_stream_buffer *buf,
                 const FrameData *src, int32_t fence_fd,
                 int32_t *out_release_fence, int dmabuf_fd);

bool frame_inject_one(const camera3_stream_buffer_t *buf,
                      StreamRole                     role,
                      const FrameData               *src);

/* [V54 zoom] Feed the app's zoom (ANDROID_SCALER_CROP_REGION +
 * SENSOR_INFO_ACTIVE_ARRAY_SIZE, active-array coordinates) from the capture
 * result metadata so JPEG injection can encode the zoomed sub-rectangle. */
void frame_inject_set_crop(const int32_t crop[4], const int32_t active[4]);

#ifdef __cplusplus
}
#endif

// [gstreamer.4] Chroma A/B. The fresh-build logs proved the SAME 0x22
// (IMPLEMENTATION_DEFINED) payload is correct on one stream and blue on another,
// and stale builds saw it blue as BOTH NV21 and NV12. A single hard-coded flag
// can't be trusted across devices, so we expose a runtime override that lets ONE
// APK test both chroma orders without a rebuild:
//
//     frame_inject_set_chroma_override(1)  -> force 0x22 as NV21 (U/V swapped)
//     frame_inject_set_chroma_override(0)  -> force 0x22 as NV12 (no swap)
//     frame_inject_set_chroma_override(-1) -> revert to the build-time default
//
// Every chroma decision is logged with the stream role + format + size + the
// effective order, so a single device run tells us what each consumer wants.
#ifdef __cplusplus
void frame_inject_set_chroma_override(int override_is_nv21);
int  frame_inject_get_chroma_override(void);
#endif

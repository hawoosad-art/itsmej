

#include "frame_inject.h"
#include "include/camera3_compat.h"
#include "frame_source.h"
#include "stream_map.h"

#include <android/log.h>
#include <android/hardware_buffer.h>
#include <android/api-level.h>
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <vector>
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

#define TAG "amkush/frame_inject"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

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
    LOGI("frame_inject_init: loading libraries...");


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
    /* Only default to 180 on a first-frame race; honor valid total==0 (upright). */
    if (rot == 0 &&
        frame_source_get_rotation() == 0 &&
        frame_source_get_manual_rotation() == 0 &&
        !frame_source_ready()) {
        rot = 180u;
    }
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
                        int dmabuf_fd) {
    if (!hwb || !src || !out_release_fence) return false;
    *out_release_fence = -1;

    AHardwareBuffer_Desc desc;
    g_describe(hwb, &desc);
    int actual_format = desc.format;

    LOGD("inject_yuv: dst=%ux%u desc.fmt=0x%x desc.stride=%u src=%ux%u stride=%u fence=%d",
         dst_w, dst_h, actual_format, desc.stride,
         src->width, src->height, src->stride, fence_fd);

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
                /* Only default to 180 on a first-frame race (no frame yet, no
                 * rotation set); honor a valid total==0 (upright/identity). */
                if (mm_rot == 0 &&
                    frame_source_get_rotation() == 0 &&
                    frame_source_get_manual_rotation() == 0 &&
                    !frame_source_ready()) {
                    mm_rot = 180u;
                }
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
                libyuv::NV12Scale(ms_y, ms_stride, ms_uv, ms_stride,
                                  ms_w, ms_h,
                                  mm_y, mm_stride, mm_uv, mm_stride,
                                  (int)dst_w, (int)dst_h, libyuv::kFilterLinear);
                __sync_synchronize();

                struct dma_buf_sync mm_sync_end = {};
                mm_sync_end.flags = (uint64_t)(DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
                ioctl(dmabuf_fd, DMA_BUF_IOCTL_SYNC, &mm_sync_end);
                munmap(mm_ptr, mm_sz);

                /* UBWC read-touch — forces GPU to read from CPU-written linear region */
                if (g_lock) {
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
            /* Only default to 180 on a first-frame race (no frame yet, no
             * rotation set); honor a valid total==0 (upright/identity). */
            if (fb_rot == 0 &&
                frame_source_get_rotation() == 0 &&
                frame_source_get_manual_rotation() == 0 &&
                !frame_source_ready()) {
                fb_rot = 180u;
            }
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

            libyuv::NV12Scale(fs_y, fs_stride, fs_uv, fs_stride,
                              fs_w, fs_h,
                              fb_y, fb_stride, fb_uv, fb_stride,
                              (int)dst_w, (int)dst_h, libyuv::kFilterLinear);
            __sync_synchronize();
            g_unlock(hwb, nullptr);
            /* UBWC read-touch for OPlus/Qualcomm cache coherency */
            {
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

        if (!dst_y || dst_stride <= 0) {
            LOGE("inject_yuv: plane[0] data=%p rowStride=%d — invalid",
                 (void *)dst_y, dst_stride);
            g_unlock(hwb, nullptr);
            return false;
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

                int cx = (src_w - crop_w) / 2 + ov_pan_x;
                int cy = (src_h - crop_h) / 2 + ov_pan_y;
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

                    uint8_t *dst_uv_lb = nullptr;
                    int      dst_us_lb = 0;
                    if (planes.planeCount >= 2 && planes.planes[1].data) {
                        dst_uv_lb = (uint8_t *)planes.planes[1].data;
                        dst_us_lb = (int)planes.planes[1].rowStride;
                        /* Fill UV plane with 128 (neutral chroma) */
                        for (uint32_t r = 0; r < dst_h / 2; r++)
                            memset(dst_uv_lb + (size_t)r * dst_us_lb, 128, (dst_w + 1) & ~1u);
                    }

                    const uint8_t *src_uv2 = src->uv_plane
                        ? src->uv_plane
                        : (src->y_plane + (size_t)src_stride * src->height);
                    uint8_t *sub_y = dst_y + (size_t)off_y_lb * dst_stride + off_x;

                    /* Fix V4.8.2: apply the same source rotation here that the
                     * zoom-in / 1x path applies in Fix1. The letterbox path used to
                     * jump straight to inject_done, skipping Fix1, so the zoomed-out
                     * frame was scaled WITHOUT rotation while 1x/zoom-in applied it
                     * → pressing zoom-out made the frame appear to rotate. Rotate the
                     * full source into a temp NV12 buffer, then letterbox-scale it. */
                    const uint8_t *rot_y  = eff_y;
                    const uint8_t *rot_uv = src_uv2;
                    int   rot_stride     = src_stride;
                    int   rot_w          = src_w;
                    int   rot_h          = src_h;
                    std::vector<uint8_t> rot_buf_lb;
                    uint32_t rot_lb = rotate_source_upright(
                        eff_y, src_uv2, src_stride, src_w, src_h,
                        rot_buf_lb, rot_y, rot_uv, rot_stride, rot_w, rot_h);

                    if (dst_uv_lb) {
                        uint8_t *sub_uv = dst_uv_lb + (size_t)(off_y_lb / 2) * dst_us_lb + off_x;
                        libyuv::NV12Scale(rot_y, rot_stride, rot_uv, rot_stride,
                                          rot_w, rot_h,
                                          sub_y, dst_stride, sub_uv, dst_us_lb,
                                          scaled_w, scaled_h,
                                          libyuv::kFilterLinear);
                    } else {
                        /* Y-only fallback */
                        for (int r = 0; r < scaled_h; r++) {
                            uint8_t *row_dst = sub_y + (size_t)r * dst_stride;
                            int src_r = r * rot_h / scaled_h;
                            if (src_r >= rot_h) src_r = rot_h - 1;
                            memcpy(row_dst, rot_y + (size_t)src_r * rot_stride,
                                   (size_t)scaled_w);
                        }
                    }
                    LOGD("inject_yuv: zoom-out letterbox scale_q16=%u scaled=%dx%d off=(%d,%d) rot=%u",
                         ov_scale, scaled_w, scaled_h, off_x, off_y_lb, rot_lb);
                }
                /* Unlock the outer lock before returning — this was the root cause
                 * of the "real camera on zoom-out" bug (buffer left locked). */
                g_unlock(hwb, nullptr);
                goto inject_done;
            }
        }


        /* Fix1: Apply source rotation written by frame_producer into the ring header.
         * Rotates the source NV12 plane pointers using a temp heap buffer and
         * libyuv I420Rotate so the content is right-way-up before libyuv scaling. */
        std::vector<uint8_t> rot_buf;
        {
            uint32_t src_rot = frame_source_get_total_rotation();
            /* Fallback ONLY for the first-frame race: if NO frame has been written
             * to the ring yet AND neither source nor manual rotation has been set,
             * the IPC rotation may not have arrived yet → default to 180°.
             *
             * IMPORTANT: do NOT fall back when total==0 from a VALID combination of
             * source+manual rotation. total==0 is the upright/identity orientation
             * (e.g. source_rotation=270 + manual_rotation=90 = 360 % 360 = 0). The old
             * `if (src_rot==0) src_rot=180` turned that upright position into 180°
             * (upside-down), which is exactly why pressing rotate never reached
             * upright. Only fall back if the ring has no frame yet AND rotation was
             * never set. */
            if (src_rot == 0 &&
                frame_source_get_rotation() == 0 &&
                frame_source_get_manual_rotation() == 0 &&
                !frame_source_ready()) {
                src_rot = 180u;
                LOGI("inject_yuv: Fix1 fallback: IPC src_rot=0 (no frame yet) → applying 180° default");
            }
            if (src_rot == 90 || src_rot == 180 || src_rot == 270) {
                libyuv::RotationMode rot_mode =
                    (src_rot == 90)  ? libyuv::kRotate90  :
                    (src_rot == 270) ? libyuv::kRotate270 : libyuv::kRotate180;

                int pre_w = src_w, pre_h = src_h;
                int post_w = (src_rot == 90 || src_rot == 270) ? pre_h : pre_w;  /* 90/270 swap dims */
                int post_h = (src_rot == 90 || src_rot == 270) ? pre_w : pre_h;
                post_w = (post_w + 1) & ~1;
                post_h = (post_h + 1) & ~1;

                /* Temp I420 buffers: pre-rotation (src) and post-rotation (dst) */
                int uv_w = (pre_w + 1) / 2, uv_h = (pre_h + 1) / 2;
                int r_uv_w = (post_w + 1) / 2, r_uv_h = (post_h + 1) / 2;
                size_t src_i420_sz = (size_t)pre_w * pre_h + (size_t)uv_w * uv_h * 2;
                size_t dst_i420_sz = (size_t)post_w * post_h + (size_t)r_uv_w * r_uv_h * 2;
                rot_buf.resize(src_i420_sz + dst_i420_sz);

                uint8_t *si_y = rot_buf.data();
                uint8_t *si_u = si_y + (size_t)pre_w * pre_h;
                uint8_t *si_v = si_u + (size_t)uv_w * uv_h;
                uint8_t *ri_y = rot_buf.data() + src_i420_sz;
                uint8_t *ri_u = ri_y + (size_t)post_w * post_h;
                uint8_t *ri_v = ri_u + (size_t)r_uv_w * r_uv_h;

                /* NV12→I420 of the (possibly cropped) effective source */
                const uint8_t *uv = eff_uv ? eff_uv : (eff_y + (size_t)src_stride * src_h);
                libyuv::NV12ToI420(eff_y, src_stride,
                                   uv,   src_stride,
                                   si_y, pre_w,
                                   si_u, uv_w,
                                   si_v, uv_w,
                                   pre_w, pre_h);

                libyuv::I420Rotate(si_y, pre_w, si_u, uv_w, si_v, uv_w,
                                   ri_y, post_w, ri_u, r_uv_w, ri_v, r_uv_w,
                                   pre_w, pre_h, rot_mode);

                /* Point eff_y / eff_uv at the rotated I420; build a packed NV12 uv */
                /* For simplicity keep as I420 — override eff_y/uv and mark planar */
                eff_y    = ri_y;
                eff_uv   = nullptr;   /* signal planar below */
                src_w    = post_w;
                src_h    = post_h;
                src_stride = post_w;

                /* We'll re-pack into NV12 now so the existing semiplanar path works */
                size_t nv12_sz = (size_t)post_w * post_h + (size_t)post_w * ((post_h + 1)/2);
                std::vector<uint8_t> nv12_tmp(nv12_sz);
                uint8_t *nv12_y  = nv12_tmp.data();
                uint8_t *nv12_uv = nv12_y + (size_t)post_w * post_h;
                libyuv::I420ToNV12(ri_y, post_w, ri_u, r_uv_w, ri_v, r_uv_w,
                                   nv12_y, post_w, nv12_uv, post_w,
                                   post_w, post_h);
                rot_buf.insert(rot_buf.end(), nv12_tmp.begin(), nv12_tmp.end());
                eff_y    = rot_buf.data() + src_i420_sz + dst_i420_sz;
                eff_uv   = eff_y + (size_t)post_w * post_h;
                src_stride = post_w;
                LOGI("inject_yuv: Fix1 V478R5 source_rotation=%u applied %dx%d->%dx%d swap=%d",
                     src_rot, pre_w, pre_h, post_w, post_h,
                     (int)(src_rot == 90 || src_rot == 270));

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
                            eff_uv += (size_t)(off_y2 / 2) * src_stride + off_x2;
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

        bool is_semiplanar = false, is_planar = false;
        switch (actual_format) {
            case HAL_PIXEL_FORMAT_YCBCR_420_888:
            case HAL_PIXEL_FORMAT_YCrCb_420_SP:
            case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
                if (planes.planeCount >= 3) {
                    if (planes.planes[1].pixelStride == 1) {
                        is_planar = true;
                    } else if (planes.planes[1].pixelStride == 2) {
                        is_semiplanar = true;
                    } else {
                        LOGW("inject_yuv: unexpected pixelStride=%u, defaulting to semiplanar",
                             planes.planes[1].pixelStride);
                        is_semiplanar = true;
                    }
                } else if (planes.planeCount == 2) {
                    is_semiplanar = true;
                }
                break;
            case HAL_PIXEL_FORMAT_YCBCR_P010:
                LOGW("inject_yuv: P010 not supported, skipping");
                g_unlock(hwb, nullptr);
                return false;
            case HAL_PIXEL_FORMAT_YV12:
                /* Planar YCrCb 4:2:0 — planes[1]=V, planes[2]=U, pixelStride=1.
                 * Previously fell into `default` → treated as semi-planar NV12,
                 * which wrote chroma to the wrong plane on video/record streams. */
                if (planes.planeCount >= 3 && planes.planes[1].pixelStride == 1) {
                    is_planar = true;
                } else {
                    is_semiplanar = (planes.planeCount >= 2);
                }
                break;
            default:
                is_semiplanar = (planes.planeCount >= 2);
                break;
        }

        LOGD("inject_yuv: is_semiplanar=%d is_planar=%d planeCount=%u",
             (int)is_semiplanar, (int)is_planar, planes.planeCount);

        bool ok = false;
        if (is_semiplanar && planes.planeCount >= 2) {
            bool is_nv21 = (planes.planeCount >= 3) &&
                           ((uintptr_t)planes.planes[1].data & 1u) &&
                           ((uintptr_t)planes.planes[2].data < (uintptr_t)planes.planes[1].data);

            LOGD("inject_yuv: NV21 detect: p1=%p p2=%p is_nv21=%d",
                 planes.planes[1].data,
                 planes.planeCount >= 3 ? planes.planes[2].data : nullptr,
                 (int)is_nv21);

            if (is_nv21) {
                uint8_t *uv_base      = (uint8_t *)planes.planes[2].data;
                int      dst_uv_stride = (int)planes.planes[2].rowStride;
                if (!uv_base || dst_uv_stride <= 0) {
                    LOGE("inject_yuv: NV21 plane[2] invalid data=%p stride=%d",
                         (void *)uv_base, dst_uv_stride);
                    g_unlock(hwb, nullptr);
                    return false;
                }

                size_t uv_rows    = ((size_t)dst_h + 1) / 2;
                size_t tmp_stride = (size_t)dst_uv_stride;
                std::vector<uint8_t> tmp_uv(tmp_stride * uv_rows);

                const uint8_t *src_uv = eff_uv;
                std::vector<uint8_t> src_uv_vec;
                if (!src_uv) {
                    int uv_w = (src_w + 1) / 2, uv_h = (src_h + 1) / 2;
                    src_uv_vec.resize((size_t)uv_w * uv_h * 2);
                    const uint8_t *su = src->y_plane + (size_t)src_stride * src_h;
                    const uint8_t *sv = su + (size_t)uv_w * uv_h;
                    for (int y = 0; y < uv_h; y++)
                        for (int x = 0; x < uv_w; x++) {
                            src_uv_vec[y * uv_w * 2 + x * 2]     = su[y * uv_w + x];
                            src_uv_vec[y * uv_w * 2 + x * 2 + 1] = sv[y * uv_w + x];
                        }
                    src_uv = src_uv_vec.data();
                }

                libyuv::NV12Scale(eff_y, src_stride, src_uv, src_stride,
                                  src_w, src_h,
                                  dst_y, dst_stride, tmp_uv.data(), (int)tmp_stride,
                                  (int)dst_w, (int)dst_h, libyuv::kFilterLinear);

                for (size_t r = 0; r < uv_rows; r++) {
                    uint8_t *srow = tmp_uv.data() + r * tmp_stride;
                    uint8_t *drow = uv_base       + r * (size_t)dst_uv_stride;
                    size_t   cols = (size_t)((dst_w + 1) / 2) * 2;
                    for (size_t c = 0; c + 1 < cols; c += 2) {
                        drow[c]     = srow[c + 1];
                        drow[c + 1] = srow[c];
                    }
                }
                /* stream_rotation not applied here: the source_rotation (Fix1 above)
                 * already handles orientation. Applying buf->stream->rotation on top
                 * causes stride/dimension mismatch and distortion for 90°/270° streams. */
                ok = true;
                LOGD("inject_yuv: NV21 scale+swap OK (%dx%d → %ux%u)", src_w, src_h, dst_w, dst_h);
            } else {
                uint8_t *dst_uv   = (uint8_t *)planes.planes[1].data;
                int dst_uv_stride = (int)planes.planes[1].rowStride;
                if (!dst_uv || dst_uv_stride <= 0) {
                    LOGE("inject_yuv: UV plane invalid data=%p stride=%d", (void *)dst_uv, dst_uv_stride);
                    g_unlock(hwb, nullptr);
                    return false;
                }
                if (src->uv_plane) {
                    libyuv::NV12Scale(eff_y, src_stride, eff_uv, src_stride,
                                      src_w, src_h,
                                      dst_y, dst_stride, dst_uv, dst_uv_stride,
                                      (int)dst_w, (int)dst_h, libyuv::kFilterLinear);
                } else {
                    int uv_w = (src_w + 1) / 2, uv_h = (src_h + 1) / 2;
                    std::vector<uint8_t> tmp_uv((size_t)uv_w * uv_h * 2);
                    const uint8_t *su = src->y_plane + (size_t)src_stride * src_h;
                    const uint8_t *sv = su + (size_t)uv_w * uv_h;
                    for (int y = 0; y < uv_h; y++)
                        for (int x = 0; x < uv_w; x++) {
                            tmp_uv[y * uv_w * 2 + x * 2]     = su[y * uv_w + x];
                            tmp_uv[y * uv_w * 2 + x * 2 + 1] = sv[y * uv_w + x];
                        }
                    libyuv::NV12Scale(eff_y, src_stride, tmp_uv.data(), uv_w * 2,
                                      src_w, src_h,
                                      dst_y, dst_stride, dst_uv, dst_uv_stride,
                                      (int)dst_w, (int)dst_h, libyuv::kFilterLinear);
                }
                /* stream_rotation not applied here: source_rotation (Fix1) covers this.
                 * Applying stream_rotation after NV12Scale uses wrong stride for 90°/270°. */
                ok = true;
                LOGD("inject_yuv: NV12 scale OK (%dx%d → %ux%u)", src_w, src_h, dst_w, dst_h);
            }

        } else if (is_planar && planes.planeCount >= 3) {
            uint8_t *dst_u = (uint8_t *)planes.planes[1].data;
            uint8_t *dst_v = (uint8_t *)planes.planes[2].data;
            int du_stride  = (int)planes.planes[1].rowStride;
            int dv_stride  = (int)planes.planes[2].rowStride;
            if (!dst_u || !dst_v || du_stride <= 0 || dv_stride <= 0) {
                LOGE("inject_yuv: I420 planes invalid u=%p v=%p us=%d vs=%d",
                     (void *)dst_u, (void *)dst_v, du_stride, dv_stride);
                g_unlock(hwb, nullptr);
                return false;
            }
            int uv_w = (src_w + 1) / 2, uv_h = (src_h + 1) / 2;
            if (eff_uv) {
                std::vector<uint8_t> tmp_u((size_t)uv_w * uv_h), tmp_v((size_t)uv_w * uv_h);
                for (int y = 0; y < uv_h; y++)
                    for (int x = 0; x < uv_w; x++) {
                        tmp_u[y * uv_w + x] = eff_uv[y * src_stride + x * 2];
                        tmp_v[y * uv_w + x] = eff_uv[y * src_stride + x * 2 + 1];
                    }
                libyuv::I420Scale(eff_y, src_stride, tmp_u.data(), uv_w, tmp_v.data(), uv_w,
                                  src_w, src_h,
                                  dst_y, dst_stride, dst_u, du_stride, dst_v, dv_stride,
                                  (int)dst_w, (int)dst_h, libyuv::kFilterLinear);
            } else {
                const uint8_t *su = src->y_plane + (size_t)src_stride * orig_src_h;
                const uint8_t *sv = su + (size_t)uv_w * uv_h;
                libyuv::I420Scale(eff_y, src_stride, su, uv_w, sv, uv_w,
                                  src_w, src_h,
                                  dst_y, dst_stride, dst_u, du_stride, dst_v, dv_stride,
                                  (int)dst_w, (int)dst_h, libyuv::kFilterLinear);
            }
            ok = true;
            LOGD("inject_yuv: I420 scale OK (%dx%d → %ux%u)", src_w, src_h, dst_w, dst_h);
        } else {
            LOGE("inject_yuv: unexpected planeCount=%u for fmt=0x%x — cannot inject",
                 planes.planeCount, actual_format);
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
        if (ok && g_lock) {
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

inject_done:
    /* Reached by the zoom-out letterbox path which locks/unlocks the buffer
     * itself and needs to bypass the normal per-format scaling block. */
    return true;
}

static bool inject_jpeg(AHardwareBuffer *hwb, const camera3_stream_buffer_t *buf,
                        const FrameData *src, int32_t fence_fd,
                        int32_t *out_release_fence,
                        int dmabuf_fd) {
    if (!hwb || !buf || !src || !out_release_fence) return false;
    *out_release_fence = -1;

    AHardwareBuffer_Desc desc;
    g_describe(hwb, &desc);



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

    LOGD("inject_jpeg: framework_blob_size=%zu src=%dx%d stride=%d fence=%d",
         framework_blob_size, src_w, src_h, src_stride, fence_fd);

    tjhandle tj = tjInitCompress();
    if (!tj) {
        LOGE("inject_jpeg: tjInitCompress failed");
        return false;
    }


    int uv_w = (src_w + 1) / 2;
    int uv_h = (src_h + 1) / 2;
    std::vector<uint8_t> u_plane((size_t)uv_w * uv_h);
    std::vector<uint8_t> v_plane((size_t)uv_w * uv_h);

    for (int y = 0; y < uv_h; y++) {
        for (int x = 0; x < uv_w; x++) {
            u_plane[y * uv_w + x] = src->uv_plane[y * src_stride + x * 2];
            v_plane[y * uv_w + x] = src->uv_plane[y * src_stride + x * 2 + 1];
        }
    }

    const unsigned char *planes_in[3] = {
        src->y_plane,
        u_plane.data(),
        v_plane.data()
    };
    int strides_in[3] = { src_stride, uv_w, uv_w };

    unsigned char *jpeg_buf = nullptr;
    unsigned long  jpeg_sz  = 0;

    int rc = tjCompressFromYUVPlanes(tj, planes_in, src_w, strides_in, src_h,
                                     TJSAMP_420, &jpeg_buf, &jpeg_sz, 85, TJFLAG_FASTDCT);
    tjDestroy(tj);

    if (rc != 0 || !jpeg_buf || jpeg_sz == 0) {
        LOGE("inject_jpeg: tjCompress FAILED: %s", tjGetErrorStr2(nullptr));
        if (jpeg_buf) tjFree(jpeg_buf);
        return false;
    }
    LOGD("inject_jpeg: JPEG encoded %lu bytes", jpeg_sz);

    void *vaddr = nullptr;

    int err = g_lock(hwb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, fence_fd, nullptr, &vaddr);
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
    g_unlock(hwb, &release_fence_fd);

    /* Qualcomm UBWC metadata invalidation workaround — same as inject_yuv */
    if (g_lock) {
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

    LOGD("inject_jpeg: OK — JPEG %lu bytes written to framework_blob_size=%zu", jpeg_sz, framework_blob_size);
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

    AHardwareBuffer *hwb = resolve_ahwb(buf);
    if (!hwb) {
        return false;
    }

    uint32_t dst_w = buf->stream ? buf->stream->width  : 0;
    uint32_t dst_h = buf->stream ? buf->stream->height : 0;
    int      fmt   = buf->stream ? buf->stream->format : HAL_PIXEL_FORMAT_BLOB;

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
            LOGD("frame_inject_one: injecting JPEG (blob) stream %ux%u", dst_w, dst_h);
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
            LOGD("frame_inject_one: injecting YUV stream fmt=0x%x %ux%u", fmt, dst_w, dst_h);
            ok = inject_yuv(hwb, dst_w, dst_h, src, incoming_fence, &downstream_fence, dmabuf_fd);
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

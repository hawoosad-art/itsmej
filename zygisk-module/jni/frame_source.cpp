

#include "frame_source.h"
#include <android/log.h>
#include <sys/mman.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include <atomic>

static inline std::atomic<uint32_t>* as_cpp_atomic(_Atomic uint32_t *p) {
    return reinterpret_cast<std::atomic<uint32_t>*>(p);
}

#define TAG "amkush/frame_source"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

static int         g_ashmem_fd  = -1;
static void       *g_map_base   = MAP_FAILED;
static size_t      g_map_size   = 0;
static uint8_t    *g_slots_base = nullptr;
static uint32_t    g_slot_stride = 0;
static uint32_t    g_slot_format = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static std::atomic<bool> g_initialized{false};

static size_t compute_slot_size(uint32_t stride, uint32_t h, uint32_t fmt) {
    uint64_t y_size = (uint64_t)stride * (uint64_t)h;
    switch (fmt) {
        case 0:
        case 1:
            return (size_t)(y_size + y_size / 2);
        case 2:
            return (size_t)(y_size + y_size / 2);
        case 3:
            return (size_t)y_size;
        default:
            return (size_t)(y_size + y_size / 2);
    }
}

static bool validate_slot_bounds(uint32_t slot_idx, size_t slot_sz,
                                  size_t total_off) {
    size_t slot_off = (size_t)slot_idx * slot_sz;
    size_t slot_end = slot_off + slot_sz;
    if (slot_end > total_off || slot_off > total_off) {
        LOGE("validate_slot_bounds: slot %u out of range (%zu > %zu)",
             slot_idx, slot_end, total_off);
        return false;
    }
    return true;
}

/* [V58 crash-fix] SIGSEGV frame_source_get_seq+16 (13.unisoc.2 tombstone_00,
 * fault addr = stale ring base + 0x34): init() used to munmap the OLD ring
 * before mmapping the new one, so a camera thread inside conv_cache_key()
 * could dereference the old g_map_base mid-remap. The old mapping is now
 * kept alive and reaped by a detached thread after 2 s — readers always see
 * either the old or the new, always-mapped, ring. */
static void schedule_delayed_unmap(void *base, size_t size) {
    if (!base || base == MAP_FAILED || size == 0) return;
    struct ReaperArgs { void *b; size_t s; };
    ReaperArgs *a = new ReaperArgs{base, size};
    pthread_t t;
    if (pthread_create(&t, nullptr, [](void *p) -> void * {
            ReaperArgs *args = (ReaperArgs *)p;
            struct timespec ts = {2, 0};
            nanosleep(&ts, nullptr);
            munmap(args->b, args->s);
            delete args;
            return nullptr;
        }, a) != 0) {
        munmap(base, size);   // last resort: old behavior, but no leak
        delete a;
    } else {
        pthread_detach(t);
    }
}

int frame_source_init(int ashmem_fd) {
    pthread_mutex_lock(&g_lock);

    /* [V58] flag first, but the OLD mapping stays mapped until the new one is
     * fully validated and published (see schedule_delayed_unmap). Readers
     * during this window get defaults (flag false) — never a dangling ptr. */
    g_initialized.store(false, std::memory_order_release);

    void *old_base = g_map_base;
    size_t old_size = g_map_size;
    if (g_ashmem_fd >= 0) {
        close(g_ashmem_fd);
    }
    g_ashmem_fd = ashmem_fd;

    off_t total_off = lseek(ashmem_fd, 0, SEEK_END);
    if (total_off <= 0) {
        LOGE("frame_source_init: lseek failed or empty fd");
        close(ashmem_fd);
        g_ashmem_fd = -1;
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    lseek(ashmem_fd, 0, SEEK_SET);


    size_t total = (size_t)total_off;
    if (total < sizeof(FrameSourceHeader)) {
        LOGE("frame_source_init: mapped size %zu too small for header (%zu)",
             total, sizeof(FrameSourceHeader));
        close(ashmem_fd);
        g_ashmem_fd = -1;
        pthread_mutex_unlock(&g_lock);
        return -1;
    }








    void *base = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, ashmem_fd, 0);
    if (base == MAP_FAILED) {
        LOGE("frame_source_init: mmap failed: %s", strerror(errno));
        close(ashmem_fd);
        g_ashmem_fd = -1;
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    close(ashmem_fd);
    g_ashmem_fd = -1;

    FrameSourceHeader *hdr = (FrameSourceHeader *)base;
    if (hdr->magic != FRAME_SOURCE_MAGIC) {
        LOGE("frame_source_init: bad magic 0x%08X (expected 0x%08X)",
             hdr->magic, FRAME_SOURCE_MAGIC);
        munmap(base, total);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    uint32_t w = hdr->master_width;
    uint32_t h = hdr->master_height;
    uint32_t stride = hdr->slot_stride;
    uint32_t fmt    = hdr->slot_format;

    if (w == 0 || h == 0 || stride == 0 || stride < w) {
        LOGE("frame_source_init: invalid dims %ux%u stride=%u", w, h, stride);
        munmap(base, total);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    size_t slot_sz = compute_slot_size(stride, h, fmt);
    size_t slots_total = slot_sz * FRAME_RING_SLOTS;
    size_t expected = sizeof(FrameSourceHeader) + slots_total;
    if (expected > total) {
        LOGE("frame_source_init: size mismatch expected=%zu actual=%zu", expected, total);
        munmap(base, total);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    g_map_base    = base;
    g_map_size    = total;
    g_slots_base  = (uint8_t *)base + sizeof(FrameSourceHeader);
    g_slot_stride = stride;
    g_slot_format = fmt;

    LOGI("frame_source_init: %ux%u stride=%u fmt=%u slot_sz=%zu total=%zu",
         w, h, stride, fmt, slot_sz, total);

    g_initialized.store(true, std::memory_order_release);
    schedule_delayed_unmap(old_base, old_size);   // [V58] safe: new ring published
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void frame_source_destroy(void) {


    g_initialized.store(false, std::memory_order_release);
    pthread_mutex_lock(&g_lock);
    void *old_base = MAP_FAILED;
    size_t old_size = 0;
    if (g_map_base != MAP_FAILED) {
        old_base = g_map_base;
        old_size = g_map_size;
        g_map_base = MAP_FAILED;
    }
    g_ashmem_fd   = -1;
    g_slots_base  = nullptr;
    g_slot_stride = 0;
    g_slot_format = 0;
    pthread_mutex_unlock(&g_lock);
    schedule_delayed_unmap(old_base, old_size);   // [V58] reap after readers drain
}

bool frame_source_initialized(void) {
    return g_initialized.load(std::memory_order_acquire);
}

bool frame_source_get_latest(FrameData *out) {
    if (!out || !g_initialized.load(std::memory_order_acquire)) return false;

    FrameSourceHeader *hdr = (FrameSourceHeader *)g_map_base;

    /* Return the most-recently written slot without consuming it.
     * write_slot==0 means no frame has been written yet. */
    uint32_t write_slot = as_cpp_atomic(&hdr->write_slot)->load(std::memory_order_acquire);
    if (write_slot == 0) return false;

    uint32_t latest_idx = (write_slot - 1) % FRAME_RING_SLOTS;

    uint32_t w      = hdr->master_width;
    uint32_t h      = hdr->master_height;
    uint32_t stride = hdr->slot_stride;
    uint32_t fmt    = hdr->slot_format;

    size_t slot_sz = compute_slot_size(stride, h, fmt);
    if (!validate_slot_bounds(latest_idx, slot_sz, g_map_size - sizeof(FrameSourceHeader))) {
        return false;
    }

    uint8_t *slot  = g_slots_base + latest_idx * slot_sz;
    out->y_plane   = slot;
    out->uv_plane  = slot + (size_t)stride * h;
    out->width     = w;
    out->height    = h;
    out->stride    = stride;
    out->format    = fmt;
    return true;
}

bool frame_source_ready(void) {


    if (!g_initialized.load(std::memory_order_acquire)) return false;
    FrameSourceHeader *hdr = (FrameSourceHeader *)g_map_base;
    uint32_t write_slot = as_cpp_atomic(&hdr->write_slot)->load(std::memory_order_acquire);
    uint32_t read_slot  = as_cpp_atomic(&hdr->read_slot)->load(std::memory_order_relaxed);
    return write_slot > read_slot;
}

bool frame_source_get(FrameData *out) {

    if (!out || !g_initialized.load(std::memory_order_acquire)) return false;

    FrameSourceHeader *hdr = (FrameSourceHeader *)g_map_base;






    uint32_t read_slot = as_cpp_atomic(&hdr->read_slot)->load(std::memory_order_relaxed);
    uint32_t next_read;
    do {
        uint32_t write_slot = as_cpp_atomic(&hdr->write_slot)->load(std::memory_order_acquire);
        if (write_slot <= read_slot) return false;
        next_read = read_slot + 1;
    } while (!as_cpp_atomic(&hdr->read_slot)->compare_exchange_weak(
                 read_slot, next_read,
                 std::memory_order_acq_rel, std::memory_order_relaxed));

    uint32_t read_idx = next_read % FRAME_RING_SLOTS;

    uint32_t w = hdr->master_width;
    uint32_t h = hdr->master_height;
    uint32_t stride = hdr->slot_stride;
    uint32_t fmt    = hdr->slot_format;

    size_t slot_sz = compute_slot_size(stride, h, fmt);
    if (!validate_slot_bounds(read_idx, slot_sz, g_map_size - sizeof(FrameSourceHeader))) {
        return false;
    }

    uint8_t *slot = g_slots_base + read_idx * slot_sz;
    out->y_plane  = slot;
    out->uv_plane = slot + (size_t)stride * h;
    out->width    = w;
    out->height   = h;
    out->stride   = stride;
    out->format   = fmt;


    return true;
}

bool frame_source_get_overlay_params(int32_t *out_pan_x,
                                     int32_t *out_pan_y,
                                     uint32_t *out_scale_q16) {
    if (!g_initialized.load(std::memory_order_acquire)) return false;
    FrameSourceHeader *hdr = (FrameSourceHeader *)g_map_base;
    if (out_pan_x)
        *out_pan_x    = reinterpret_cast<std::atomic<int32_t>*>(&hdr->pan_x)->load(std::memory_order_relaxed);
    if (out_pan_y)
        *out_pan_y    = reinterpret_cast<std::atomic<int32_t>*>(&hdr->pan_y)->load(std::memory_order_relaxed);
    if (out_scale_q16)
        *out_scale_q16 = reinterpret_cast<std::atomic<uint32_t>*>(&hdr->scale_q16)->load(std::memory_order_relaxed);
    return true;
}

uint32_t frame_source_get_rotation(void) {
    if (!g_initialized.load(std::memory_order_acquire)) return 0u;
    FrameSourceHeader *hdr = (FrameSourceHeader *)g_map_base;
    return reinterpret_cast<std::atomic<uint32_t>*>(&hdr->source_rotation)
               ->load(std::memory_order_relaxed);
}


uint32_t frame_source_get_manual_rotation(void) {
    if (!g_initialized.load(std::memory_order_acquire)) return 0u;
    FrameSourceHeader *hdr = (FrameSourceHeader *)g_map_base;
    return reinterpret_cast<std::atomic<uint32_t>*>(&hdr->manual_rotation)
               ->load(std::memory_order_relaxed);
}

bool frame_source_live(void) {
    FrameSourceHeader *hdr = (FrameSourceHeader *)g_map_base;
    if (!hdr) return false;
    uint32_t hb = reinterpret_cast<std::atomic<uint32_t>*>(&hdr->heartbeat_ms32)
                      ->load(std::memory_order_acquire);
    if (hb == 0) return false;   // producer never started
    struct timespec ts; clock_gettime(CLOCK_BOOTTIME, &ts);
    uint32_t now = (uint32_t)((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    return (now - hb) < 2500u;   // unsigned wrap-safe age check
}

uint32_t frame_source_get_seq(void) {
    /* [V58] was the tombstone_00 crash site: no g_initialized check and a
     * bare null test that MAP_FAILED ((void*)-1) sails through. */
    if (!g_initialized.load(std::memory_order_acquire)) return 0u;
    FrameSourceHeader *hdr = (FrameSourceHeader *)g_map_base;
    if (!hdr || hdr == MAP_FAILED) return 0u;
    return reinterpret_cast<std::atomic<uint32_t>*>(&hdr->frame_seq)
        ->load(std::memory_order_acquire);
}

uint32_t frame_source_get_total_rotation(void) {
    if (!g_initialized.load(std::memory_order_acquire)) return 0u;
    FrameSourceHeader *hdr = (FrameSourceHeader *)g_map_base;
    uint32_t src = reinterpret_cast<std::atomic<uint32_t>*>(&hdr->source_rotation)
                       ->load(std::memory_order_relaxed);
    uint32_t man = reinterpret_cast<std::atomic<uint32_t>*>(&hdr->manual_rotation)
                       ->load(std::memory_order_relaxed);
    return (src + man) % 360u;
}

// [gstreamer.4] Chroma A/B override for the opaque 0x22 stream (shared header).
// -1 = unset (use build default), 0 = NV12, 1 = NV21.
int32_t frame_source_get_chroma_override(void) {
    if (!g_initialized.load(std::memory_order_acquire)) return -1;
    FrameSourceHeader *hdr = (FrameSourceHeader *)g_map_base;
    return reinterpret_cast<std::atomic<int32_t>*>(&hdr->chroma_override)
               ->load(std::memory_order_acquire);
}

void frame_source_set_chroma_override(int32_t override_is_nv21) {
    if (!g_initialized.load(std::memory_order_acquire)) return;
    const int32_t v = (override_is_nv21 > 1) ? 1 : (override_is_nv21 < 0 ? -1 : override_is_nv21);
    FrameSourceHeader *hdr = (FrameSourceHeader *)g_map_base;
    reinterpret_cast<std::atomic<int32_t>*>(&hdr->chroma_override)
        ->store(v, std::memory_order_release);
}


#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

extern "C" void gst_android_load_gio_modules(void) __attribute__((weak));

#include <android/log.h>
#include <android/sharedmem.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <atomic>
#include <thread>
#include <vector>
#include <string.h>
#include <cstring>
#include <errno.h>
#include <jni.h>
#include <stdint.h>
#include <inttypes.h>
#include <chrono>
#include <string>
#include <algorithm>
#include <cctype>

#define TAG "amkush/frame_producer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)

#define FRAME_PRODUCER_VERSION "AMKUSH.70-GZDROPBOX-20260914"

// FRAME_BUILD_ID — the exact git branch + commit SHA this libframe_producer.so
// was built from, injected by the CI workflow. Baked in at compile time so it
// cannot lie. Logged in a banner at startup and in the rotation log, so the
// on-device logcat proves whether the APK is the fresh gstreamer.3 build.
#ifndef FRAME_BUILD_ID
#define FRAME_BUILD_ID "dev-unknown"
#endif

#define FRAME_SOURCE_MAGIC   0xAB7C5801U
#define FRAME_RING_SLOTS     3
#define AMKUSH_SOCKET_NAME   "\0amkush_frame_fd"

static const int MASTER_WIDTH  = 1280;
static const int MASTER_HEIGHT = 720;

typedef struct {
    uint32_t              magic;
    std::atomic<uint32_t> write_slot;
    std::atomic<uint32_t> read_slot;
    uint32_t              master_width;
    uint32_t              master_height;
    uint32_t              slot_stride;
    uint32_t              slot_format;
    std::atomic<int32_t>  pan_x;
    std::atomic<int32_t>  pan_y;
    std::atomic<uint32_t> scale_q16;
    std::atomic<uint32_t> source_rotation;
    std::atomic<uint32_t> manual_rotation;
    std::atomic<int32_t>  chroma_override; /* -1 unset, 0 NV12, 1 NV21 ([gstreamer.4]) */
    std::atomic<uint32_t> frame_seq;       /* [V26] bump per ring write */
    std::atomic<uint32_t> heartbeat_ms32;  /* [V27] producer liveness */
    uint8_t               _pad[4];
} FrameSourceHeader;

static_assert(sizeof(FrameSourceHeader) == 64, "FrameSourceHeader must be 64 bytes");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "std::atomic<uint32_t> must be lock-free for shared-memory IPC");

static int         g_ashmem_fd   = -1;
static void       *g_map_base    = MAP_FAILED;
static size_t      g_map_size    = 0;
static uint8_t    *g_slots_base  = nullptr;
static uint32_t    g_slot_stride = 0;
static pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;

/* [V27] liveness heartbeat: while the producer pipeline is up, refresh
 * header->heartbeat_ms32 twice a second from a detached thread. When the app
 * dies the thread dies with it, the stamp goes stale, and the cameraserver
 * hook stops injecting the frozen last frame (real camera passes through). */
static std::atomic<bool>                g_hb_run{false};
static std::thread                      g_hb_thr;
/* [V28 CRASH] 13.unisoc.1: the ring can still be UNMAPPED when the pipeline
 * reaches PLAYING — live/RTSP sources create it lazily once real dimensions
 * are known (ensureProducerRing), and dimension changes recreate it. V27
 * stored the raw g_map_base (== MAP_FAILED) here and the thread's first
 * store faulted: SIGBUS BUS_ADRALN @ MAP_FAILED+0x38 (0x38 = offset of
 * heartbeat_ms32), killing the whole app on every inject start. The thread
 * now re-reads this pointer each tick and skips the store while the ring is
 * not mapped; create_ashmem_ring() binds it as soon as the ring exists. */
static std::atomic<FrameSourceHeader *> g_hb_hdr{(FrameSourceHeader *)MAP_FAILED};
/* [V66 SLOT-BRIDGE] Previous generation's ring kept mapped AND heartbeat-fed
 * while nativeStart rebuilds the pipeline for a slot switch (S1/S2). The
 * cameraserver hook may still be mapped to the old ring; keeping its
 * heartbeat fresh holds live() true, so the hook serves the last frame of
 * the old slot (frozen) instead of falling through to the REAL camera
 * during the rebuild+IPC-handover window (>2.5s on slow devices — seen on
 * TECNO BG6: "switch slot -> suddenly real frames"). Retired when the next
 * generation replaces it, or fully on explicit stop. */
static std::atomic<FrameSourceHeader *> g_hb_prev_hdr{nullptr};
static void  *g_prev_map       = MAP_FAILED;
static size_t g_prev_map_size  = 0;
static int    g_prev_ashmem_fd = -1;
static void heartbeat_start(FrameSourceHeader *hdr) {
    g_hb_hdr.store(hdr ? hdr : (FrameSourceHeader *)MAP_FAILED,
                   std::memory_order_release);
    if (g_hb_run.exchange(true)) return;
    g_hb_thr = std::thread([]{
        while (g_hb_run.load(std::memory_order_relaxed)) {
            struct timespec ts; clock_gettime(CLOCK_BOOTTIME, &ts);
            uint32_t now = (uint32_t)((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
            FrameSourceHeader *h = g_hb_hdr.load(std::memory_order_acquire);
            if (h && h != (FrameSourceHeader *)MAP_FAILED)
                h->heartbeat_ms32.store(now, std::memory_order_release);
            /* [V66] keep the retired ring's heartbeat fresh too, until the
             * hook hands over to the new ring (or the producer stops). */
            FrameSourceHeader *pv = g_hb_prev_hdr.load(std::memory_order_acquire);
            if (pv && pv != (FrameSourceHeader *)MAP_FAILED && pv != h)
                pv->heartbeat_ms32.store(now, std::memory_order_release);
            usleep(400 * 1000);
        }
    });
}
static void heartbeat_stop() {
    if (!g_hb_run.exchange(false)) return;
    if (g_hb_thr.joinable()) g_hb_thr.join();
}
static std::atomic<bool> g_ring_ready{false};

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

static int create_ashmem_ring(int w, int h) {
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        LOGE("Invalid dimensions: %dx%d", w, h);
        return -1;
    }
    uint32_t stride = (uint32_t)w;
    uint32_t fmt = 0;
    uint64_t slot_sz_64 = (uint64_t)compute_slot_size(stride, (uint32_t)h, fmt);
    uint64_t total_64   = (uint64_t)sizeof(FrameSourceHeader) + slot_sz_64 * FRAME_RING_SLOTS;
    if (slot_sz_64 > (uint64_t)SIZE_MAX || total_64 > (uint64_t)SIZE_MAX) {
        LOGE("Buffer size overflow: slot=%llu total=%llu", (unsigned long long)slot_sz_64, (unsigned long long)total_64);
        return -1;
    }
    size_t slot_sz = (size_t)slot_sz_64;
    size_t total   = (size_t)total_64;
    int fd = ASharedMemory_create("amkush_ring", total);
    if (fd < 0) {
        LOGE("ASharedMemory_create failed: %s", strerror(errno));
        return -1;
    }
    void *base = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        LOGE("mmap failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    FrameSourceHeader *hdr = (FrameSourceHeader *)base;
    hdr->magic         = FRAME_SOURCE_MAGIC;
    hdr->write_slot.store(0, std::memory_order_release);
    hdr->read_slot.store(0, std::memory_order_release);
    hdr->master_width  = (uint32_t)w;
    hdr->master_height = (uint32_t)h;
    hdr->slot_stride   = stride;
    hdr->slot_format   = fmt;
    hdr->pan_x.store(0,      std::memory_order_release);
    hdr->pan_y.store(0,      std::memory_order_release);
    hdr->scale_q16.store(65536u, std::memory_order_release);
    hdr->frame_seq.store(0u, std::memory_order_release);
    hdr->heartbeat_ms32.store(0u, std::memory_order_release);
    hdr->source_rotation.store(0u, std::memory_order_release);
    hdr->manual_rotation.store(0u, std::memory_order_release);
    hdr->chroma_override.store(-1, std::memory_order_release); /* unset -> build default */
    g_ashmem_fd   = fd;
    g_map_base    = base;
    g_map_size    = total;
    g_slots_base  = (uint8_t *)base + sizeof(FrameSourceHeader);
    g_slot_stride = stride;
    g_ring_ready.store(true, std::memory_order_release);
    g_hb_hdr.store(hdr, std::memory_order_release); /* [V28] bind heartbeat to the (re)created ring */
    LOGI("Ashmem ring created: %dx%d stride=%u format=%u slot_sz=%zu total=%zu fd=%d",
         w, h, stride, fmt, slot_sz, total, fd);
    return fd;
}

static void rotate_nv12_90cw(
    const uint8_t *src_y, int src_w, int src_h,
    uint8_t *dst_y)
{
    for (int y = 0; y < src_h; y++) {
        for (int x = 0; x < src_w; x++) {
            int dst_x = src_h - 1 - y;
            int dst_y_coord = x;
            dst_y[dst_y_coord * src_h + dst_x] = src_y[y * src_w + x];
        }
    }
}

static void rotate_nv12_90cw_uv(
    const uint8_t *src_uv, int src_w, int src_h,
    uint8_t *dst_uv)
{
    int uvW = src_w / 2, uvH = src_h / 2;
    int dst_uvW = uvH;
    for (int y = 0; y < uvH; y++) {
        for (int x = 0; x < uvW; x++) {
            int dst_x = uvH - 1 - y;
            int dst_y_coord = x;
            dst_uv[(dst_y_coord * dst_uvW + dst_x) * 2]     = src_uv[(y * uvW + x) * 2];
            dst_uv[(dst_y_coord * dst_uvW + dst_x) * 2 + 1] = src_uv[(y * uvW + x) * 2 + 1];
        }
    }
}

static void rotate_nv12_180(
    const uint8_t *src_y, const uint8_t *src_uv,
    int src_w, int src_h,
    uint8_t *dst_y, uint8_t *dst_uv)
{
    for (int y = 0; y < src_h; y++) {
        for (int x = 0; x < src_w; x++) {
            dst_y[(src_h - 1 - y) * src_w + (src_w - 1 - x)] = src_y[y * src_w + x];
        }
    }
    int uvW = src_w / 2, uvH = src_h / 2;
    for (int y = 0; y < uvH; y++) {
        for (int x = 0; x < uvW; x++) {
            dst_uv[(uvH - 1 - y) * uvW * 2 + (uvW - 1 - x) * 2]     = src_uv[y * uvW * 2 + x * 2];
            dst_uv[(uvH - 1 - y) * uvW * 2 + (uvW - 1 - x) * 2 + 1] = src_uv[y * uvW * 2 + x * 2 + 1];
        }
    }
}

static std::vector<uint8_t> g_rot_tmp_y;
static std::vector<uint8_t> g_rot_tmp_uv;

static void ring_write(const uint8_t *y_src, int y_stride,
                        const uint8_t *uv_src, int uv_stride,
                        int w, int h) {
    FrameSourceHeader *hdr = (FrameSourceHeader *)g_map_base;
    if (!hdr || !g_slots_base) return;
    uint32_t slot_stride = hdr->slot_stride;
    uint32_t slot_h      = hdr->master_height;
    if (w <= 0 || h <= 0 || w > (int)slot_stride || h > (int)slot_h) {
        LOGE("ring_write: invalid dimensions %dx%d (slot=%ux%u stride=%u)",
             w, h, hdr->master_width, slot_h, slot_stride);
        return;
    }
    if (!y_src || !uv_src) {
        LOGE("ring_write: null source planes");
        return;
    }
    if (y_stride <= 0 || uv_stride <= 0) {
        LOGE("ring_write: invalid strides Y=%d UV=%d", y_stride, uv_stride);
        return;
    }
    uint32_t current_write = hdr->write_slot.load(std::memory_order_relaxed);
    uint32_t read_slot     = hdr->read_slot.load(std::memory_order_acquire);
    uint32_t next = current_write + 1;
    if (next - read_slot > FRAME_RING_SLOTS) {
        hdr->read_slot.fetch_add(1, std::memory_order_acq_rel);
    }
    uint32_t write_idx = next % FRAME_RING_SLOTS;
    size_t slot_sz = compute_slot_size(slot_stride, slot_h, hdr->slot_format);
    uint8_t *dst_y   = g_slots_base + write_idx * slot_sz;
    uint8_t *dst_uv  = dst_y + (size_t)slot_stride * slot_h;
    uint8_t *slot_end = dst_y + slot_sz;
    uint8_t *map_end  = (uint8_t *)g_map_base + g_map_size;
    if (slot_end > map_end) {
        LOGE("ring_write: slot %u out of mapped bounds", write_idx);
        return;
    }
    for (int row = 0; row < h; row++) {
        memcpy(dst_y + row * slot_stride, y_src + row * y_stride, (size_t)w);
        if ((int)slot_stride > w) {
            memset(dst_y + row * slot_stride + w, 16, slot_stride - w);
        }
    }
    for (int row = 0; row < h / 2; row++) {
        memcpy(dst_uv + row * slot_stride, uv_src + row * uv_stride, (size_t)w);
        if ((int)slot_stride > w) {
            memset(dst_uv + row * slot_stride + w, 128, slot_stride - w);
        }
    }
    // Clear any remaining rows if h < slot_h (letterbox bottom)
    if ((int)slot_h > h) {
        for (int row = h; row < (int)slot_h; row++) {
            memset(dst_y + row * slot_stride, 16, slot_stride);
        }
        for (int row = h/2; row < (int)slot_h/2; row++) {
            memset(dst_uv + row * slot_stride, 128, slot_stride);
        }
    }
    hdr->write_slot.store(next, std::memory_order_release);
    hdr->frame_seq.fetch_add(1u, std::memory_order_release); /* [V26] */
}

static int send_fd_to_hook(int fd) {
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, AMKUSH_SOCKET_NAME, sizeof(AMKUSH_SOCKET_NAME) - 1);
    const socklen_t addr_len = static_cast<socklen_t>(
        offsetof(struct sockaddr_un, sun_path) + sizeof(AMKUSH_SOCKET_NAME) - 1);

    char buf[1] = {0};
    struct iovec iov = { buf, 1 };
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));

    // cameraserver can finish installing the hook after the app has already
    // decoded its first frame. Keep the ashmem fd alive and retry the abstract
    // socket instead of dropping the only handshake attempt.
    constexpr int kMaxAttempts = 120;
    constexpr useconds_t kRetryDelayUs = 100000; // 100 ms, up to 12 seconds
    int last_errno = 0;
    int connect_errno = 0, send_errno = 0; // [V16] diagnostics: which phase fails
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) {
            last_errno = errno;
        } else if (connect(sock, reinterpret_cast<struct sockaddr *>(&addr), addr_len) < 0) {
            last_errno = connect_errno = errno;
            close(sock);
        } else {
            const ssize_t n = sendmsg(sock, &msg, MSG_NOSIGNAL);
            if (n > 0) {
                close(sock);
                LOGI("Sent Ashmem fd=%d to cameraserver hook after %d attempt(s)", fd, attempt);
                return 0;
            }
            last_errno = send_errno = errno;
            close(sock);
        }

        if (attempt < kMaxAttempts) usleep(kRetryDelayUs);
    }

    LOGE("Could not send Ashmem fd=%d after %d attempts: %s (connect=%s sendmsg=%s)",
         fd, kMaxAttempts, strerror(last_errno),
         connect_errno ? strerror(connect_errno) : "n/a",
         send_errno ? strerror(send_errno) : "n/a");
    return -1;
}


static std::atomic_bool g_running{false};
static std::atomic_bool g_paused{false};
static pthread_t g_thread;
static bool g_thread_valid = false;
static char g_source[4096];
static GstElement* g_pipeline = nullptr;
static GstElement* g_source_element = nullptr;
static GstElement* g_video_queue = nullptr;
static GstElement* g_video_convert = nullptr;
static GstElement* g_video_scale = nullptr;
static GstElement* g_video_caps = nullptr;
static GstElement* g_video_sink = nullptr;
static bool g_video_linked = false;
static bool g_static_source = false;
static bool g_live_source = false;
static int g_ring_width = 0;
static int g_ring_height = 0;
static GstClockTime g_last_pts = GST_CLOCK_TIME_NONE;
static std::vector<uint8_t> g_static_y;
static std::vector<uint8_t> g_static_uv;
static int g_static_y_stride = 0;
static int g_static_uv_stride = 0;
static uint64_t g_local_frame_count = 0;
// [gstreamer.4] Freeze watchdog: wall-clock time (us, monotonic) of the last sample
// actually written to the ring. If a live source goes this long without a new frame
// we treat it as a stalled pipeline and force a rebuild/reconnect.
static std::atomic<int64_t> g_last_frame_us{0};
static const int64_t kLiveWatchdogUs = 2500000;  // 2.5s of no samples on a live source
/* [V23] First-frame grace per pipeline generation: OBS-style encoders emit a
 * keyframe every ~2s, so after PLAY the first decodable frame can legitimately
 * take 2s (keyframe wait) + handshake/jitter/decode. A 2.5s watchdog killed
 * healthy sessions before their first frame (13.unisoc.1: permanent rebuild
 * loop, zero bus errors) — give first-frame 8s, keep 2.5s once flowing. */
static const int64_t kFirstFrameWatchdogUs = 8000000;

static bool hasPrefix(const std::string& value, const char* prefix) {
    return value.size() >= std::strlen(prefix) && value.compare(0, std::strlen(prefix), prefix) == 0;
}

static bool hasSuffix(const std::string& value, const char* suffix) {
    const size_t n = std::strlen(suffix);
    return value.size() >= n && value.compare(value.size() - n, n, suffix) == 0;
}

static std::string lowerCopy(std::string value) {
    for (char& c : value)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}

static std::string producerUri() {
    std::string raw(g_source);
    if (raw.empty() || raw.find("://") != std::string::npos) return raw;
    if (raw[0] == '/') {
        gchar* uri = gst_filename_to_uri(raw.c_str(), nullptr);
        if (!uri) return raw;
        std::string result(uri);
        g_free(uri);
        return result;
    }
    return raw;
}

static bool isLiveSource(const std::string& uri) {
    std::string scheme;
    const auto sep = uri.find("://");
    if (sep != std::string::npos) scheme = lowerCopy(uri.substr(0, sep));
    return scheme == "rtsp" || scheme == "rtsps" || scheme == "rtmp" ||
           scheme == "rtmps" || scheme == "udp" || scheme == "rtp" ||
           scheme == "srt" || scheme == "tcp" || scheme == "http" ||
           scheme == "https";
}

static bool isStaticImage(const std::string& uri) {
    std::string path = lowerCopy(uri);
    const auto query = path.find_first_of("?#");
    if (query != std::string::npos) path.resize(query);
    return path.size() >= 4 &&
           (hasSuffix(path, ".jpg") || hasSuffix(path, ".jpeg") ||
            hasSuffix(path, ".png") || hasSuffix(path, ".bmp") ||
            hasSuffix(path, ".webp") || hasSuffix(path, ".tif") ||
            hasSuffix(path, ".tiff"));
}

static jlong producerPtsUs(GstClockTime pts) {
    return GST_CLOCK_TIME_IS_VALID(pts) ? static_cast<jlong>(GST_TIME_AS_USECONDS(pts)) : 0;
}

/* [V23 TCP] After consecutive frameless rebuilds, force RTP-over-TCP
 * (interleaved). The 13.unisoc.1 V22 log showed the terminal stall: RTSP
 * handshake/PLAY kept succeeding (zero bus errors) but no RTP ever arrived
 * — the classic silent UDP session death (zombie sessions/NAT). TCP cannot
 * stall that way. Reset to default transports once frames flow again. */
/* [V54 RTSP stall fix] TCP-FIRST: the user's OBS->RTSP injection froze for
 * 2-3 s then jumped ahead — classic UDP packet loss: the H.264 decoder must
 * wait for OBS's next keyframe (2 s default interval) after ANY lost packet.
 * The watchdog only forced TCP after a stall was detected; start on TCP
 * interleaved instead — zero loss on LAN/WiFi, no keyframe waits, no stalls. */
static std::atomic<bool> g_force_rtsp_tcp{true};

static void configureProducerSource(GstElement* source) {
    GstElementFactory* factory = gst_element_get_factory(source);
    const char* name = factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)) : "";
    if (name && std::strcmp(name, "rtspsrc") == 0) {
        // Freeze fix (gstreamer.4): the previous live config set latency=0 with
        // do-retransmission=FALSE, so on any RTSP packet loss the source could not
        // retransmit and the decode starved -> the producer stopped writing the ring
        // (freeze) even though OBS kept playing the same stream. Give rtspsrc a modest
        // jitter buffer and ALLOW retransmission so a lost packet can be recovered
        // instead of stalling the pipeline. drop-on-latency stays on so we never build a
        // large backlog (keeps the source near-real-time).
        g_object_set(source,
                     "latency", 200u,              // 200ms jitter buffer -> room to retransmit
                     "drop-on-latency", TRUE,
                     "do-retransmission", TRUE,    // recover lost packets instead of starving
                     "tcp-timeout", (guint64)3000000000,  // [V18] 3 s TCP connect/tx timeout
                     "timeout",        (guint64)5000000000, // [V18] 5 s UDP timeout
                     nullptr);
        if (g_force_rtsp_tcp.load(std::memory_order_acquire)) {
            /* GST_RTSP_LOWER_TRANS_TCP = 0x4 — RTP interleaved in the RTSP
             * TCP connection; immune to the UDP black-hole stall. */
            g_object_set(source, "protocols", 0x4u, nullptr);
            LOGI("Media RTSP source configured: [V23] transport FORCED TCP (interleaved)");
        }
        LOGI("Media RTSP source configured: latency=200ms, retransmit=TRUE, tcp-timeout=3s (V18)");
    }
}

static void setProducerCaps(int width, int height) {
    if (!g_video_caps || width <= 0 || height <= 0) return;
    const bool portrait = height > width;
    const double aspect = static_cast<double>(width) / static_cast<double>(height);
    // [gstreamer.4 quality] Feed the injector at the SOURCE's native resolution
    // (capped at 1920 on the long side) instead of forcing 720p. The FaceTec/browser
    // streams we inject into are 1080p (1920x1080); delivering only a 720p ring forced
    // a ~2.7x libyuv upscale that softens/blurs the face. Preserving the source
    // resolution means the injector does far less (or no) upscaling, so the injected
    // selfie stays sharp. We never upscale past the source, only downscale if the
    // source exceeds the 1920 cap.
    const int kMaxLong = 1920;
    int dstW, dstH;
    if (portrait) {
        dstH = std::min(height, kMaxLong);
        dstW = static_cast<int>(static_cast<double>(dstH) * aspect);
    } else {
        dstW = std::min(width, kMaxLong);
        dstH = static_cast<int>(static_cast<double>(dstW) / aspect);
    }
    dstW = std::max(2, (dstW + 1) & ~1);
    dstH = std::max(2, (dstH + 1) & ~1);
    g_ring_width = dstW;
    g_ring_height = dstH;
    LOGI("[%s] producer caps: src=%dx%d -> ring %dx%d (portrait=%d)", FRAME_PRODUCER_VERSION, width, height, dstW, dstH, portrait ? 1 : 0);

    GstCaps* caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "NV12",
        "width", G_TYPE_INT, dstW,
        "height", G_TYPE_INT, dstH,
        nullptr);
    g_object_set(g_video_caps, "caps", caps, nullptr);
    gst_caps_unref(caps);
}

static void ensureProducerRing() {
    if (g_ring_ready.load(std::memory_order_acquire) || g_ring_width <= 0 || g_ring_height <= 0)
        return;
    if (create_ashmem_ring(g_ring_width, g_ring_height) < 0) {
        LOGE("create_ashmem_ring failed for %dx%d", g_ring_width, g_ring_height);
        return;
    }
    g_ring_ready.store(true, std::memory_order_release);
}

static std::atomic<bool> g_hook_fed{false};

/* [V13 RTSP] Feed the cameraserver hook as soon as the ring exists. For live
 * sources that happens asynchronously (when the RTSP connect finally lands),
 * letting nativeStart return success immediately instead of false-failing
 * after the 10 s window (13.unisoc.3: "failed" toast on save+start). */
static void maybe_feed_hook() {
    if (g_hook_fed.load(std::memory_order_acquire)) return;
    if (g_ring_ready.load(std::memory_order_acquire) && g_ashmem_fd >= 0) {
        if (send_fd_to_hook(g_ashmem_fd) == 0) {
            g_hook_fed.store(true, std::memory_order_release);
            LOGI("[build=%s] cameraserver hook fed — injection live", FRAME_BUILD_ID);
        } else {
            LOGW("send_fd_to_hook failed — will retry on next pad/sample");
        }
    }
}

static void onProducerSourceSetup(GstElement*, GstElement* source, gpointer) {
    configureProducerSource(source);
}

/* [V21 CRASH] A14 crash at gst_element_link_pads_full (fault addr
 * 0xaaaaaaaaaaaaaaaa = freed element) from libframe_producer: a late
 * "pad-added" signal from the OLD uridecodebin raced pipeline teardown and
 * linked against the freed video-queue. Guard the callback with a pipeline
 * generation counter and a parent-element identity check. */
static std::atomic<uint64_t> g_pipeline_gen{0};

static void onProducerPadAdded(GstElement*, GstPad* pad, gpointer) {
    if (!g_video_queue) return;
    const uint64_t gen_at_entry = g_pipeline_gen.load(std::memory_order_acquire);
    GstElement *pad_parent = gst_pad_get_parent_element(pad);
    const bool parent_current = pad_parent && pad_parent == g_source_element;
    if (pad_parent) gst_object_unref(pad_parent);
    if (!parent_current ||
        gen_at_entry != g_pipeline_gen.load(std::memory_order_acquire)) {
        LOGW("onProducerPadAdded: stale callback (gen=%llu) — not linking",
             (unsigned long long)gen_at_entry);
        return;
    }
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    if (!caps || gst_caps_is_empty(caps)) {
        if (caps) gst_caps_unref(caps);
        return;
    }
    GstStructure* structure = gst_caps_get_structure(caps, 0);
    const char* name = structure ? gst_structure_get_name(structure) : "";
    if (name && g_str_has_prefix(name, "video/")) {
        int width = 0;
        int height = 0;
        gst_structure_get_int(structure, "width", &width);
        gst_structure_get_int(structure, "height", &height);
        setProducerCaps(width, height);
        if (g_map_base != MAP_FAILED && g_map_base != nullptr) {
            auto* header = static_cast<FrameSourceHeader*>(g_map_base);
            // Rotation fix: the injected live composite arrives already upright
            // (OBS decodes the same RTSP stream directly and shows it upright), so
            // the phone must display it at 0° to match OBS. The old heuristic forced
            // 90° for any portrait live source, which tipped the composite sideways on
            // the phone. Honor a manual rotation set by the user; otherwise keep the
            // source upright. Local/static media keeps its legacy 180° fallback.
            // [V11 ROT] per-source upright defaults, from fresh-build evidence:
            //  - static media (OPPO CPH2387, 14.mediatek.1, b842ed8): already
            //    upright → total 0; the legacy 180 deg static default rendered the
            //    preview upside-down (only manual 180 fixed it).
            //  - live RTSP (TECNO BG6, 13.unisoc.1, 235da71): chrome/FaceTec showed
            //    the composite UPSIDE-DOWN at total 0 → the live feed needs 180.
            // Manual rotation via the overlay still adds on top for exceptions.
            const uint32_t rotation = g_live_source ? 180u : 0u;
            header->source_rotation.store(rotation, std::memory_order_release);
            LOGI("[build=%s][%s] source_rotation set to %u (live=%d src=%dx%d)",
                 FRAME_BUILD_ID, FRAME_PRODUCER_VERSION,
                 rotation, g_live_source ? 1 : 0, width, height);
        }
        GstPad* sinkPad = gst_element_get_static_pad(g_video_queue, "sink");
        if (sinkPad && !gst_pad_is_linked(sinkPad)) {
            const GstPadLinkReturn result = gst_pad_link(pad, sinkPad);
            g_video_linked = result == GST_PAD_LINK_OK;
            if (!g_video_linked)
                LOGE("Could not link decoded video pad: %s", gst_pad_link_get_name(result));
        }
        if (sinkPad) gst_object_unref(sinkPad);
        ensureProducerRing();
        maybe_feed_hook();
    }
    gst_caps_unref(caps);
}

static GstFlowReturn onProducerSample(GstAppSink* sink, gpointer) {
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample || !g_running.load(std::memory_order_acquire)) {
        if (sample) gst_sample_unref(sample);
        return GST_FLOW_FLUSHING;
    }

    GstCaps* caps = gst_sample_get_caps(sample);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstVideoInfo info;
    gst_video_info_init(&info);
    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps) ||
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_NV12) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    ensureProducerRing();
    maybe_feed_hook();
    const bool paused = g_paused.load(std::memory_order_acquire);
    GstVideoFrame frame;
    if (!g_ring_ready.load(std::memory_order_acquire) ||
        !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    const int width = GST_VIDEO_INFO_WIDTH(&info);
    const int height = GST_VIDEO_INFO_HEIGHT(&info);
    const uint8_t* y = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    const uint8_t* uv = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
    const int yStride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    const int uvStride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);

    ++g_local_frame_count;
    if (!g_live_source) {
        const GstClockTime pts = GST_BUFFER_PTS(buffer);
        if (GST_CLOCK_TIME_IS_VALID(pts) && GST_CLOCK_TIME_IS_VALID(g_last_pts) && pts > g_last_pts) {
            const GstClockTime delta = pts - g_last_pts;
            if (delta < GST_SECOND) g_usleep(static_cast<gulong>(delta / 1000));
        } else if (!GST_CLOCK_TIME_IS_VALID(g_last_pts)) {
            g_usleep(33333);
        }
        g_last_pts = pts;
    }

    // Lag instrumentation: log the timestamp of the frame being written so the
    // on-device lag (phone vs OBS) can be quantified, not just eyeballed.
    // [gstreamer.4] Also stamp g_last_frame_us (every frame) for the watchdog.
    const int64_t nowUs = gst_util_get_timestamp() / 1000;  // monotonic us
    g_last_frame_us.store(nowUs, std::memory_order_release);
    if (g_live_source && (g_local_frame_count % 30 == 1)) {
        const GstClockTime bpts = GST_BUFFER_PTS(buffer);
        LOGI("[%s] ring_write #%d w=%d h=%d ptsUs=%lld nowUs=%lld", FRAME_PRODUCER_VERSION,
             g_local_frame_count, width, height,
             GST_CLOCK_TIME_IS_VALID(bpts) ? (long long)GST_TIME_AS_USECONDS(bpts) : -1,
             (long long)nowUs);
    }

    if (!paused) ring_write(y, yStride, uv, uvStride, width, height);
    if (g_static_source || (!g_live_source && g_local_frame_count == 1)) {
        const int uvHeight = (height + 1) / 2;
        g_static_y_stride = yStride;
        g_static_uv_stride = uvStride;
        g_static_y.resize(static_cast<size_t>(yStride) * height);
        g_static_uv.resize(static_cast<size_t>(uvStride) * uvHeight);
        for (int row = 0; row < height; ++row)
            std::memcpy(g_static_y.data() + static_cast<size_t>(row) * yStride,
                        y + static_cast<size_t>(row) * yStride, yStride);
        for (int row = 0; row < uvHeight; ++row)
            std::memcpy(g_static_uv.data() + static_cast<size_t>(row) * uvStride,
                        uv + static_cast<size_t>(row) * uvStride, uvStride);
    }
    gst_video_frame_unmap(&frame);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static bool buildProducerPipeline() {
    g_pipeline = gst_pipeline_new("ecomcam-gstreamer-producer");
    g_source_element = gst_element_factory_make("uridecodebin", "source");
    g_video_queue = gst_element_factory_make("queue", "video-queue");
    g_video_convert = gst_element_factory_make("videoconvert", "video-convert");
    g_video_scale = gst_element_factory_make("videoscale", "video-scale");
    g_video_caps = gst_element_factory_make("capsfilter", "video-caps");
    g_video_sink = gst_element_factory_make("appsink", "video-sink");
    if (!g_pipeline || !g_source_element || !g_video_queue || !g_video_convert ||
        !g_video_scale || !g_video_caps || !g_video_sink) {
        LOGE("Amkush producer element creation failed");
        return false;
    }

    const std::string uri = producerUri();
    g_live_source = isLiveSource(uri);
    // Keep static-image detection independent from transport: an HTTP or
    // content-provider image must still enter the cached 30 fps loop at EOS.
    g_static_source = isStaticImage(uri);
    g_last_pts = GST_CLOCK_TIME_NONE;
    g_static_y.clear();
    g_static_uv.clear();
    g_local_frame_count = 0;
    g_video_linked = false;
    g_object_set(g_source_element, "uri", uri.c_str(), nullptr);
    // [V12 PACE] sync=FALSE let LOCAL files decode as fast as the SoC allows;
    // on the mt6765 that is slower than real time, so the injected content ran
    // in slow motion (14.mediatek.2 @ 235da71) and starved the injector thread
    // (1-2 s inject stalls). Pace local media to real time (sync=TRUE); live
    // RTSP stays unsynced for low latency (drop+max-buffers keep it current).
    g_object_set(g_video_sink,
                 "emit-signals", TRUE, "sync", g_live_source ? FALSE : TRUE,
                 "max-buffers", 1u, "drop", TRUE, nullptr);
    LOGI("[build=%s][%s] appsink sync=%s (live=%d)", FRAME_BUILD_ID,
         FRAME_PRODUCER_VERSION, g_live_source ? "FALSE" : "TRUE",
         g_live_source ? 1 : 0);

    // Lag fix (gstreamer.3): the default GStreamer queue buffers up to 200 buffers, which for a
    // live source adds a large fixed latency (OBS decodes directly so it stays
    // ahead of the phone). Cap it to the latest buffer and leak toward the sink so
    // the pipeline always delivers the newest decoded frame (drop-oldest).
    //
    // Freeze fix (gstreamer.4): max-size-buffers=1 removed ALL resilience -- on the
    // tiniest RTSP hiccup the single buffer was dropped and the pipeline had nothing
    // to deliver, so the ring wrote nothing and the phone froze. A small (3-buffer)
    // leaky queue keeps latency low but lets the pipeline ride out a hiccup; the
    // watchdog below additionally rebuilds the pipeline if a live source stalls.
    if (g_live_source) {
        g_object_set(g_video_queue,
                     "max-size-buffers", 3u,
                     "max-size-bytes", 0u,
                     "max-size-time", 0u,
                     "leaky", 2 /* GstQueueLeaky::downstream */,
                     nullptr);
        LOGI("[build=%s][%s] video-queue set to low-latency resilient (max-size-buffers=3, leaky=downstream)",
             FRAME_BUILD_ID, FRAME_PRODUCER_VERSION);
    }

    gst_bin_add_many(GST_BIN(g_pipeline), g_source_element, g_video_queue,
                     g_video_convert, g_video_scale, g_video_caps, g_video_sink, nullptr);
    if (!gst_element_link_many(g_video_queue, g_video_convert, g_video_scale,
                               g_video_caps, g_video_sink, nullptr)) {
        LOGE("Amkush producer branch linking failed");
        return false;
    }
    g_pipeline_gen.fetch_add(1, std::memory_order_release);
    g_signal_connect(g_source_element, "pad-added", G_CALLBACK(onProducerPadAdded), nullptr);
    g_signal_connect(g_source_element, "source-setup", G_CALLBACK(onProducerSourceSetup), nullptr);
    g_signal_connect(g_video_sink, "new-sample", G_CALLBACK(onProducerSample), nullptr);
    if (gst_element_set_state(g_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        LOGE("Media producer failed to enter PLAYING for %s", uri.c_str());
        return false;
    }
    /* [V22 WATCHDOG] Reset the freeze-watchdog clock when a NEW pipeline
     * starts. The old code kept g_last_frame_us from the PREVIOUS pipeline,
     * so every rebuild was instantly judged stale (elapsed grew 2.6s ->
     * 1232s across 4335 rebuilds on 13.unisoc.1) and the producer
     * restart-stormed every ~400ms forever, hammering the RTSP server
     * (404s) and never recovering — the ring kept the last frame, which the
     * phone injected in a loop ("stuck on 1 frame"). */
    g_last_frame_us.store((int64_t)(gst_util_get_timestamp() / 1000),
                          std::memory_order_release);
    LOGI("Media producer pipeline running uri=%s live=%d static=%d",
         uri.c_str(), g_live_source ? 1 : 0, g_static_source ? 1 : 0);
    heartbeat_start((FrameSourceHeader *)g_map_base); /* [V27] */
    return true;
}

static void destroyProducerPipeline() {
    /* [V21 CRASH] bump the generation and disconnect signal handlers BEFORE
     * any state change so no callback can observe half-torn-down globals. */
    g_pipeline_gen.fetch_add(1, std::memory_order_release);
    if (g_source_element) {
        g_signal_handlers_disconnect_by_func(g_source_element, (gpointer)onProducerPadAdded, nullptr);
        g_signal_handlers_disconnect_by_func(g_source_element, (gpointer)onProducerSourceSetup, nullptr);
    }
    if (g_video_sink)
        g_signal_handlers_disconnect_by_func(g_video_sink, (gpointer)onProducerSample, nullptr);
    if (g_pipeline) {
        gst_element_set_state(g_pipeline, GST_STATE_NULL);
        gst_element_get_state(g_pipeline, nullptr, nullptr, 2 * GST_SECOND);
        gst_object_unref(g_pipeline);
    }
    g_pipeline = nullptr;
    g_source_element = nullptr;
    g_video_queue = nullptr;
    g_video_convert = nullptr;
    g_video_scale = nullptr;
    g_video_caps = nullptr;
    g_video_sink = nullptr;
    g_video_linked = false;
}

static void pushStaticFrameLoop() {
    if (g_static_y.empty() || g_static_uv.empty()) return;
    const int yStride = g_static_y_stride > 0 ? g_static_y_stride : g_ring_width;
    const int uvStride = g_static_uv_stride > 0 ? g_static_uv_stride : g_ring_width;
    while (g_running.load(std::memory_order_acquire)) {
        if (!g_paused.load(std::memory_order_acquire)) {
            ring_write(g_static_y.data(), yStride, g_static_uv.data(), uvStride,
                       g_ring_width, g_ring_height);
        }
        g_usleep(33333);
    }
}

static void* decode_thread(void*) {
    gst_init(nullptr, nullptr);
    if (gst_android_load_gio_modules) gst_android_load_gio_modules();

    int wd_backoff_ms = 0;   /* [V22 WATCHDOG] restart backoff state */
    int unhealthy_gens = 0;  /* [V24 TCP] consecutive short-lived generations */
    bool tcp_proven = false; /* [V24 TCP] TCP delivered a healthy gen — never go back to UDP */
    while (g_running.load(std::memory_order_acquire)) {
        if (!buildProducerPipeline()) {
            destroyProducerPipeline();
            if (g_running.load(std::memory_order_acquire)) g_usleep(500000);
            continue;
        }

        const int64_t built_us = (int64_t)(gst_util_get_timestamp() / 1000);
        GstBus* bus = gst_element_get_bus(g_pipeline);
        bool restart = false;
        while (g_running.load(std::memory_order_acquire) && !restart) {
            GstMessage* message = gst_bus_timed_pop_filtered(
                bus, 100 * GST_MSECOND,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
            if (!message) {
                // [gstreamer.4] Freeze watchdog: if a LIVE source has delivered no frame
                // to the ring for kLiveWatchdogUs, the pipeline is stalled (e.g. RTSP
                // deskewed or dropped). Force a rebuild/reconnect instead of freezing.
                if (g_live_source && !g_paused.load(std::memory_order_acquire)) {
                    const int64_t last = g_last_frame_us.load(std::memory_order_acquire);
                    const int64_t now = gst_util_get_timestamp() / 1000;
                    /* [V23] adaptive limit: 8s until this generation's first
                     * frame (keyframe wait), 2.5s for mid-stream stalls. */
                    const int64_t wd_limit =
                        (last > built_us) ? kLiveWatchdogUs : kFirstFrameWatchdogUs;
                    if (last > 0 && (now - last) > wd_limit) {
                        LOGW("[build=%s][%s] Freeze watchdog: no live frame for %lldms -> restarting pipeline",
                             FRAME_BUILD_ID, FRAME_PRODUCER_VERSION, (long long)((now - last) / 1000));
                        restart = true;
                    }
                }
                continue;
            }
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError* error = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                LOGE("Media producer error: %s%s%s",
                     error ? error->message : "unknown", debug ? " (" : "", debug ? debug : "");
                if (error) g_error_free(error);
                if (debug) g_free(debug);
                restart = true;
            } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
                if ((g_static_source || (!g_live_source && g_local_frame_count <= 1)) &&
                    !g_static_y.empty()) {
                    gst_message_unref(message);
                    pushStaticFrameLoop();
                    break;
                }
                /* [V64 LOOP] Local file video at EOS: seek back to 0 for a
                 * seamless loop instead of tearing the pipeline down and
                 * rebuilding (old behavior: ~150ms+ gap, fd reopen, full
                 * redecode). The decode thread + heartbeat keep running, so
                 * the ring never goes stale while the video loops forever —
                 * the camera can be opened at any moment and still see the
                 * injected media. Non-seekable sources fall back to rebuild. */
                if (!g_live_source) {
                    LOGI("[build=%s][%s] EOS on local video — seeking to 0 (seamless loop, frames=%llu)",
                         FRAME_BUILD_ID, FRAME_PRODUCER_VERSION,
                         (unsigned long long)g_local_frame_count);
                    const gboolean sought = gst_element_seek(
                        g_pipeline, 1.0, GST_FORMAT_TIME,
                        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                        GST_SEEK_TYPE_SET, 0, GST_SEEK_TYPE_NONE, 0);
                    if (sought) {
                        g_last_pts = GST_CLOCK_TIME_NONE;
                        gst_message_unref(message);
                        continue;
                    }
                    LOGW("[build=%s][%s] EOS seek failed (non-seekable source) — falling back to pipeline rebuild",
                         FRAME_BUILD_ID, FRAME_PRODUCER_VERSION);
                }
                restart = true;
            }
            gst_message_unref(message);
        }
        if (bus) gst_object_unref(bus);
        destroyProducerPipeline();
        if (g_running.load(std::memory_order_acquire)) {
            /* [V22 WATCHDOG] If this pipeline delivered at least one frame,
             * reset the restart backoff; if it delivered NOTHING, back off
             * exponentially (500ms -> 4s cap) so a dead/unreachable RTSP
             * source cannot turn into a reconnect storm. */
            /* [V24 TCP] Health = delivered a REAL stream, not just the first
             * keyframe. 13.unisoc.1 V23 log: every rebuilt session connected
             * fine and delivered EXACTLY ONE frame (the opening keyframe,
             * pts~0.8-1.0s), then RTP stopped dead — zero bus errors, forever,
             * on a 4.15s rebuild cycle ("stuck on 1 frame while OBS plays").
             * V23's frameless-only trigger never fired because gens delivered
             * >=1 frame. Count SHORT-LIVED gens (<90 frames ~= 1.5s @60fps) as
             * unhealthy; 2 in a row -> force RTP-over-TCP. Once TCP has proven
             * itself with a healthy generation, stay on TCP (ratchet) — going
             * back to UDP would just re-enter the black hole. */
            const bool delivered =
                g_last_frame_us.load(std::memory_order_acquire) > built_us;
            const uint64_t frames_gen = delivered ? g_local_frame_count : 0;
            const bool healthy = frames_gen >= 90;
            LOGI("[build=%s][%s] generation summary: frames=%llu healthy=%d tcp=%d unhealthy_streak=%d",
                 FRAME_BUILD_ID, FRAME_PRODUCER_VERSION, (unsigned long long)frames_gen,
                 healthy ? 1 : 0, g_force_rtsp_tcp.load(std::memory_order_acquire) ? 1 : 0,
                 unhealthy_gens);
            if (healthy) {
                wd_backoff_ms = 0;
                unhealthy_gens = 0;
                if (g_force_rtsp_tcp.load(std::memory_order_acquire)) tcp_proven = true;
                g_force_rtsp_tcp.store(tcp_proven, std::memory_order_release);
                g_usleep(150000);
            } else {
                unhealthy_gens++;
                if (unhealthy_gens >= 2 &&
                    !g_force_rtsp_tcp.load(std::memory_order_acquire)) {
                    g_force_rtsp_tcp.store(true, std::memory_order_release);
                    LOGW("[build=%s][%s] %d consecutive short-lived generations (last=%llu frames) — forcing RTSP transport to TCP",
                         FRAME_BUILD_ID, FRAME_PRODUCER_VERSION, unhealthy_gens,
                         (unsigned long long)frames_gen);
                }
                /* [V71] backoff ladder 500ms → 15s cap. On the Redmi the
                 * first RTSP stall left the OBS server holding a dead session;
                 * reconnects every ≤4s kept landing on the ghost and got 0
                 * frames forever. Longer gaps let the server-side session
                 * time out so a rebuild can actually receive RTP again. */
                wd_backoff_ms = (wd_backoff_ms == 0) ? 500
                                : (wd_backoff_ms < 15000 ? wd_backoff_ms * 2 : 15000);
                LOGW("[build=%s][%s] generation delivered only %llu frames — backing off %dms before rebuild",
                     FRAME_BUILD_ID, FRAME_PRODUCER_VERSION,
                     (unsigned long long)frames_gen, wd_backoff_ms);
                g_usleep(wd_backoff_ms * 1000);
            }
        }
    }
    destroyProducerPipeline();
    return nullptr;
}

extern "C" {

// The GStreamer Android mobile bridge (GStreamer::mobile) is linked statically
// and exports a WEAK JNI_OnLoad. When this library is dlopen'd the dynamic
// linker invokes JNI_OnLoad; because the bridge's copy is weak it is the one
// that gets selected, and on this SDK/device it returns 0 (an invalid JNI
// version). Android's System.loadLibrary then throws UnsatisfiedLinkError
// ("Bad JNI version returned from JNI_OnLoad ...: 0"), so the whole
// frame_producer library fails to load. That single failure is what surfaces
// as the "Native injection failed – check Zygisk module" toast.
//
// Define our own STRONG JNI_OnLoad so it overrides the weak bridge symbol,
// initialize GStreamer here (idempotent — decode_thread also calls gst_init),
// and report a real JNI version so the library loads and the producer works.
JNIEXPORT jint JNICALL
JNI_OnLoad(JavaVM *vm, void *) {
    (void)vm;
    gst_init(nullptr, nullptr);
    if (gst_android_load_gio_modules) gst_android_load_gio_modules();
    LOGI("[%s] JNI_OnLoad: Amkush producer initialized", FRAME_PRODUCER_VERSION);
    return JNI_VERSION_1_6;
}

JNIEXPORT jint JNICALL
Java_com_itsme_amkush_hooks_NativeFrameProducer_nativeStart(JNIEnv *env,
                                                              jobject ,
                                                              jstring jSourcePath) {
    pthread_mutex_lock(&g_init_lock);
    if (g_thread_valid) {
        g_running.store(false);
        pthread_join(g_thread, nullptr);
        g_thread_valid = false;
    }
    /* [V66] Retire the outgoing ring into the prev slot (kept mapped, fd kept
     * open, heartbeat keeps writing it via g_hb_prev_hdr) instead of the old
     * unmap-and-unbind. The hook stays live on the last frame of the old slot
     * until the new ring is fed over IPC. */
    if (g_prev_map != MAP_FAILED) { munmap(g_prev_map, g_prev_map_size); g_prev_map = MAP_FAILED; }
    if (g_prev_ashmem_fd >= 0) { close(g_prev_ashmem_fd); g_prev_ashmem_fd = -1; }
    if (g_map_base != MAP_FAILED) {
        g_hb_prev_hdr.store((FrameSourceHeader *)g_map_base, std::memory_order_release);
        g_prev_map = g_map_base;
        g_prev_map_size = g_map_size;
        g_map_base = MAP_FAILED;
    }
    if (g_ashmem_fd >= 0) { g_prev_ashmem_fd = g_ashmem_fd; g_ashmem_fd = -1; }
    const char *src = env->GetStringUTFChars(jSourcePath, nullptr);
    if (!src) {
        pthread_mutex_unlock(&g_init_lock);
        return -1;
    }
    strncpy(g_source, src, sizeof(g_source) - 1);
    g_source[sizeof(g_source) - 1] = '\0';
    env->ReleaseStringUTFChars(jSourcePath, src);
    g_ring_ready.store(false, std::memory_order_release);
    g_ring_width = 0;
    g_ring_height = 0;
    g_static_y_stride = 0;
    g_static_uv_stride = 0;
    g_paused.store(false, std::memory_order_release);
    g_hook_fed.store(false, std::memory_order_release);
    g_running.store(true);
    if (pthread_create(&g_thread, nullptr, decode_thread, nullptr) != 0) {
        LOGE("pthread_create failed: %s", strerror(errno));
        g_running.store(false);
        pthread_mutex_unlock(&g_init_lock);
        return -1;
    }
    g_thread_valid = true;
    pthread_mutex_unlock(&g_init_lock);
    if (isLiveSource(std::string(g_source))) {
        /* [V13 RTSP] Don't block the UI on the RTSP handshake: report started
         * now; decode_thread retries the connect and maybe_feed_hook() feeds
         * the cameraserver hook on the first decoded pad. */
        LOGI("NativeFrameProducer [%s] live source — started (async hook feed): %s",
             FRAME_PRODUCER_VERSION, g_source);
        LOGI("║ FRESH-BUILD-CHECK(producer) version=%s build_id=%s (async live start)",
             FRAME_PRODUCER_VERSION, FRAME_BUILD_ID);
        return 0;
    }
    {
        const int kMaxTries = 500;
        for (int i = 0; i < kMaxTries; ++i) {
            if (!g_ring_ready.load(std::memory_order_acquire)) {
                if (!g_running.load(std::memory_order_acquire)) break;
                struct timespec ts = {0, 20000000L};
                nanosleep(&ts, nullptr);
                continue;
            }
            FrameSourceHeader *rhdr = static_cast<FrameSourceHeader *>(g_map_base);
            if (rhdr && rhdr->write_slot.load(std::memory_order_acquire) > 0) break;
            if (!g_running.load(std::memory_order_acquire)) break;
            struct timespec ts = {0, 20000000L};
            nanosleep(&ts, nullptr);
        }
    }
    pthread_mutex_lock(&g_init_lock);
    maybe_feed_hook();
    const bool hook_fed = g_hook_fed.load(std::memory_order_acquire);
    if (!hook_fed) {
        LOGW("Ring never became ready -- cameraserver hook not fed");
    }
    pthread_mutex_unlock(&g_init_lock);

    if (!hook_fed) {
        // Do not report success when the only producer-to-cameraserver
        // handshake failed. Stop and release this attempt so the UI can retry
        // after the hook becomes live instead of leaving a false-positive
        // producer running with zero injectable frames.
        LOGE("NativeFrameProducer could not initialize cameraserver frame source");
        g_running.store(false, std::memory_order_release);
        if (g_thread_valid) {
            pthread_join(g_thread, nullptr);
            g_thread_valid = false;
        }
        pthread_mutex_lock(&g_init_lock);
        if (g_map_base != MAP_FAILED) {
            g_hb_hdr.store((FrameSourceHeader *)MAP_FAILED, std::memory_order_release); /* [V28] unbind before unmap */
            munmap(g_map_base, g_map_size);
            g_map_base = MAP_FAILED;
        }
        if (g_ashmem_fd >= 0) {
            close(g_ashmem_fd);
            g_ashmem_fd = -1;
        }
        g_slots_base = nullptr;
        g_slot_stride = 0;
        g_ring_ready.store(false, std::memory_order_release);
        pthread_mutex_unlock(&g_init_lock);
        return -2;
    }

    LOGI("NativeFrameProducer [%s] started and frame source sent: %s",
         FRAME_PRODUCER_VERSION, g_source);
    // ── FRESH-BUILD PROOF ──────────────────────────────────────────────────
    // FRAME_BUILD_ID is the git branch+sha baked into libframe_producer.so at
    // compile time. Grep Mylogs logcat for this banner to confirm the APK on the
    // phone is the NEW gstreamer.4 build (build_id == gstreamer.4-<sha>).
    LOGI("╔══════════════════════════════════════════════════════════════════╗");
    LOGI("║ FRESH-BUILD-CHECK(producer) version=%s", FRAME_PRODUCER_VERSION);
    LOGI("║ FRESH-BUILD-CHECK(producer) build_id=%s", FRAME_BUILD_ID);
    if (g_map_base != MAP_FAILED && g_map_base != nullptr) {
        FrameSourceHeader *chdr = static_cast<FrameSourceHeader *>(g_map_base);
        if (chdr->magic == FRAME_SOURCE_MAGIC) {
            LOGI("║ FRESH-BUILD-CHECK(producer) chroma_override=%d",
                 chdr->chroma_override.load(std::memory_order_acquire));
        }
    }
    LOGI("║ FRESH-BUILD-CHECK(producer) (build_id == gstreamer.4-<sha> ⇒ NEW build)");
    LOGI("╚══════════════════════════════════════════════════════════════════╝");
    return 0;
}

JNIEXPORT void JNICALL
Java_com_itsme_amkush_hooks_NativeFrameProducer_nativeStop(JNIEnv * ,
                                                             jobject ) {
    pthread_mutex_lock(&g_init_lock);
    g_running.store(false);
    if (g_thread_valid) {
        pthread_join(g_thread, nullptr);
        g_thread_valid = false;
    }
    if (g_map_base != MAP_FAILED) {
        g_hb_hdr.store((FrameSourceHeader *)MAP_FAILED, std::memory_order_release); /* [V28] unbind before unmap */
        munmap(g_map_base, g_map_size);
        g_map_base = MAP_FAILED;
    }
    if (g_ashmem_fd >= 0) {
        close(g_ashmem_fd);
        g_ashmem_fd = -1;
    }
    g_slots_base  = nullptr;
    g_slot_stride = 0;
    /* [V66] explicit stop retires the bridged ring too — the hook must go
     * stale so the real camera returns. */
    g_hb_prev_hdr.store(nullptr, std::memory_order_release);
    if (g_prev_map != MAP_FAILED) { munmap(g_prev_map, g_prev_map_size); g_prev_map = MAP_FAILED; }
    if (g_prev_ashmem_fd >= 0) { close(g_prev_ashmem_fd); g_prev_ashmem_fd = -1; }
    g_ring_ready.store(false, std::memory_order_release);
    g_paused.store(false, std::memory_order_release);
    heartbeat_stop(); /* [V27] let the hook go stale -> real camera */
    pthread_mutex_unlock(&g_init_lock);
    LOGI("NativeFrameProducer stopped");
}

JNIEXPORT void JNICALL
Java_com_itsme_amkush_hooks_NativeFrameProducer_nativeSetOverlayParams(
        JNIEnv * , jobject ,
        jint panX, jint panY, jint scaleQ16) {
    if (g_map_base != MAP_FAILED && g_map_base != nullptr) {
        FrameSourceHeader *hdr = static_cast<FrameSourceHeader *>(g_map_base);
        if (hdr->magic == FRAME_SOURCE_MAGIC) {
            hdr->pan_x.store((int32_t)panX,         std::memory_order_release);
            hdr->pan_y.store((int32_t)panY,         std::memory_order_release);
            hdr->scale_q16.store((uint32_t)scaleQ16, std::memory_order_release);
            LOGI("[%s] setOverlayParams panX=%d panY=%d scaleQ16=%d", FRAME_PRODUCER_VERSION, panX, panY, scaleQ16);
        }
    }
}

JNIEXPORT void JNICALL
Java_com_itsme_amkush_hooks_NativeFrameProducer_nativeSetRotation(
        JNIEnv * , jobject ,
        jint degrees) {
    uint32_t rot = (uint32_t)(((degrees % 360) + 360) % 360);
    if (g_map_base != MAP_FAILED && g_map_base != nullptr) {
        FrameSourceHeader *hdr = static_cast<FrameSourceHeader *>(g_map_base);
        if (hdr->magic == FRAME_SOURCE_MAGIC) {
            hdr->manual_rotation.store(rot, std::memory_order_release);
            LOGI("setRotation manual_rotation=%u", rot);
        }
    }
}

JNIEXPORT void JNICALL
Java_com_itsme_amkush_hooks_NativeFrameProducer_nativeSetChromaOverride(
        JNIEnv * , jobject ,
        jint overrideIsNv21) {
    // [gstreamer.4] Chroma A/B: writes the override into the shared header so a
    // SINGLE APK can test both orders for the opaque 0x22 stream without a rebuild.
    // -1 = unset (use build default), 0 = NV12 (no swap), 1 = NV21 (swap).
    const int32_t v = (overrideIsNv21 > 1) ? 1 : (overrideIsNv21 < 0 ? -1 : overrideIsNv21);
    if (g_map_base != MAP_FAILED && g_map_base != nullptr) {
        FrameSourceHeader *hdr = static_cast<FrameSourceHeader *>(g_map_base);
        if (hdr->magic == FRAME_SOURCE_MAGIC) {
            hdr->chroma_override.store(v, std::memory_order_release);
            LOGI("[build=%s] setChromaOverride=%d (%s)", FRAME_PRODUCER_VERSION, v,
                 v == 1 ? "force NV21" : (v == 0 ? "force NV12" : "revert to default"));
        }
    }
}

JNIEXPORT void JNICALL
Java_com_itsme_amkush_hooks_NativeFrameProducer_nativeSetPaused(
        JNIEnv * , jobject ,
        jboolean paused) {
    g_paused.store(paused ? true : false, std::memory_order_release);
    LOGI("setPaused=%d", paused ? 1 : 0);
}

}

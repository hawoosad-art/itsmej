

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/display.h>
}

#include <android/log.h>
#include <android/sharedmem.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <atomic>
#include <vector>
#include <string.h>
#include <errno.h>
#include <jni.h>
#include <stdint.h>
#include <inttypes.h>

#define TAG "amkush/frame_producer"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)

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
    /* Pan/zoom overlay control — written by FaceGate, read by cameraserver */
    std::atomic<int32_t>  pan_x;
    std::atomic<int32_t>  pan_y;
    std::atomic<uint32_t> scale_q16;      /* Q16 fixed-point: 65536 = 1.0 */
    std::atomic<uint32_t> source_rotation;  /* Fix1: CW rotation degrees (0/90/180/270) detected from media */
    std::atomic<uint32_t> manual_rotation;  /* User-controlled extra CW rotation (0/90/180/270) */
    uint8_t               _pad[16];
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
static std::atomic<bool> g_ring_ready{false};   /* set once ring is mapped (decode thread) */

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
        LOGE("Buffer size overflow: slot=%" PRIu64 " total=%" PRIu64, slot_sz_64, total_64);
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
    hdr->source_rotation.store(0u, std::memory_order_release);
    hdr->manual_rotation.store(0u, std::memory_order_release);

    g_ashmem_fd   = fd;
    g_map_base    = base;
    g_map_size    = total;
    g_slots_base  = (uint8_t *)base + sizeof(FrameSourceHeader);
    g_slot_stride = stride;
    g_ring_ready.store(true, std::memory_order_release);

    LOGI("Ashmem ring created: %dx%d stride=%u format=%u slot_sz=%zu total=%zu fd=%d",
         w, h, stride, fmt, slot_sz, total, fd);
    return fd;
}

/* ── NV12 rotation helpers ─────────────────────────────────────────────────
 * Apply CW rotation to NV12 frames before writing to the ring buffer.
 * For 90/270° the output dimensions are swapped (dst_w=src_h, dst_h=src_w).
 * Caller must ensure dst buffers are large enough for the rotated dimensions.
 */
static void rotate_nv12_90cw(
    const uint8_t *src_y, int src_w, int src_h,
    uint8_t *dst_y)
{
    // dst is (src_h x src_w)
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
    // UV plane: half dimensions
    int uvW = src_w / 2, uvH = src_h / 2;
    // dst UV dims: uvH x uvW (since image was rotated)
    int dst_uvW = uvH; // dst_w/2 where dst_w=src_h
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
    // Y plane
    for (int y = 0; y < src_h; y++) {
        for (int x = 0; x < src_w; x++) {
            dst_y[(src_h - 1 - y) * src_w + (src_w - 1 - x)] = src_y[y * src_w + x];
        }
    }
    // UV plane
    int uvW = src_w / 2, uvH = src_h / 2;
    for (int y = 0; y < uvH; y++) {
        for (int x = 0; x < uvW; x++) {
            dst_uv[(uvH - 1 - y) * uvW * 2 + (uvW - 1 - x) * 2]     = src_uv[y * uvW * 2 + x * 2];
            dst_uv[(uvH - 1 - y) * uvW * 2 + (uvW - 1 - x) * 2 + 1] = src_uv[y * uvW * 2 + x * 2 + 1];
        }
    }
}

/* Rotate NV12 in-place (using temp buffers).
 * rotation: 0, 90, 180, 270 (all CW).
 * For 90/270, src must already be MASTER_HEIGHT x MASTER_WIDTH so the output
 * fits in MASTER_WIDTH x MASTER_HEIGHT (i.e. dimensions swap back). */
static std::vector<uint8_t> g_rot_tmp_y;
static std::vector<uint8_t> g_rot_tmp_uv;

static void apply_nv12_rotation(
    uint8_t *y_buf, uint8_t *uv_buf,
    int w, int h, uint32_t rotation)
{
    if (rotation == 0) return;

    size_t y_sz  = (size_t)w * h;
    size_t uv_sz = (size_t)(w / 2) * (h / 2) * 2;

    if (g_rot_tmp_y.size()  < y_sz)  g_rot_tmp_y.resize(y_sz);
    if (g_rot_tmp_uv.size() < uv_sz) g_rot_tmp_uv.resize(uv_sz);

    if (rotation == 90) {
        rotate_nv12_90cw(y_buf, w, h, g_rot_tmp_y.data());
        rotate_nv12_90cw_uv(uv_buf, w, h, g_rot_tmp_uv.data());
        memcpy(y_buf,  g_rot_tmp_y.data(),  y_sz);
        memcpy(uv_buf, g_rot_tmp_uv.data(), uv_sz);
    } else if (rotation == 180) {
        rotate_nv12_180(y_buf, uv_buf, w, h, g_rot_tmp_y.data(), g_rot_tmp_uv.data());
        memcpy(y_buf,  g_rot_tmp_y.data(),  y_sz);
        memcpy(uv_buf, g_rot_tmp_uv.data(), uv_sz);
    } else if (rotation == 270) {
        // 270 CW = 90 CCW: rotate 90 CW three times, or simply do transpose differently
        // Easiest: rotate 90 CW twice (= 180), then 90 CW again
        // Step 1: 90 CW
        rotate_nv12_90cw(y_buf, w, h, g_rot_tmp_y.data());
        rotate_nv12_90cw_uv(uv_buf, w, h, g_rot_tmp_uv.data());
        // Step 2: 180 on the 90-rotated result (now h x w)
        // For 270 CW total we can also do: for dst(x,y) = src(y, w-1-x)
        // Let's just apply two 90CW to get 180, then one more for 270...
        // Actually 270CW = 90CCW: dst(x,y) = src(H-1-x, y) where dst is (H x W)
        // Redo from src directly:
        memset(g_rot_tmp_y.data(),  0, y_sz);
        memset(g_rot_tmp_uv.data(), 0, uv_sz);
        // 270 CW = dst(x, y) = src(w-1-x, y) [NOTE: dst dims = h x w]
        // Actually: 270 CW: dst(dst_x, dst_y) = src(dst_y, src_h - 1 - dst_x)
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                // dst dim: h x w (dst_w = h, dst_h = w)
                int dst_x = y;
                int dst_y_coord = w - 1 - x;
                g_rot_tmp_y[dst_y_coord * h + dst_x] = y_buf[y * w + x];
            }
        }
        // UV
        int uvW = w / 2, uvH = h / 2;
        int dst_uvW = uvH; // dst_w/2
        for (int y = 0; y < uvH; y++) {
            for (int x = 0; x < uvW; x++) {
                int dst_x = y;
                int dst_y_coord = uvW - 1 - x;
                g_rot_tmp_uv[(dst_y_coord * dst_uvW + dst_x) * 2]     = uv_buf[(y * uvW + x) * 2];
                g_rot_tmp_uv[(dst_y_coord * dst_uvW + dst_x) * 2 + 1] = uv_buf[(y * uvW + x) * 2 + 1];
            }
        }
        memcpy(y_buf,  g_rot_tmp_y.data(),  y_sz);
        memcpy(uv_buf, g_rot_tmp_uv.data(), uv_sz);
    }
}

static void ring_write(const uint8_t *y_src, int y_stride,
                        const uint8_t *uv_src, int uv_stride,
                        int w, int h) {
    FrameSourceHeader *hdr = (FrameSourceHeader *)g_map_base;
    if (!hdr || !g_slots_base) return;

    /* Use the dimensions the ring was actually created with (header fields),
     * not the compile-time MASTER_* constants.  For a portrait source the ring
     * is 720×1280, so writing 720×1280 must NOT overflow.  Using the header's
     * master dims keeps slot sizing and UV offset consistent with the reader
     * (frame_source.cpp computes uv at slot_stride*master_height). */
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

    /* Fix4: never drop video frames — overwrite oldest slot when ring is full so that
     * continuous video playback always delivers the latest frame even though
     * frame_source_get_latest() in cameraserver never consumes (advances) read_slot. */
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
    }
    for (int row = 0; row < h / 2; row++) {
        memcpy(dst_uv + row * slot_stride, uv_src + row * uv_stride, (size_t)w);
    }

    hdr->write_slot.store(next, std::memory_order_release);
}

static int send_fd_to_hook(int fd) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        LOGE("socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, AMKUSH_SOCKET_NAME, sizeof(AMKUSH_SOCKET_NAME) - 1);
    socklen_t addr_len = offsetof(struct sockaddr_un, sun_path) + sizeof(AMKUSH_SOCKET_NAME) - 1;

    if (connect(sock, (struct sockaddr *)&addr, addr_len) < 0) {
        LOGE("connect() to cameraserver hook failed: %s", strerror(errno));
        close(sock);
        return -1;
    }

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

    ssize_t n = sendmsg(sock, &msg, 0);
    close(sock);

    if (n <= 0) {
        LOGE("sendmsg() failed: %s", strerror(errno));
        return -1;
    }
    LOGI("Sent Ashmem fd=%d to cameraserver hook", fd);
    return 0;
}

static std::atomic_bool g_running{false};
static std::atomic_bool g_paused{false};   /* play/stop toggle: while true, decode thread stops writing frames */
static pthread_t   g_thread;
static bool        g_thread_valid = false;
static char        g_source[4096];

static void *decode_thread(void *) {
    AVFormatContext *fmt_ctx = nullptr;
    AVCodecContext  *dec_ctx = nullptr;
    SwsContext      *sws_ctx = nullptr;
    AVFrame  *frame   = av_frame_alloc();
    AVFrame  *nv12_fr = av_frame_alloc();
    AVPacket *pkt     = av_packet_alloc();

    if (!frame || !nv12_fr || !pkt) {
        LOGE("av_frame/packet alloc failed");
        goto cleanup;
    }

    if (avformat_open_input(&fmt_ctx, g_source, nullptr, nullptr) < 0) {
        LOGE("avformat_open_input failed for: %s", g_source);
        goto cleanup;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        LOGE("avformat_find_stream_info failed for: %s", g_source);
        goto cleanup;
    }

    {
        int video_stream = -1;
        const AVCodec *codec = nullptr;
        for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
            if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                video_stream = (int)i;
                codec = avcodec_find_decoder(fmt_ctx->streams[i]->codecpar->codec_id);
                break;
            }
        }
        if (video_stream < 0 || !codec) {
            LOGE("No video stream found in: %s", g_source);
            goto cleanup;
        }

        dec_ctx = avcodec_alloc_context3(codec);
        if (!dec_ctx) {
            LOGE("avcodec_alloc_context3 failed");
            goto cleanup;
        }
        if (avcodec_parameters_to_context(dec_ctx,
                    fmt_ctx->streams[video_stream]->codecpar) < 0) {
            LOGE("avcodec_parameters_to_context failed");
            goto cleanup;
        }
        if (avcodec_open2(dec_ctx, codec, nullptr) < 0) {
            LOGE("avcodec_open2 failed (codec=%s)", codec->name);
            goto cleanup;
        }

        /* Detect portrait source (height > width). We scale to a ring that
         * PRESERVES the source's aspect ratio (instead of forcing a fixed
         * 720×1280 / 1280×720), so faces are not horizontally stretched or
         * smeared. For a portrait source we fit the long side to MASTER_WIDTH
         * and scale width proportionally; for landscape we fit width to
         * MASTER_WIDTH and scale height proportionally. The ring is created at
         * these exact dims (dynamic sizing already supported since V4.8.0).
         * inject_yuv's Fix1/Fix3 then rotate + AR-crop to the destination. */
        bool src_is_portrait = (dec_ctx->height > dec_ctx->width && dec_ctx->width > 0);
        double src_aspect = (dec_ctx->height > 0)
            ? (double)dec_ctx->width / (double)dec_ctx->height
            : 1.0;
        int sws_dst_w, sws_dst_h;
        if (src_is_portrait) {
            sws_dst_h = MASTER_WIDTH;                       /* long side = 1280 */
            sws_dst_w = (int)(sws_dst_h * src_aspect);      /* scale width to keep aspect */
        } else {
            sws_dst_w = MASTER_WIDTH;                       /* 1280 */
            sws_dst_h = (int)(sws_dst_w / src_aspect);
        }
        sws_dst_w = (sws_dst_w + 1) & ~1;                   /* keep even (chroma) */
        sws_dst_h = (sws_dst_h + 1) & ~1;
        if (sws_dst_w < 2) sws_dst_w = 2;
        if (sws_dst_h < 2) sws_dst_h = 2;
        if (sws_dst_w > MASTER_WIDTH)  sws_dst_w = MASTER_WIDTH  & ~1;
        if (sws_dst_h > MASTER_WIDTH)  sws_dst_h = MASTER_WIDTH  & ~1;
        LOGI("Fix-aspect: source %dx%d (portrait=%d aspect=%.3f) → ring %dx%d",
             dec_ctx->width, dec_ctx->height, (int)src_is_portrait, src_aspect,
             sws_dst_w, sws_dst_h);

        nv12_fr->format = AV_PIX_FMT_NV12;
        nv12_fr->width  = sws_dst_w;
        nv12_fr->height = sws_dst_h;
        if (av_frame_get_buffer(nv12_fr, 64) < 0) {
            LOGE("av_frame_get_buffer failed for %dx%d NV12",
                 sws_dst_w, sws_dst_h);
            goto cleanup;
        }

        sws_ctx = sws_getContext(dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
                                 sws_dst_w, sws_dst_h, AV_PIX_FMT_NV12,
                                 SWS_LANCZOS, nullptr, nullptr, nullptr);
        if (!sws_ctx) {
            LOGE("sws_getContext failed (%dx%d fmt=%d → %dx%d NV12)",
                 dec_ctx->width, dec_ctx->height, (int)dec_ctx->pix_fmt,
                 sws_dst_w, sws_dst_h);
            goto cleanup;
        }

        /* Create the ashmem ring sized to the ACTUAL scaled source dimensions.
         * Previously it was always created at 1280×720 in nativeStart; writing a
         * 720×1280 portrait frame into that fixed slot overflowed the buffer and
         * produced the green frame.  Sizing the ring to the content (720×1280)
         * means ring_write never overflows, so the injected media shows correctly
         * with correct aspect ratio (no stretching — inject_yuv's Fix3 crop handles
         * the portrait→landscape fit). */
        if (create_ashmem_ring(sws_dst_w, sws_dst_h) < 0) {
            LOGE("create_ashmem_ring failed for %dx%d", sws_dst_w, sws_dst_h);
            goto cleanup;
        }

        const AVCodecID static_codecs[] = {
            AV_CODEC_ID_MJPEG, AV_CODEC_ID_PNG,
            AV_CODEC_ID_BMP,   AV_CODEC_ID_TIFF,
            AV_CODEC_ID_NONE
        };
        bool is_static = false;
        if (fmt_ctx->nb_streams == 1) {
            AVCodecID cid = fmt_ctx->streams[video_stream]->codecpar->codec_id;
            for (int si = 0; static_codecs[si] != AV_CODEC_ID_NONE; si++) {
                if (cid == static_codecs[si]) { is_static = true; break; }
            }
        }
        /* Fix1: detect source rotation from AVStream metadata or display matrix side data.
         *
         * RTSP / live network streams:
         *   - Their display matrix / "rotate" metadata is unreliable, so we do NOT
         *     auto-rotate from metadata.
         *   - But the camera PREVIEW frame is always landscape (640×480, 1280×720,
         *     1920×1080). A PORTRAIT RTSP source (H>W, e.g. 960×1280) must be rotated
         *     90° so it becomes landscape, otherwise inject_yuv does a direct
         *     I420Scale(portrait → landscape) which stretches the picture badly
         *     (pixelated / distorted / blurry). We pick rotation from the source's
         *     own orientation: portrait → 90° (to landscape), landscape → 0°.
         *     The user can still fine-tune with the manual rotate button.
         *
         * Detection: prefer the demuxer/format name over the source string, because
         * an RTSP URL may be opened via a content-provider fd (/proc/self/fd/N) which
         * contains no "rtsp://" text. libavformat's iformat->name is "rtsp", "sdp",
         * "http", "hls" etc. for network inputs. */
        bool is_rtsp_live = false;
        {
            const char *fmt_name = (fmt_ctx && fmt_ctx->iformat && fmt_ctx->iformat->name)
                                       ? fmt_ctx->iformat->name : "";
            is_rtsp_live =
                strstr(fmt_name, "rtsp")   || strstr(fmt_name, "sdp")  ||
                strstr(fmt_name, "http")   || strstr(fmt_name, "hls")  ||
                strstr(fmt_name, "rtmp")   || strstr(fmt_name, "mms")  ||
                strstr(fmt_name, "srt")    || strstr(fmt_name, "udp")  ||
                strncmp(g_source, "rtsp://", 7) == 0 ||
                strncmp(g_source, "rtmp://", 7) == 0 ||
                strncmp(g_source, "http://", 7) == 0 ||
                strncmp(g_source, "https://", 8) == 0 ||
                strncmp(g_source, "rtp://", 6) == 0;
        }
        {
            int rot = 0;
            AVStream *vs = fmt_ctx->streams[video_stream];

            if (is_rtsp_live) {
                /* Live source: no metadata-based auto-rotation. But fit the source's
                 * orientation to the landscape preview so we don't stretch portrait
                 * content into a landscape frame (which looked pixelated/distorted). */
                int src_h = (dec_ctx && dec_ctx->height > 0) ? dec_ctx->height
                          : (vs->codecpar && vs->codecpar->height > 0) ? vs->codecpar->height : 0;
                int src_w = (dec_ctx && dec_ctx->width  > 0) ? dec_ctx->width
                          : (vs->codecpar && vs->codecpar->width  > 0) ? vs->codecpar->width  : 0;
                if (src_w > 0 && src_h > src_w) {
                    rot = 90;   /* portrait → landscape (matches the landscape preview) */
                } else {
                    rot = 0;    /* already landscape */
                }
                LOGI("Fix1: live/network source detected (%s) %dx%d — source_rotation=%d "
                     "(fit to landscape preview, no metadata)",
                     g_source, src_w, src_h, rot);
            } else {
            /* Method 1: displaymatrix side data (JPEG/MP4/MOV typically set this)
             * av_stream_get_side_data() was removed in FFmpeg 7.0; use the
             * codecpar coded_side_data array via av_packet_side_data_get() instead. */
            const AVPacketSideData *_sd1 = (vs->codecpar)
                ? av_packet_side_data_get(vs->codecpar->coded_side_data,
                                          vs->codecpar->nb_coded_side_data,
                                          AV_PKT_DATA_DISPLAYMATRIX)
                : nullptr;
            const uint8_t *dm = _sd1 ? _sd1->data : nullptr;
            if (dm) {
                double angle = av_display_rotation_get((const int32_t *)dm);
                if (!__builtin_isnan(angle)) {
                    /* Use lround() for correct rounding of negative values.
                     * (int)(-angle + 0.5) truncates toward zero, so angle=90.0
                     * yields (int)(-89.5)=-89 → normalises to 271, not 270,
                     * causing no rotation to be applied and a sideways face.
                     * lround(-90.0) = -90 → normalises to 270 correctly. */
                    rot = (int)lround(-angle);
                }
            }
            /* Method 2: "rotate" tag in stream metadata */
            if (rot == 0) {
                AVDictionaryEntry *tag =
                    av_dict_get(vs->metadata, "rotate", nullptr, 0);
                if (tag && tag->value) rot = atoi(tag->value);
            }
            /* Method 3: displaymatrix in codec-parameter side data
             * (used by newer FFmpeg / some MP4 encoders that write rotation
             *  into AVCodecParameters rather than the stream side data) */
            if (rot == 0 && vs->codecpar) {
                for (int sdi = 0; sdi < vs->codecpar->nb_coded_side_data; sdi++) {
                    if (vs->codecpar->coded_side_data[sdi].type == AV_PKT_DATA_DISPLAYMATRIX) {
                        const int32_t *dm32 = (const int32_t *)vs->codecpar->coded_side_data[sdi].data;
                        double angle2 = av_display_rotation_get(dm32);
                        if (!__builtin_isnan(angle2) && angle2 != 0.0) {
                            rot = (int)lround(-angle2);
                        }
                        break;
                    }
                }
            }
            /* Method 4: "rotate" tag in container-level metadata
             * (some QuickTime/MP4 files put it on the format context) */
            if (rot == 0 && fmt_ctx->metadata) {
                AVDictionaryEntry *ctag =
                    av_dict_get(fmt_ctx->metadata, "rotate", nullptr, 0);
                if (ctag && ctag->value) rot = atoi(ctag->value);
            }
            /* Method 5: universal fallback — if no rotation metadata found,
             * always default to 180°.  This is correct for the common case:
             * the source content is 180° from upright in the decoded buffer,
             * as confirmed empirically (rot=0→upside-down, rot=90→head-left,
             * rot=180→upright).  Works for both landscape and portrait sources
             * because inject_yuv's AR-crop handles the aspect ratio after rotation. */
            if (rot == 0) {
                int m5_w = (dec_ctx && dec_ctx->width  > 0) ? dec_ctx->width
                         : (vs->codecpar && vs->codecpar->width  > 0)
                               ? vs->codecpar->width  : MASTER_WIDTH;
                int m5_h = (dec_ctx && dec_ctx->height > 0) ? dec_ctx->height
                         : (vs->codecpar && vs->codecpar->height > 0)
                               ? vs->codecpar->height : MASTER_HEIGHT;
                rot = 180;
                LOGI("Fix1 Method5: no rotation metadata; source %dx%d "
                     "(dec=%dx%d par=%dx%d) — defaulting source_rotation=180",
                     m5_w, m5_h,
                     dec_ctx ? dec_ctx->width  : -1,
                     dec_ctx ? dec_ctx->height : -1,
                     vs->codecpar ? vs->codecpar->width  : -1,
                     vs->codecpar ? vs->codecpar->height : -1);
            }
            }
            rot = ((rot % 360) + 360) % 360;
            /* Store in the ring-buffer header so cameraserver can read it */
            FrameSourceHeader *hdr2 = (FrameSourceHeader *)g_map_base;
            if (hdr2) {
                hdr2->source_rotation.store((uint32_t)rot, std::memory_order_release);
            }
            LOGI("Fix1: detected source_rotation=%d degrees CW", rot);
        }

        LOGI("source=%s  is_static=%d  codec=%s",
             g_source, (int)is_static,
             avcodec_get_name(fmt_ctx->streams[video_stream]->codecpar->codec_id));

        while (g_running.load()) {
            /* Play/stop: while paused, do NOT read/decode/write frames to the
             * ring. The last written frame stays in the ring and inject_yuv keeps
             * showing it, so "stop" freezes the injected media on screen until the
             * user resumes. Sleep briefly so this thread doesn't spin hot. */
            if (g_paused.load(std::memory_order_acquire)) {
                usleep(20000);
                continue;
            }
            int ret = av_read_frame(fmt_ctx, pkt);
            if (ret == AVERROR_EOF) {
                if (is_static) {
                    av_packet_unref(pkt);
                    LOGI("static image — entering 30fps cache-push loop");
                    while (g_running.load()) {
                        /* Play/stop: while paused, skip writing the cached frame so
                         * the injected image freezes (last frame stays on screen). */
                        if (g_paused.load(std::memory_order_acquire)) {
                            usleep(20000);
                            continue;
                        }
                        /* Write raw (unrotated) frame to ring; inject_yuv applies
                         * combined rotation (source_rotation + manual_rotation) at
                         * injection time using libyuv with correct stride handling.
                         *
                         * Use nv12_fr->linesize[] for strides (NOT sws_dst_w): the
                         * NV12 buffer is 64-byte aligned by av_frame_get_buffer, so
                         * for odd/padded widths linesize can exceed the width. The
                         * video path below already does this; using sws_dst_w here
                         * read every row at the wrong pitch and produced the
                         * vertical green/pink/brown stripes on static image injects. */
                        ring_write(nv12_fr->data[0], nv12_fr->linesize[0],
                                   nv12_fr->data[1], nv12_fr->linesize[1],
                                   sws_dst_w, sws_dst_h);
                        usleep(33333);
                    }
                    break;
                }

                if (av_seek_frame(fmt_ctx, video_stream, 0, AVSEEK_FLAG_BACKWARD) < 0) {
                    LOGW("av_seek_frame failed, restarting from beginning");
                    avformat_close_input(&fmt_ctx);
                    if (avformat_open_input(&fmt_ctx, g_source, nullptr, nullptr) < 0) {
                        LOGE("avformat_open_input retry failed");
                        break;
                    }
                    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
                        LOGE("avformat_find_stream_info retry failed");
                        break;
                    }
                }
                avcodec_flush_buffers(dec_ctx);
                continue;
            }
            if (ret < 0) break;
            if (pkt->stream_index != video_stream) { av_packet_unref(pkt); continue; }

            avcodec_send_packet(dec_ctx, pkt);
            av_packet_unref(pkt);

            while (avcodec_receive_frame(dec_ctx, frame) == 0) {
                sws_scale(sws_ctx,
                          (const uint8_t *const *)frame->data, frame->linesize,
                          0, dec_ctx->height,
                          nv12_fr->data, nv12_fr->linesize);

                /* Rotation (source_rotation + manual_rotation) is applied by
                 * inject_yuv using libyuv with proper stride/dimension handling.
                 * Do NOT rotate here — double rotation causes wrong orientation
                 * and in-place NV12 rotation corrupts strides for 90/270 degrees. */
                ring_write(nv12_fr->data[0], nv12_fr->linesize[0],
                           nv12_fr->data[1], nv12_fr->linesize[1],
                           sws_dst_w, sws_dst_h);

                av_frame_unref(frame);
            }
        }
    }

cleanup:
    if (sws_ctx)  sws_freeContext(sws_ctx);
    if (dec_ctx)  avcodec_free_context(&dec_ctx);
    if (fmt_ctx)  avformat_close_input(&fmt_ctx);
    av_frame_free(&frame);
    av_frame_free(&nv12_fr);
    av_packet_free(&pkt);
    LOGI("decode_thread exited");
    return nullptr;
}

extern "C" {

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

    if (g_map_base != MAP_FAILED) {
        munmap(g_map_base, g_map_size);
        g_map_base = MAP_FAILED;
    }
    if (g_ashmem_fd >= 0) {
        close(g_ashmem_fd);
        g_ashmem_fd = -1;
    }

    const char *src = env->GetStringUTFChars(jSourcePath, nullptr);
    if (!src) {
        pthread_mutex_unlock(&g_init_lock);
        return -1;
    }
    strncpy(g_source, src, sizeof(g_source) - 1);
    g_source[sizeof(g_source) - 1] = '\0';
    env->ReleaseStringUTFChars(jSourcePath, src);

    g_ring_ready.store(false, std::memory_order_release);
    g_paused.store(false, std::memory_order_release);

    /* NOTE: the ashmem ring is now created inside decode_thread once it knows
     * the real source dimensions (portrait → 720×1280, landscape → 1280×720).
     * This fixes the green frame (ring overflow) while keeping correct aspect
     * ratio (no stretching — inject_yuv's Fix3 crop handles portrait→landscape). */







    g_running.store(true);
    if (pthread_create(&g_thread, nullptr, decode_thread, nullptr) != 0) {
        LOGE("pthread_create failed: %s", strerror(errno));
        g_running.store(false);
        pthread_mutex_unlock(&g_init_lock);
        return -1;
    }
    g_thread_valid = true;



    {
        /* Wait for the decode thread to create the ring AND write at least one
         * frame before handing the fd to cameraserver.  On slow devices
         * (MediaTek Android 13) the first frame can take >3 s; we wait up to
         * 10 s so the fd is always delivered with write_slot > 0 (avoiding the
         * "skip_no_frame forever" symptom where cameraserver maps a ring that
         * is permanently empty). */
        const int kMaxTries = 500; /* 500 × 20 ms = 10 s */
        for (int i = 0; i < kMaxTries; ++i) {
            if (!g_ring_ready.load(std::memory_order_acquire)) {
                /* ring not created yet — decode thread still opening source */
                if (!g_running.load(std::memory_order_acquire)) break; /* decode failed */
                struct timespec ts = {0, 20000000L};
                nanosleep(&ts, nullptr);
                continue;
            }
            FrameSourceHeader *rhdr = static_cast<FrameSourceHeader *>(g_map_base);
            if (rhdr->write_slot.load(std::memory_order_acquire) > 0) break;
            if (!g_running.load(std::memory_order_acquire)) break; /* decode failed */
            struct timespec ts = {0, 20000000L};
            nanosleep(&ts, nullptr);
        }
    }

    if (g_ring_ready.load(std::memory_order_acquire) && g_ashmem_fd >= 0) {
        if (send_fd_to_hook(g_ashmem_fd) != 0) {
            LOGW("Could not connect to cameraserver hook — hook may not be active");
        }
    } else {
        LOGW("Ring never became ready — cameraserver hook not fed");
    }

    pthread_mutex_unlock(&g_init_lock);

    LOGI("NativeFrameProducer started for: %s", g_source);
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
        munmap(g_map_base, g_map_size);
        g_map_base = MAP_FAILED;
    }
    if (g_ashmem_fd >= 0) {
        close(g_ashmem_fd);
        g_ashmem_fd = -1;
    }
    g_slots_base  = nullptr;
    g_slot_stride = 0;
    g_ring_ready.store(false, std::memory_order_release);
    g_paused.store(false, std::memory_order_release);

    pthread_mutex_unlock(&g_init_lock);

    LOGI("NativeFrameProducer stopped");
}


/**
 * nativeSetOverlayParams — write pan/zoom control values atomically into the
 * shared ashmem FrameSourceHeader so cameraserver's inject_yuv() can read them
 * on the next frame without any IPC round-trip.
 *
 * panX / panY : signed pixel offsets in source-frame coordinates.
 * scaleQ16    : Q16 zoom factor (65536 = 1.0 / identity).
 */
JNIEXPORT void JNICALL
Java_com_itsme_amkush_hooks_NativeFrameProducer_nativeSetOverlayParams(
        JNIEnv * , jobject ,
        jint panX, jint panY, jint scaleQ16) {
    pthread_mutex_lock(&g_init_lock);
    if (g_map_base != MAP_FAILED) {
        FrameSourceHeader *hdr = static_cast<FrameSourceHeader *>(g_map_base);
        hdr->pan_x.store((int32_t)panX,         std::memory_order_release);
        hdr->pan_y.store((int32_t)panY,         std::memory_order_release);
        hdr->scale_q16.store((uint32_t)scaleQ16, std::memory_order_release);
        LOGI("setOverlayParams panX=%d panY=%d scaleQ16=%d", panX, panY, scaleQ16);
    }
    pthread_mutex_unlock(&g_init_lock);
}

/**
 * nativeSetRotation — write a manual CW rotation (0/90/180/270) to the shared
 * ashmem header. The decode thread adds this to the auto-detected source rotation
 * before writing each frame to the ring buffer.
 */
JNIEXPORT void JNICALL
Java_com_itsme_amkush_hooks_NativeFrameProducer_nativeSetRotation(
        JNIEnv * , jobject ,
        jint degrees) {
    uint32_t rot = (uint32_t)(((degrees % 360) + 360) % 360);
    pthread_mutex_lock(&g_init_lock);
    if (g_map_base != MAP_FAILED) {
        FrameSourceHeader *hdr = static_cast<FrameSourceHeader *>(g_map_base);
        hdr->manual_rotation.store(rot, std::memory_order_release);
        LOGI("setRotation manual_rotation=%u", rot);
    }
    pthread_mutex_unlock(&g_init_lock);
}

/**
 * nativeSetPaused — play/stop toggle for the injected media.
 * paused=true  → decode thread stops writing frames to the ring; the last frame
 *                stays on screen (frozen).
 * paused=false → decode thread resumes writing frames (play).
 */
JNIEXPORT void JNICALL
Java_com_itsme_amkush_hooks_NativeFrameProducer_nativeSetPaused(
        JNIEnv * , jobject ,
        jboolean paused) {
    g_paused.store(paused ? true : false, std::memory_order_release);
    LOGI("setPaused=%d", paused ? 1 : 0);
}

}

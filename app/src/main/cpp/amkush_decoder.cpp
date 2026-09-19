#include <jni.h>
#include <android/log.h>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/audio/audio.h>
#include <gst/video/video.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define LOG_TAG "AmkushDecoder"
#define DECODER_TAG "DECODER"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LDECI(...) __android_log_print(ANDROID_LOG_INFO, DECODER_TAG, __VA_ARGS__)
#define LDECE(...) __android_log_print(ANDROID_LOG_ERROR, DECODER_TAG, __VA_ARGS__)
#define FG_TAG "ECOMCAM"
#define LOGFG(...) __android_log_print(ANDROID_LOG_DEBUG, FG_TAG, __VA_ARGS__)

extern "C" void gst_android_load_gio_modules(void) __attribute__((weak));

static JavaVM* g_jvm = nullptr;
static jclass g_frameCallbackClass = nullptr;
static jmethodID g_onFrameAvailable = nullptr;
static jmethodID g_onError = nullptr;
static jmethodID g_onEof = nullptr;
static jmethodID g_onAudioFrame = nullptr;
static bool g_methodsCached = false;

struct DecoderCtx {
    std::string url;
    std::atomic<bool> running{true};
    std::atomic<bool> hotSwapping{false};
    std::mutex swapMu;
    std::string hotSwapUrl;
    jobject callback = nullptr;
    std::thread thread;

    std::atomic<int> width{0};
    std::atomic<int> height{0};
    std::atomic<bool> usingHwAccel{false};
    /* [V81] one-shot diagnostics: preview failures were completely silent in
     * every log we capture, so log the first decoded frame (size + uri) once. */
    std::atomic<bool> loggedFirstVideo{false};
    /* [V91] PREVIEW-FIX. The injection producer (frame_producer) forces RTSP
     * over TCP since V23 because the LAN cameras black-hole UDP RTP; the
     * preview decoder never got that change, so on the Samsung SM-X200 the
     * preview pipeline entered PLAYING, negotiated over RTSP/TCP, then sat
     * forever waiting for UDP media that never arrived ("Connecting…" with no
     * error and no "FIRST decoded frame" line in any capture). Start the
     * preview on TCP too, and fall back to UDP only if TCP delivers nothing. */
    std::atomic<bool> rtspPreferTcp{true};
    /* [V91] monotonic frame counter + transport retry state, driven by the
     * no-frame watchdog in runDecoder. */
    std::atomic<long> videoFrames{0};
    std::atomic<int>  transportAttempt{0};   /* 0 = TCP, 1 = UDP, 2 = give up */
    /* [V92 PREVIEWDIAG] diagnostics: the V91 TCP fix still produced a blank
     * preview and the capture contained nothing between "pipeline running" and
     * the watchdog firing, so every stage of the preview path is now logged:
     * bus traffic, state changes, pad/caps negotiation, audio, and a 2 s
     * heartbeat that proves the pipeline is alive even when no frame arrives. */
    std::atomic<long> audioFrames{0};
    std::atomic<int>  padsAdded{0};
    std::atomic<long> lastVideoMs{0};
    std::atomic<bool> sawSourceSetup{false};
    std::atomic<long> busMessages{0};
    /* [V93] consecutive bus errors for this decoder. Reset by the first decoded
     * video frame. Used to cap the retry loop — see the ERROR branch. */
    std::atomic<int> consecutiveErrors{0};

    GstElement* pipeline = nullptr;
    GstElement* source = nullptr;
    GstElement* videoQueue = nullptr;
    GstElement* videoConvert = nullptr;
    GstElement* videoCaps = nullptr;
    GstElement* videoSink = nullptr;
    GstElement* audioQueue = nullptr;
    GstElement* audioConvert = nullptr;
    GstElement* audioResample = nullptr;
    GstElement* audioCaps = nullptr;
    GstElement* audioSink = nullptr;

    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;
    std::vector<uint8_t> audio;
};

static JNIEnv* attachCurrentThread(bool& didAttach) {
    didAttach = false;
    if (!g_jvm) return nullptr;
    JNIEnv* env = nullptr;
    jint result = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (result == JNI_OK) return env;
    if (result == JNI_EDETACHED && g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
        didAttach = true;
        return env;
    }
    return nullptr;
}

static void detachCurrentThread(bool didAttach) {
    if (didAttach && g_jvm) g_jvm->DetachCurrentThread();
}

static std::string toGstUri(const std::string& raw) {
    if (raw.empty()) return raw;
    const auto scheme = raw.find("://");
    if (scheme != std::string::npos) return raw;
    if (raw[0] == '/') {
        gchar* uri = gst_filename_to_uri(raw.c_str(), nullptr);
        if (!uri) return raw;
        std::string result(uri);
        g_free(uri);
        return result;
    }
    return raw;
}

static bool isLiveUri(const std::string& uri) {
    std::string scheme;
    const auto sep = uri.find("://");
    if (sep != std::string::npos) scheme = uri.substr(0, sep);
    for (char& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return scheme == "rtsp" || scheme == "rtsps" || scheme == "rtmp" ||
           scheme == "rtmps" || scheme == "udp" || scheme == "rtp" ||
           scheme == "srt" || scheme == "tcp";
}

static jlong ptsToUs(GstClockTime pts) {
    return GST_CLOCK_TIME_IS_VALID(pts) ? static_cast<jlong>(GST_TIME_AS_USECONDS(pts)) : 0;
}

static void fireError(DecoderCtx* ctx, int code, const char* message) {
    bool didAttach = false;
    JNIEnv* env = attachCurrentThread(didAttach);
    if (!env || !ctx || !ctx->callback || !g_onError) {
        detachCurrentThread(didAttach);
        return;
    }
    jstring msg = env->NewStringUTF(message ? message : "Amkush pipeline error");
    env->CallVoidMethod(ctx->callback, g_onError, static_cast<jint>(code), msg);
    if (msg) env->DeleteLocalRef(msg);
    if (env->ExceptionCheck()) env->ExceptionClear();
    detachCurrentThread(didAttach);
}

static void fireEof(DecoderCtx* ctx) {
    bool didAttach = false;
    JNIEnv* env = attachCurrentThread(didAttach);
    if (env && ctx && ctx->callback && g_onEof) {
        env->CallVoidMethod(ctx->callback, g_onEof);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    detachCurrentThread(didAttach);
}

static void fireVideoFrame(DecoderCtx* ctx, jlong ptsUs) {
    bool didAttach = false;
    JNIEnv* env = attachCurrentThread(didAttach);
    if (!env || !ctx || !ctx->callback || !g_onFrameAvailable || ctx->y.empty()) {
        detachCurrentThread(didAttach);
        return;
    }

    jobject yBuf = env->NewDirectByteBuffer(ctx->y.data(), static_cast<jlong>(ctx->y.size()));
    jobject uBuf = env->NewDirectByteBuffer(ctx->u.data(), static_cast<jlong>(ctx->u.size()));
    jobject vBuf = env->NewDirectByteBuffer(ctx->v.data(), static_cast<jlong>(ctx->v.size()));
    if (yBuf && uBuf && vBuf) {
        env->CallVoidMethod(ctx->callback, g_onFrameAvailable, yBuf, uBuf, vBuf,
                            static_cast<jint>(ctx->width.load()),
                            static_cast<jint>(ctx->height.load()), ptsUs);
    }
    if (yBuf) env->DeleteLocalRef(yBuf);
    if (uBuf) env->DeleteLocalRef(uBuf);
    if (vBuf) env->DeleteLocalRef(vBuf);
    if (env->ExceptionCheck()) env->ExceptionClear();
    detachCurrentThread(didAttach);
}

static void fireAudioFrame(DecoderCtx* ctx, int sampleRate, int channels,
                           int samples, jlong ptsUs) {
    bool didAttach = false;
    JNIEnv* env = attachCurrentThread(didAttach);
    if (!env || !ctx || !ctx->callback || !g_onAudioFrame || ctx->audio.empty()) {
        detachCurrentThread(didAttach);
        return;
    }
    jobject pcmBuf = env->NewDirectByteBuffer(ctx->audio.data(), static_cast<jlong>(ctx->audio.size()));
    if (pcmBuf) {
        env->CallVoidMethod(ctx->callback, g_onAudioFrame, pcmBuf,
                            static_cast<jint>(sampleRate), static_cast<jint>(channels),
                            static_cast<jint>(samples), ptsUs);
        env->DeleteLocalRef(pcmBuf);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    detachCurrentThread(didAttach);
}

static GstFlowReturn onVideoSample(GstAppSink* sink, gpointer userData) {
    auto* ctx = static_cast<DecoderCtx*>(userData);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample || !ctx || !ctx->running.load()) {
        if (sample) gst_sample_unref(sample);
        return GST_FLOW_FLUSHING;
    }

    GstCaps* caps = gst_sample_get_caps(sample);
    GstVideoInfo info;
    gst_video_info_init(&info);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (!caps || !buffer || !gst_video_info_from_caps(&info, caps) ||
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_I420) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    /* [V81] first decoded frame once — proves the source actually produced
     * video and tells us the negotiated size (previously unlogged). */
    if (!ctx->loggedFirstVideo.exchange(true)) {
        LDECI("[onVideoSample] FIRST decoded frame %dx%d fmt=I420 uri=%s",
              GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info),
              ctx->url.c_str());
    }
    /* [V91] feeds the no-frame watchdog in runDecoder. */
    ctx->videoFrames.fetch_add(1, std::memory_order_relaxed);
    /* [V93 RETRYCAP] video is flowing, so the failure streak is over. */
    ctx->consecutiveErrors.store(0, std::memory_order_relaxed);
    /* [V92 PREVIEWDIAG] wall-clock of the last frame, for the heartbeat. */
    ctx->lastVideoMs.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count(),
        std::memory_order_relaxed);

    const int width = GST_VIDEO_INFO_WIDTH(&info);
    const int height = GST_VIDEO_INFO_HEIGHT(&info);
    const int uvWidth = (width + 1) / 2;
    const int uvHeight = (height + 1) / 2;
    ctx->width.store(width);
    ctx->height.store(height);
    ctx->y.resize(static_cast<size_t>(width) * height);
    ctx->u.resize(static_cast<size_t>(uvWidth) * uvHeight);
    ctx->v.resize(static_cast<size_t>(uvWidth) * uvHeight);

    const uint8_t* srcY = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
    const uint8_t* srcU = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 1));
    const uint8_t* srcV = static_cast<const uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 2));
    const int strideY = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    const int strideU = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 1);
    const int strideV = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 2);
    for (int row = 0; row < height; ++row)
        std::memcpy(ctx->y.data() + static_cast<size_t>(row) * width,
                    srcY + static_cast<size_t>(row) * strideY, width);
    for (int row = 0; row < uvHeight; ++row) {
        std::memcpy(ctx->u.data() + static_cast<size_t>(row) * uvWidth,
                    srcU + static_cast<size_t>(row) * strideU, uvWidth);
        std::memcpy(ctx->v.data() + static_cast<size_t>(row) * uvWidth,
                    srcV + static_cast<size_t>(row) * strideV, uvWidth);
    }

    fireVideoFrame(ctx, ptsToUs(GST_BUFFER_PTS(buffer)));
    gst_video_frame_unmap(&frame);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

static GstFlowReturn onAudioSample(GstAppSink* sink, gpointer userData) {
    auto* ctx = static_cast<DecoderCtx*>(userData);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample || !ctx || !ctx->running.load()) {
        if (sample) gst_sample_unref(sample);
        return GST_FLOW_FLUSHING;
    }

    GstCaps* caps = gst_sample_get_caps(sample);
    GstAudioInfo info;
    gst_audio_info_init(&info);
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (!caps || !buffer || !gst_audio_info_from_caps(&info, caps) ||
        GST_AUDIO_INFO_FORMAT(&info) != GST_AUDIO_FORMAT_S16LE ||
        !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    /* [V92 PREVIEWDIAG] first decoded audio frame once — an audio-only stall
     * (video never negotiates) previously looked identical to a dead stream. */
    if (ctx->audioFrames.fetch_add(1, std::memory_order_relaxed) == 0) {
        LDECI("[PREVIEW] FIRST audio frame rate=%d ch=%d fmt=S16LE uri=%s",
              GST_AUDIO_INFO_RATE(&info), GST_AUDIO_INFO_CHANNELS(&info),
              ctx->url.c_str());
    }

    ctx->audio.assign(map.data, map.data + map.size);
    const int sampleRate = GST_AUDIO_INFO_RATE(&info);
    const int channels = GST_AUDIO_INFO_CHANNELS(&info);
    const int samples = channels > 0 ? static_cast<int>(map.size / (sizeof(int16_t) * channels)) : 0;
    fireAudioFrame(ctx, sampleRate, channels, samples, ptsToUs(GST_BUFFER_PTS(buffer)));
    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/* [V92 PREVIEWDIAG] uridecodebin picks the source element itself; logging which
 * one it chose proves whether the RTSP path was taken at all. */
static void onChildAdded(GstElement*, GstElement* child, gpointer userData) {
    auto* ctx = static_cast<DecoderCtx*>(userData);
    if (!ctx || !child) return;
    GstElementFactory* factory = gst_element_get_factory(child);
    LDECI("[PREVIEW] source child element: %s",
          factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)) : "(unknown)");
}

static void onSourceSetup(GstElement*, GstElement* source, gpointer userData) {
    auto* ctx = static_cast<DecoderCtx*>(userData);
    GstElementFactory* factory = gst_element_get_factory(source);
    const char* name = factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)) : "";
    if (ctx) ctx->sawSourceSetup.store(true);
    LDECI("[PREVIEW] source-setup fired for element=%s", name && *name ? name : "(unknown)");
    if (name && std::strcmp(name, "rtspsrc") == 0) {
        /* [V91] Same live config the injection producer has used since V18/V23.
         * The old preview config (latency=0, do-retransmission=FALSE, default
         * UDP-first transport) is exactly the combination that starved the
         * producer before gstreamer.4 — and on this LAN the camera's UDP RTP
         * never reaches the tablet at all, so the preview hung on
         * "Connecting…" forever while injection worked. */
        const bool preferTcp = ctx ? ctx->rtspPreferTcp.load(std::memory_order_acquire) : true;
        g_object_set(source,
                     "latency", 200u,              // jitter buffer -> room to retransmit
                     "drop-on-latency", TRUE,
                     "do-retransmission", TRUE,    // recover lost packets instead of starving
                     "tcp-timeout", (guint64)3000000000,  // 3 s TCP connect/tx timeout
                     "timeout",     (guint64)5000000000,  // 5 s UDP timeout
                     nullptr);
        if (preferTcp) {
            /* GST_RTSP_LOWER_TRANS_TCP = 0x4 — RTP interleaved in the RTSP
             * TCP connection; immune to the UDP black-hole on these LANs. */
            g_object_set(source, "protocols", 0x4u, nullptr);
            LDECI("[V91] preview RTSP transport FORCED TCP (interleaved)");
        } else {
            /* GST_RTSP_LOWER_TRANS_UDP = 0x1 — only reached when TCP produced
             * nothing for the whole watchdog window. */
            g_object_set(source, "protocols", 0x1u, nullptr);
            LDECI("[V91] preview RTSP transport fallback UDP (TCP delivered no frames)");
        }
        if (ctx) LDECI("[V91] preview RTSP source: latency=200ms retransmit=TRUE tcp-timeout=3s");
    }
}

static void onPadAdded(GstElement*, GstPad* pad, gpointer userData) {
    auto* ctx = static_cast<DecoderCtx*>(userData);
    if (!ctx) return;
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    if (!caps || gst_caps_is_empty(caps)) {
        if (caps) gst_caps_unref(caps);
        return;
    }

    GstStructure* structure = gst_caps_get_structure(caps, 0);
    const char* media = structure ? gst_structure_get_name(structure) : "";
    GstElement* branch = nullptr;
    if (media && g_str_has_prefix(media, "video/")) branch = ctx->videoQueue;
    else if (media && g_str_has_prefix(media, "audio/")) branch = ctx->audioQueue;

    /* [V92 PREVIEWDIAG] This is the single most informative line in the whole
     * preview path: no pad-added at all means the source produced no streams
     * (bad URL / auth / codec), while a pad we cannot route means the branch is
     * missing. Both used to be invisible. */
    {
        gchar* capsStr = gst_caps_to_string(caps);
        LDECI("[PREVIEW] pad-added #%d name=%s media=%s routable=%s caps=%s",
              ctx->padsAdded.fetch_add(1, std::memory_order_relaxed) + 1,
              gst_pad_get_name(pad),
              media && *media ? media : "(none)",
              branch ? "yes" : "NO - branch missing",
              capsStr ? capsStr : "(null)");
        if (capsStr) g_free(capsStr);
    }

    if (branch) {
        GstPad* sinkPad = gst_element_get_static_pad(branch, "sink");
        if (sinkPad && !gst_pad_is_linked(sinkPad)) {
            GstPadLinkReturn result = gst_pad_link(pad, sinkPad);
            if (result != GST_PAD_LINK_OK)
                LDECE("[PREVIEW] link FAILED for %s pad: %s", media,
                      gst_pad_link_get_name(result));
            else
                LDECI("[PREVIEW] linked %s pad -> branch", media);
        } else if (sinkPad) {
            LDECI("[PREVIEW] %s branch already linked (duplicate pad)", media);
        }
        if (sinkPad) gst_object_unref(sinkPad);
    }
    gst_caps_unref(caps);
}

static void destroyPipeline(DecoderCtx* ctx) {
    if (!ctx) return;
    if (ctx->pipeline) {
        gst_element_set_state(ctx->pipeline, GST_STATE_NULL);
        gst_element_get_state(ctx->pipeline, nullptr, nullptr, 2 * GST_SECOND);
        gst_object_unref(ctx->pipeline);
    }
    ctx->pipeline = nullptr;
    ctx->source = nullptr;
    ctx->videoQueue = nullptr;
    ctx->videoConvert = nullptr;
    ctx->videoCaps = nullptr;
    ctx->videoSink = nullptr;
    ctx->audioQueue = nullptr;
    ctx->audioConvert = nullptr;
    ctx->audioResample = nullptr;
    ctx->audioCaps = nullptr;
    ctx->audioSink = nullptr;
}

static bool buildPipeline(DecoderCtx* ctx) {
    destroyPipeline(ctx);
    ctx->pipeline = gst_pipeline_new("ecomcam-amkush-pipeline");
    ctx->source = gst_element_factory_make("uridecodebin", "source");
    ctx->videoQueue = gst_element_factory_make("queue", "video-queue");
    ctx->videoConvert = gst_element_factory_make("videoconvert", "video-convert");
    GstElement* videoScale = gst_element_factory_make("videoscale", "video-scale");
    ctx->videoCaps = gst_element_factory_make("capsfilter", "video-caps");
    ctx->videoSink = gst_element_factory_make("appsink", "video-sink");
    ctx->audioQueue = gst_element_factory_make("queue", "audio-queue");
    ctx->audioConvert = gst_element_factory_make("audioconvert", "audio-convert");
    ctx->audioResample = gst_element_factory_make("audioresample", "audio-resample");
    ctx->audioCaps = gst_element_factory_make("capsfilter", "audio-caps");
    ctx->audioSink = gst_element_factory_make("appsink", "audio-sink");

    if (!ctx->pipeline || !ctx->source || !ctx->videoQueue || !ctx->videoConvert ||
        !videoScale || !ctx->videoCaps || !ctx->videoSink || !ctx->audioQueue ||
        !ctx->audioConvert || !ctx->audioResample || !ctx->audioCaps || !ctx->audioSink) {
        LOGE("Media element creation failed; required plugins are missing");
        destroyPipeline(ctx);
        return false;
    }

    const std::string uri = toGstUri(ctx->url);
    GstCaps* videoCaps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "I420", nullptr);
    GstCaps* audioCaps = gst_caps_new_simple("audio/x-raw",
                                               "format", G_TYPE_STRING, "S16LE",
                                               "layout", G_TYPE_STRING, "interleaved",
                                               nullptr);
    g_object_set(ctx->source, "uri", uri.c_str(), nullptr);
    g_object_set(ctx->videoCaps, "caps", videoCaps, nullptr);
    g_object_set(ctx->audioCaps, "caps", audioCaps, nullptr);
    /* [V94 ASYNCFIX] THE preview-never-worked bug. The audio branch is always
     * added to the bin and linked queue->convert->resample->capsfilter->appsink,
     * but its upstream is only connected when the stream actually has audio.
     * For an audio-less RTSP stream the audio appsink therefore never receives
     * a buffer, never prerolls, and — with the GstBaseSink default async=TRUE —
     * it holds the whole pipeline's PAUSED->PLAYING transition open forever.
     * No PLAYING means rtspsrc never sends PLAY, so no media flows at all, even
     * though the video pad was created and linked with valid caps.
     *
     * log14 V93 proves it exactly: every heartbeat reads
     *   state=PAUSED pending=PLAYING ... vFrames=0
     * with ZERO ASYNC_DONE for the whole session, while pad-added reported
     * video/x-raw I420 760x1024 linked OK. The injector works on the same camera
     * because its pipeline is video-only. async=FALSE takes both sinks out of
     * the async state change, which is correct here anyway: a preview sink has
     * no reason to gate preroll. */
    g_object_set(ctx->videoSink,
                 "emit-signals", TRUE, "sync", FALSE, "async", FALSE,
                 "max-buffers", 1u, "drop", TRUE, nullptr);
    g_object_set(ctx->audioSink,
                 "emit-signals", TRUE, "sync", FALSE, "async", FALSE,
                 "max-buffers", 8u, "drop", TRUE, nullptr);
    gst_caps_unref(videoCaps);
    gst_caps_unref(audioCaps);

    gst_bin_add_many(GST_BIN(ctx->pipeline), ctx->source,
                     ctx->videoQueue, ctx->videoConvert, videoScale, ctx->videoCaps, ctx->videoSink,
                     ctx->audioQueue, ctx->audioConvert, ctx->audioResample, ctx->audioCaps, ctx->audioSink,
                     nullptr);
    if (!gst_element_link_many(ctx->videoQueue, ctx->videoConvert, videoScale,
                               ctx->videoCaps, ctx->videoSink, nullptr) ||
        !gst_element_link_many(ctx->audioQueue, ctx->audioConvert, ctx->audioResample,
                               ctx->audioCaps, ctx->audioSink, nullptr)) {
        LOGE("Media branch linking failed");
        destroyPipeline(ctx);
        return false;
    }

    g_signal_connect(ctx->source, "pad-added", G_CALLBACK(onPadAdded), ctx);
    g_signal_connect(ctx->source, "source-setup", G_CALLBACK(onSourceSetup), ctx);
    /* [V92 PREVIEWDIAG] both uridecodebin (child-added) and uridecodebin3
     * (child-added) expose the internal elements; harmless if never emitted. */
    g_signal_connect(ctx->source, "child-added", G_CALLBACK(onChildAdded), ctx);
    g_signal_connect(ctx->videoSink, "new-sample", G_CALLBACK(onVideoSample), ctx);
    g_signal_connect(ctx->audioSink, "new-sample", G_CALLBACK(onAudioSample), ctx);

    GstStateChangeReturn state = gst_element_set_state(ctx->pipeline, GST_STATE_PLAYING);
    if (state == GST_STATE_CHANGE_FAILURE) {
        LOGE("Media pipeline failed to enter PLAYING for %s", uri.c_str());
        destroyPipeline(ctx);
        return false;
    }
    LDECI("[buildPipeline] Media pipeline running uri=%s", uri.c_str());
    return true;
}

static void runDecoder(DecoderCtx* ctx) {
    /* [V91] every AmkushDecoder.open() starts a fresh runDecoder thread, so
     * transport retry state is reset here (not in buildPipeline, which also
     * runs on mid-session restarts). */
    ctx->transportAttempt.store(0, std::memory_order_release);
    /* [V93 RETRYCAP] log14 (SM-X200, V91) proved the old policy was unbounded:
     * every bus ERROR set restart=true, so a camera that refused the session
     * produced 972 pipeline rebuilds and 960 error callbacks in 47 minutes,
     * peaking at 212 failures/minute. Four leaked preview decoders doing this
     * exhausted the camera's session limit and took the injected stream down
     * with them. Retries are now capped and exponentially backed off. */
    static const int  kMaxDecodeRetries = 4;
    static const long kBackoffBaseMs    = 1000;
    static const long kBackoffMaxMs     = 15000;
    bool giveUp = false;
    long backoffMs = kBackoffBaseMs;
    while (ctx->running.load() && !giveUp) {
        if (!ctx->pipeline && !buildPipeline(ctx)) {
            fireError(ctx, -1, "Unable to create media pipeline");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        GstBus* bus = gst_element_get_bus(ctx->pipeline);
        bool restart = false;
        /* [V91] anchor for the no-frame watchdog (set when the pipeline is up). */
        auto buildStartedAt = std::chrono::steady_clock::now();
        long lastHeartbeatMs = 0;   /* [V92 PREVIEWDIAG] */
        LDECI("[PREVIEW] ---- session start uri=%s transport=%s live=%d ----",
              ctx->url.c_str(),
              ctx->rtspPreferTcp.load(std::memory_order_relaxed) ? "TCP" : "UDP",
              isLiveUri(ctx->url) ? 1 : 0);
        /* [V94] !giveUp added: in V93 the no-frame watchdog set giveUp but only
         * the OUTER loop tested it, so after "reporting preview failure" the
         * thread sat in this inner loop emitting heartbeats for the rest of the
         * session (log14: 01:34:24 -> 01:41:44, over 7 minutes). */
        while (ctx->running.load() && !restart && !giveUp) {
            if (ctx->hotSwapping.load()) {
                std::lock_guard<std::mutex> lock(ctx->swapMu);
                ctx->url = ctx->hotSwapUrl;
                ctx->hotSwapping.store(false);
                LDECI("[runDecoder] hot swap -> %s", ctx->url.c_str());
                restart = true;
                break;
            }

            /* [V92 PREVIEWDIAG] was filtered to ERROR|EOS only, so a preview
             * that stalled silently (RTSP handshake fine, media never flowing)
             * produced ZERO log lines. Every message type is now surfaced —
             * WARNING in particular is where rtspsrc reports timeouts. */
            GstMessage* message = gst_bus_timed_pop_filtered(
                bus, 100 * GST_MSECOND,
                static_cast<GstMessageType>(GST_MESSAGE_ANY));
            if (!message) {
                /* [V92 PREVIEWDIAG] heartbeat: proof of life plus the exact
                 * state of every stage, every 2 s. This is what turns "the log
                 * just stops" into an answer. */
                const long nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                if (nowMs - lastHeartbeatMs >= 2000) {
                    lastHeartbeatMs = nowMs;
                    GstState st = GST_STATE_NULL, pend = GST_STATE_VOID_PENDING;
                    gst_element_get_state(ctx->pipeline, &st, &pend, 0);
                    const long vFrames = ctx->videoFrames.load(std::memory_order_relaxed);
                    const long lastV   = ctx->lastVideoMs.load(std::memory_order_relaxed);
                    LDECI("[PREVIEW] heartbeat vFrames=%ld aFrames=%ld pads=%d state=%s pending=%s "
                          "transport=%s sourceSetup=%d busMsgs=%ld lastFrameAgo=%ldms uri=%s",
                          vFrames,
                          ctx->audioFrames.load(std::memory_order_relaxed),
                          ctx->padsAdded.load(std::memory_order_relaxed),
                          gst_element_state_get_name(st),
                          gst_element_state_get_name(pend),
                          ctx->rtspPreferTcp.load(std::memory_order_relaxed) ? "TCP" : "UDP",
                          ctx->sawSourceSetup.load(std::memory_order_relaxed) ? 1 : 0,
                          ctx->busMessages.load(std::memory_order_relaxed),
                          (vFrames > 0 && lastV > 0) ? (nowMs - lastV) : -1L,
                          ctx->url.c_str());
                }
                /* [V91] NO-FRAME WATCHDOG. A live RTSP source can sit in
                 * PLAYING with a negotiated session and never deliver a single
                 * RTP packet (UDP black-hole) — no bus message, no error, the
                 * UI just spins "Connecting…" forever. Watch the decoded-frame
                 * counter: no frame 7 s after the pipeline entered PLAYING →
                 * retry on the other transport, then surface a real error. */
                if (isLiveUri(ctx->url)) {
                    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - buildStartedAt).count();
                    /* [V94] was 7000ms. log14 V93: the video pad appeared at
                     * 6.96s and was linked at 6.98s, then the watchdog fired at
                     * 7.24s and tore down a pipeline that was about to deliver.
                     * The producer already learned this lesson for the same
                     * reason (kFirstFrameWatchdogUs = 8s). */
                    if (elapsedMs >= 12000 && ctx->videoFrames.load(std::memory_order_relaxed) == 0) {
                        std::string scheme;
                        const auto sep = ctx->url.find("://");
                        if (sep != std::string::npos) scheme = ctx->url.substr(0, sep);
                        for (char& c : scheme)
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        const bool isRtsp = (scheme == "rtsp" || scheme == "rtsps");
                        const int attempt = ctx->transportAttempt.load(std::memory_order_acquire);

                        if (isRtsp && attempt == 0) {
                            ctx->rtspPreferTcp.store(false, std::memory_order_release);
                            ctx->transportAttempt.store(1, std::memory_order_release);
                            ctx->loggedFirstVideo.store(false);
                            LDECE("[V94] no frame for 12000ms on TCP — retrying preview on UDP uri=%s",
                                  ctx->url.c_str());
                            restart = true;
                        } else if (attempt < 2) {
                            ctx->transportAttempt.store(2, std::memory_order_release);
                            LDECE("[V91] no frame on %s — reporting preview failure uri=%s",
                                  isRtsp ? "TCP and UDP" : scheme.c_str(), ctx->url.c_str());
                            fireError(ctx, -2,
                                      "Stream connected but no video arrived. "
                                      "Check the URL, the encoder and the network.");
                            /* [V93 RETRYCAP] the old code rebuilt the pipeline
                             * here too, and with transportAttempt already at 2
                             * the next generation's watchdog matched no branch —
                             * so it silently rebuilt forever. Fail once, stop. */
                            giveUp = true;
                        }
                        buildStartedAt = std::chrono::steady_clock::now();
                    }
                }
                continue;
            }

            ctx->busMessages.fetch_add(1, std::memory_order_relaxed);

            /* [V92 PREVIEWDIAG] Log every message, but keep the noise down:
             * QOS/PROGRESS/LATENCY spam, and STATE_CHANGED is only interesting
             * for the pipeline itself (not for each of the 12 children). */
            /* [V92 PREVIEWDIAG] Deliberately an if/else chain and not a switch
             * over every GST_MESSAGE_* constant: some of those enum values only
             * exist in newer GStreamer releases and naming them here would break
             * the build. Everything uninteresting simply falls through. */
            const GstMessageType mtype = GST_MESSAGE_TYPE(message);
            if (mtype == GST_MESSAGE_STATE_CHANGED) {
                /* Only the pipeline itself — the 12 children would spam. */
                if (GST_MESSAGE_SRC(message) == GST_OBJECT(ctx->pipeline)) {
                    GstState os = GST_STATE_NULL, ns = GST_STATE_NULL,
                             pd = GST_STATE_VOID_PENDING;
                    gst_message_parse_state_changed(message, &os, &ns, &pd);
                    LDECI("[PREVIEW] pipeline state %s -> %s (pending %s)",
                          gst_element_state_get_name(os),
                          gst_element_state_get_name(ns),
                          gst_element_state_get_name(pd));
                }
            } else if (mtype == GST_MESSAGE_WARNING) {
                /* This is where rtspsrc reports connect/receive timeouts — the
                 * exact case that used to be completely silent. */
                GError* w = nullptr; gchar* wdbg = nullptr;
                gst_message_parse_warning(message, &w, &wdbg);
                LDECE("[PREVIEW] WARNING domain=%s code=%d msg=%s%s%s",
                      w ? g_quark_to_string(w->domain) : "?",
                      w ? w->code : 0,
                      w ? w->message : "unknown",
                      wdbg ? " | " : "", wdbg ? wdbg : "");
                if (w) g_error_free(w);
                if (wdbg) g_free(wdbg);
            } else if (mtype == GST_MESSAGE_ASYNC_DONE) {
                LDECI("[PREVIEW] ASYNC_DONE - preroll complete, pipeline is live");
            } else if (mtype == GST_MESSAGE_STREAM_START) {
                LDECI("[PREVIEW] STREAM_START");
            } else if (mtype == GST_MESSAGE_ELEMENT) {
                const GstStructure* st = gst_message_get_structure(message);
                gchar* stStr = st ? gst_structure_to_string(st) : nullptr;
                LDECI("[PREVIEW] ELEMENT msg=%s", stStr ? stStr : "(none)");
                if (stStr) g_free(stStr);
            }

            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError* error = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                LOGE("Media error: %s%s%s", error ? error->message : "unknown",
                     debug ? " (" : "", debug ? debug : "");
                if (error) fireError(ctx, error->code, error->message);
                const char* edomain = error ? g_quark_to_string(error->domain) : "?";
                const int   ecode   = error ? error->code : 0;
                if (error) g_error_free(error);
                if (debug) g_free(debug);

                const int errs = ctx->consecutiveErrors.fetch_add(1, std::memory_order_relaxed) + 1;
                if (errs >= kMaxDecodeRetries) {
                    LDECE("[PREVIEW] GIVING UP after %d consecutive failures (%s code=%d) uri=%s",
                          errs, edomain, ecode, ctx->url.c_str());
                    fireError(ctx, -3,
                              "Stopped retrying: the stream failed repeatedly. "
                              "Another app may already be using this camera.");
                    giveUp = true;
                } else {
                    LDECE("[PREVIEW] failure %d/%d (%s code=%d) - rebuilding in %ldms uri=%s",
                          errs, kMaxDecodeRetries, edomain, ecode, backoffMs, ctx->url.c_str());
                    restart = true;
                }
            } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
                fireEof(ctx);
                // Local files and still images loop; live streams reconnect.
                restart = true;
            }
            gst_message_unref(message);
        }
        if (bus) gst_object_unref(bus);
        destroyPipeline(ctx);
        if (giveUp) break;
        /* [V93 RETRYCAP] exponential backoff, slept in slices so close() is
         * never made to wait out a long pause. */
        if (ctx->running.load()) {
            const long waitMs = restart ? backoffMs : 150;
            for (long slept = 0; slept < waitMs && ctx->running.load(); slept += 100)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            backoffMs = std::min(kBackoffMaxMs, backoffMs * 2);
        }
    }
    if (giveUp) LDECI("[PREVIEW] decoder thread exiting (gave up) uri=%s", ctx->url.c_str());
    destroyPipeline(ctx);
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;

    gst_init(nullptr, nullptr);
    if (gst_android_load_gio_modules) gst_android_load_gio_modules();

    jclass local = env->FindClass("com/itsme/amkush/media/AmkushDecoder$FrameCallback");
    if (!local) return JNI_ERR;
    g_frameCallbackClass = static_cast<jclass>(env->NewGlobalRef(local));
    g_onFrameAvailable = env->GetMethodID(g_frameCallbackClass, "onFrameAvailable",
        "(Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;IIJ)V");
    g_onError = env->GetMethodID(g_frameCallbackClass, "onError", "(ILjava/lang/String;)V");
    g_onEof = env->GetMethodID(g_frameCallbackClass, "onEof", "()V");
    g_onAudioFrame = env->GetMethodID(g_frameCallbackClass, "onAudioFrameWithPts",
        "(Ljava/nio/ByteBuffer;IIIJ)V");
    g_methodsCached = g_onFrameAvailable && g_onError && g_onEof && g_onAudioFrame;
    env->DeleteLocalRef(local);
    if (!g_methodsCached) return JNI_ERR;
    LDECI("JNI_OnLoad: Amkush decoder initialized and callback methods cached");
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_itsme_amkush_media_AmkushDecoder_open(JNIEnv* env, jclass,
                                                       jstring urlString, jobject callback) {
    if (!g_methodsCached || !urlString || !callback) return 0;
    const char* raw = env->GetStringUTFChars(urlString, nullptr);
    if (!raw) return 0;
    auto* ctx = new DecoderCtx();
    ctx->url = raw;
    ctx->callback = env->NewGlobalRef(callback);
    env->ReleaseStringUTFChars(urlString, raw);
    if (!buildPipeline(ctx)) {
        env->DeleteGlobalRef(ctx->callback);
        delete ctx;
        return 0;
    }
    ctx->thread = std::thread(runDecoder, ctx);
    return reinterpret_cast<jlong>(ctx);
}

extern "C" JNIEXPORT void JNICALL
Java_com_itsme_amkush_media_AmkushDecoder_close(JNIEnv* env, jclass, jlong handle) {
    auto* ctx = reinterpret_cast<DecoderCtx*>(handle);
    if (!ctx) return;
    ctx->running.store(false);
    if (ctx->thread.joinable()) ctx->thread.join();
    destroyPipeline(ctx);
    if (ctx->callback) env->DeleteGlobalRef(ctx->callback);
    delete ctx;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_itsme_amkush_media_AmkushDecoder_hotSwap(JNIEnv* env, jclass,
                                                          jlong handle, jstring urlString) {
    auto* ctx = reinterpret_cast<DecoderCtx*>(handle);
    if (!ctx || !ctx->running.load() || !urlString) return JNI_FALSE;
    const char* raw = env->GetStringUTFChars(urlString, nullptr);
    if (!raw) return JNI_FALSE;
    {
        std::lock_guard<std::mutex> lock(ctx->swapMu);
        ctx->hotSwapUrl = raw;
        ctx->hotSwapping.store(true);
    }
    env->ReleaseStringUTFChars(urlString, raw);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_itsme_amkush_media_AmkushDecoder_getWidth(JNIEnv*, jclass, jlong handle) {
    auto* ctx = reinterpret_cast<DecoderCtx*>(handle);
    return ctx ? ctx->width.load() : 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_itsme_amkush_media_AmkushDecoder_getHeight(JNIEnv*, jclass, jlong handle) {
    auto* ctx = reinterpret_cast<DecoderCtx*>(handle);
    return ctx ? ctx->height.load() : 0;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_itsme_amkush_media_AmkushDecoder_isUsingHardwareDecoder(JNIEnv*, jclass, jlong handle) {
    auto* ctx = reinterpret_cast<DecoderCtx*>(handle);
    return ctx ? (ctx->usingHwAccel.load() ? JNI_TRUE : JNI_FALSE) : JNI_FALSE;
}

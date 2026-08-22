#include <jni.h>
#include <android/log.h>
#include <atomic>
#include <thread>
#include <string>
#include <mutex>
#include <chrono>
#include <cstring>
#include <cctype>
#include <limits.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/jni.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/frame.h>
#include <libavutil/buffer.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#define LOG_TAG     "FFmpegDecoder"
#define DECODER_TAG "DECODER"
#define LOGI(...)  __android_log_print(ANDROID_LOG_INFO,  LOG_TAG,     __VA_ARGS__)
#define LOGE(...)  __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,     __VA_ARGS__)
#define LOGD(...)  __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG,     __VA_ARGS__)
#define LDECI(...) __android_log_print(ANDROID_LOG_INFO,  DECODER_TAG, __VA_ARGS__)
#define LDECE(...) __android_log_print(ANDROID_LOG_ERROR, DECODER_TAG, __VA_ARGS__)
#define LDECD(...) __android_log_print(ANDROID_LOG_DEBUG, DECODER_TAG, __VA_ARGS__)
#define FG_TAG  "FACEGATE"
#define LOGFG(...) __android_log_print(ANDROID_LOG_DEBUG, FG_TAG, __VA_ARGS__)

static void ffmpeg_log_callback(void* ptr, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_WARNING) return;

    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, vl);

    if (strstr(buf, "unable to decode APP fields") != nullptr) return;

    switch (level) {
        case AV_LOG_ERROR:
            LOGE("FFmpeg: %s", buf);
            LDECE("[ffmpeg] %s", buf);
            break;
        case AV_LOG_WARNING:
            LOGI("FFmpeg: %s", buf);
            LDECI("[ffmpeg] %s", buf);
            break;
        default:
            LOGD("FFmpeg: %s", buf);
            break;
    }
}

static JavaVM*   g_jvm              = nullptr;

static jclass    g_frameCallbackClass = nullptr;
static jmethodID g_onFrameAvailable   = nullptr;
static jmethodID g_onError            = nullptr;
static jmethodID g_onEof              = nullptr;
static jmethodID g_onAudioFrame       = nullptr;
static bool      g_methodsCached      = false;

struct DecoderCtx {
    std::string          url;
    std::atomic<bool>    running{true};
    std::atomic<bool>    hotSwapping{false};
    std::string          hotSwapUrl;
    std::mutex           swapMu;
    jobject              callback = nullptr;

    std::atomic<int>     width{0};
    std::atomic<int>     height{0};
    std::atomic<int>     sampleRate{0};
    std::atomic<int>     audioChannels{0};

    AVRational           timeBase{0, 1};
    AVRational           audioTimeBase{0, 1};
    int                  srcFmt = -1;


    AVFormatContext*     fmtCtx    = nullptr;
    AVCodecContext*      codecCtx  = nullptr;
    SwsContext*          swsCtx    = nullptr;
    int                  videoIdx  = -1;
    AVFrame*             frame     = nullptr;
    AVFrame*             frameI420 = nullptr;
    AVPacket*            packet    = nullptr;


    AVCodecContext*      audioCodecCtx = nullptr;
    SwrContext*          swrCtx        = nullptr;
    int                  audioIdx      = -1;
    AVFrame*             audioFrame    = nullptr;
    uint8_t*             audioBuffer   = nullptr;
    int                  audioBufferSize = 0;

    bool                 usingHwAccel = false;



    std::atomic<bool>    abortIo{false};

    std::thread          thread;
};

static int ffmpeg_interrupt_cb(void* opaque) {
    auto* ctx = static_cast<DecoderCtx*>(opaque);
    return (ctx && ctx->abortIo.load()) ? 1 : 0;
}

static JNIEnv* attachCurrentThread(bool& didAttach) {
    didAttach = false;
    JNIEnv* env = nullptr;
    jint res = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (res == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) == JNI_OK) {
            didAttach = true;
        } else {
            env = nullptr;
        }
    }
    return env;
}

static void detachCurrentThread() {
    g_jvm->DetachCurrentThread();
}

static void closeStream(DecoderCtx* ctx);

static void buildDemuxerOpts(AVDictionary** opts, const std::string& url) {
    std::string scheme;
    const auto sep = url.find("://");
    const std::string raw = (sep != std::string::npos) ? url.substr(0, sep) : url;
    scheme.resize(raw.size());
    for (size_t i = 0; i < raw.size(); ++i)
        scheme[i] = static_cast<char>(::tolower(static_cast<unsigned char>(raw[i])));

    if (scheme == "rtsp" || scheme == "rtsps") {
        av_dict_set(opts, "rtsp_transport", "tcp",     0);
        av_dict_set(opts, "stimeout",       "5000000", 0);
    } else if (scheme == "http" || scheme == "https") {
        av_dict_set(opts, "timeout",            "5000000", 0);
        av_dict_set(opts, "reconnect",          "1",       0);
        av_dict_set(opts, "reconnect_streamed", "1",       0);
        av_dict_set(opts, "reconnect_delay_max","5",       0);
    } else if (scheme == "rtmp" || scheme == "rtmps") {
        av_dict_set(opts, "rtmp_live",   "-1", 0);
        av_dict_set(opts, "tcp_nodelay", "1",  0);
    } else if (scheme == "srt") {
        av_dict_set(opts, "latency", "200000", 0);
    }

    av_dict_set(opts, "analyzeduration", "3000000", 0);
    av_dict_set(opts, "probesize",       "1000000", 0);
}

static const AVCodec* findBestDecoder(enum AVCodecID codec_id, bool& hwAccel) {
    hwAccel = false;
    LDECD("[findBestDecoder] codec_id=%d", static_cast<int>(codec_id));

    const char* hwDecoderName = nullptr;
    switch (codec_id) {
        case AV_CODEC_ID_H264:  hwDecoderName = "h264_mediacodec";  break;
        case AV_CODEC_ID_HEVC:  hwDecoderName = "hevc_mediacodec";  break;
        case AV_CODEC_ID_VP8:   hwDecoderName = "vp8_mediacodec";   break;
        case AV_CODEC_ID_VP9:   hwDecoderName = "vp9_mediacodec";   break;
        default: break;
    }

    if (hwDecoderName) {
        const AVCodec* hwCodec = avcodec_find_decoder_by_name(hwDecoderName);
        if (hwCodec) {
            LOGI("Using hardware decoder: %s", hwDecoderName);
            LDECI("[findBestDecoder] hw=%s  codec_id=%d", hwDecoderName, static_cast<int>(codec_id));
            hwAccel = true;
            return hwCodec;
        }
        LOGI("Hardware decoder %s not available, falling back to software", hwDecoderName);
        LDECI("[findBestDecoder] hw=%s unavailable — sw fallback", hwDecoderName);
    }

    const AVCodec* swCodec = avcodec_find_decoder(codec_id);
    if (swCodec) {
        LOGI("Using software decoder: %s", swCodec->name);
        LDECI("[findBestDecoder] sw=%s  codec_id=%d", swCodec->name, static_cast<int>(codec_id));
    } else {
        LOGE("No decoder found for codec_id=%d", static_cast<int>(codec_id));
        LDECE("[findBestDecoder] no decoder for codec_id=%d", static_cast<int>(codec_id));
    }
    return swCodec;
}

static bool openStream(DecoderCtx* ctx) {
    LDECI("[openStream] url=%s", ctx->url.c_str());
    ctx->fmtCtx = avformat_alloc_context();
    if (!ctx->fmtCtx) {
        LOGE("avformat_alloc_context failed");
        LDECE("[openStream] avformat_alloc_context failed");
        return false;
    }


    ctx->fmtCtx->interrupt_callback.callback = ffmpeg_interrupt_cb;
    ctx->fmtCtx->interrupt_callback.opaque   = ctx;

    AVDictionary* opts = nullptr;
    buildDemuxerOpts(&opts, ctx->url);

    int ret = avformat_open_input(&ctx->fmtCtx, ctx->url.c_str(), nullptr, &opts);
    av_dict_free(&opts);

    if (ret < 0) {
        char err[256];
        av_strerror(ret, err, sizeof(err));
        LOGE("avformat_open_input: %s  url=%s", err, ctx->url.c_str());
        avformat_close_input(&ctx->fmtCtx);
        ctx->fmtCtx = nullptr;
        return false;
    }

    if (avformat_find_stream_info(ctx->fmtCtx, nullptr) < 0) {
        LOGE("avformat_find_stream_info failed");
        avformat_close_input(&ctx->fmtCtx);
        ctx->fmtCtx = nullptr;
        return false;
    }


    ctx->videoIdx = av_find_best_stream(ctx->fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (ctx->videoIdx >= 0) {
        AVStream* vStream = ctx->fmtCtx->streams[ctx->videoIdx];
        bool hwAccel = false;
        const AVCodec* codec = findBestDecoder(vStream->codecpar->codec_id, hwAccel);

        if (codec) {
            ctx->codecCtx = avcodec_alloc_context3(codec);
            if (ctx->codecCtx) {
                if (avcodec_parameters_to_context(ctx->codecCtx, vStream->codecpar) >= 0) {
                    if (!hwAccel) {
                        ctx->codecCtx->thread_count = 2;
                        ctx->codecCtx->thread_type  = FF_THREAD_SLICE;
                    }
                    if (avcodec_open2(ctx->codecCtx, codec, nullptr) >= 0) {
                        ctx->usingHwAccel = hwAccel;
                        ctx->timeBase = vStream->time_base;
                        LOGI("Video stream opened: %dx%d codec=%s hw=%s",
                             ctx->codecCtx->width, ctx->codecCtx->height,
                             codec->name, hwAccel ? "YES" : "NO");
                    } else {
                        LOGE("avcodec_open2 (video) failed");
                        avcodec_free_context(&ctx->codecCtx);
                    }
                } else {
                    LOGE("avcodec_parameters_to_context (video) failed");
                    avcodec_free_context(&ctx->codecCtx);
                }
            }
        } else {
            LOGE("No video decoder for codec_id=%d", vStream->codecpar->codec_id);
        }
    } else {
        LOGI("No video stream found");
    }


    ctx->audioIdx = av_find_best_stream(ctx->fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (ctx->audioIdx >= 0) {
        AVStream* aStream = ctx->fmtCtx->streams[ctx->audioIdx];
        const AVCodec* aCodec = avcodec_find_decoder(aStream->codecpar->codec_id);
        if (aCodec) {
            ctx->audioCodecCtx = avcodec_alloc_context3(aCodec);
            if (ctx->audioCodecCtx) {
                if (avcodec_parameters_to_context(ctx->audioCodecCtx, aStream->codecpar) >= 0) {
                    if (avcodec_open2(ctx->audioCodecCtx, aCodec, nullptr) >= 0) {
                        ctx->audioTimeBase = aStream->time_base;
                        LOGI("Audio stream opened: sr=%d ch=%d fmt=%s codec=%s",
                             ctx->audioCodecCtx->sample_rate,
                             ctx->audioCodecCtx->ch_layout.nb_channels,
                             av_get_sample_fmt_name(ctx->audioCodecCtx->sample_fmt),
                             aCodec->name);
                    } else {
                        LOGE("avcodec_open2 (audio) failed");
                        avcodec_free_context(&ctx->audioCodecCtx);
                    }
                } else {
                    LOGE("avcodec_parameters_to_context (audio) failed");
                    avcodec_free_context(&ctx->audioCodecCtx);
                }
            }
        } else {
            LOGE("No audio decoder for codec_id=%d", aStream->codecpar->codec_id);
        }
    } else {
        LOGI("No audio stream found");
    }

    if (ctx->videoIdx < 0 && ctx->audioIdx < 0) {
        LOGE("No video or audio stream found");
        avformat_close_input(&ctx->fmtCtx);
        ctx->fmtCtx = nullptr;
        return false;
    }


    ctx->packet = av_packet_alloc();


    if (ctx->videoIdx >= 0 && ctx->codecCtx) {
        ctx->frame     = av_frame_alloc();
        ctx->frameI420 = av_frame_alloc();
    }


    if (ctx->audioIdx >= 0 && ctx->audioCodecCtx) {
        ctx->audioFrame = av_frame_alloc();
    }

    if (!ctx->packet ||
        (ctx->videoIdx >= 0 && ctx->codecCtx && (!ctx->frame || !ctx->frameI420)) ||
        (ctx->audioIdx >= 0 && ctx->audioCodecCtx && !ctx->audioFrame)) {
        LOGE("frame/packet alloc failed");
        closeStream(ctx);
        return false;
    }

    if (ctx->codecCtx) {
        ctx->width.store(ctx->codecCtx->width);
        ctx->height.store(ctx->codecCtx->height);
    }
    if (ctx->audioCodecCtx) {
        ctx->sampleRate.store(ctx->audioCodecCtx->sample_rate);
        ctx->audioChannels.store(ctx->audioCodecCtx->ch_layout.nb_channels);
    }
    ctx->srcFmt = -1;

    LOGI("Stream opened successfully: url=%s video=%d audio=%d",
         ctx->url.c_str(), ctx->videoIdx, ctx->audioIdx);
    return true;
}

static void closeStream(DecoderCtx* ctx) {
    if (ctx->packet)    { av_packet_free(&ctx->packet);    ctx->packet = nullptr; }
    if (ctx->frame)     { av_frame_free(&ctx->frame);      ctx->frame = nullptr; }
    if (ctx->frameI420) { av_frame_free(&ctx->frameI420);  ctx->frameI420 = nullptr; }
    if (ctx->swsCtx)    { sws_freeContext(ctx->swsCtx);    ctx->swsCtx = nullptr; }
    if (ctx->codecCtx)  { avcodec_free_context(&ctx->codecCtx); ctx->codecCtx = nullptr; }

    if (ctx->audioFrame)    { av_frame_free(&ctx->audioFrame);    ctx->audioFrame = nullptr; }
    if (ctx->audioBuffer)   { av_freep(&ctx->audioBuffer);        ctx->audioBuffer = nullptr; ctx->audioBufferSize = 0; }
    if (ctx->swrCtx)        { swr_free(&ctx->swrCtx);             ctx->swrCtx = nullptr; }
    if (ctx->audioCodecCtx) { avcodec_free_context(&ctx->audioCodecCtx); ctx->audioCodecCtx = nullptr; }

    if (ctx->fmtCtx)    { avformat_close_input(&ctx->fmtCtx);   ctx->fmtCtx = nullptr; }

    ctx->videoIdx = -1;
    ctx->audioIdx = -1;
    ctx->width.store(0);
    ctx->height.store(0);
    ctx->sampleRate.store(0);
    ctx->audioChannels.store(0);
    ctx->srcFmt = -1;
    ctx->usingHwAccel = false;
}

static void fireOnFrame(JNIEnv* env, jobject cb, AVFrame* f, jlong ptsUs) {
    const int w      = f->width;
    const int h      = f->height;
    const int ySize  = w * h;
    const int uvW    = (w + 1) / 2;
    const int uvH    = (h + 1) / 2;
    const int uvSize = uvW * uvH;

    jobject yBuf = env->NewDirectByteBuffer(f->data[0], ySize);
    jobject uBuf = env->NewDirectByteBuffer(f->data[1], uvSize);
    jobject vBuf = env->NewDirectByteBuffer(f->data[2], uvSize);

    if (!yBuf || !uBuf || !vBuf) {
        LOGE("fireOnFrame: NewDirectByteBuffer failed");
        if (yBuf) env->DeleteLocalRef(yBuf);
        if (uBuf) env->DeleteLocalRef(uBuf);
        if (vBuf) env->DeleteLocalRef(vBuf);
        return;
    }

    env->CallVoidMethod(cb, g_onFrameAvailable, yBuf, uBuf, vBuf,
                        static_cast<jint>(w), static_cast<jint>(h), ptsUs);

    env->DeleteLocalRef(yBuf);
    env->DeleteLocalRef(uBuf);
    env->DeleteLocalRef(vBuf);

    if (env->ExceptionCheck()) {
        LOGE("fireOnFrame: onFrameAvailable threw a Java exception");
        env->ExceptionClear();
    }
}

static void fireOnAudio(JNIEnv* env, jobject cb, const uint8_t* pcmData, int sizeBytes,
                        int sampleRate, int channels, int samplesPerChannel, jlong ptsUs) {
    jobject pcmBuf = env->NewDirectByteBuffer(const_cast<uint8_t*>(pcmData), sizeBytes);
    if (!pcmBuf) {
        LOGE("fireOnAudio: NewDirectByteBuffer failed");
        return;
    }

    env->CallVoidMethod(cb, g_onAudioFrame, pcmBuf,
                        static_cast<jint>(sampleRate),
                        static_cast<jint>(channels),
                        static_cast<jint>(samplesPerChannel),
                        ptsUs);

    env->DeleteLocalRef(pcmBuf);

    if (env->ExceptionCheck()) {
        LOGE("fireOnAudio: onAudioFrameWithPts threw a Java exception");
        env->ExceptionClear();
    }
}

static AVFrame* ensureI420(DecoderCtx* ctx, AVFrame* src) {



    AVFrame* hwFrame = nullptr;
    if (src->hw_frames_ctx != nullptr) {
        hwFrame = av_frame_alloc();
        if (!hwFrame) {
            LOGE("ensureI420: av_frame_alloc (hw→cpu transfer) failed");
            return nullptr;
        }
        if (av_hwframe_transfer_data(hwFrame, src, 0) < 0) {
            LOGE("ensureI420: av_hwframe_transfer_data failed — codec surface not CPU-accessible");
            av_frame_free(&hwFrame);
            return nullptr;
        }
        hwFrame->pts = src->pts;
        src = hwFrame;
    }

    const int w      = src->width;
    const int h      = src->height;
    const int srcFmt = src->format;

    const bool needsRebuild = !ctx->swsCtx                   ||
                              ctx->frameI420->width  != w     ||
                              ctx->frameI420->height != h     ||
                              ctx->srcFmt            != srcFmt;

    if (needsRebuild) {
        if (ctx->swsCtx) {
            sws_freeContext(ctx->swsCtx);
            ctx->swsCtx = nullptr;
        }

        ctx->swsCtx = sws_getContext(
            w, h, static_cast<AVPixelFormat>(srcFmt),
            w, h, AV_PIX_FMT_YUV420P,
            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!ctx->swsCtx) {
            LOGE("ensureI420: sws_getContext failed");
            if (hwFrame) av_frame_free(&hwFrame);
            return nullptr;
        }

        av_frame_unref(ctx->frameI420);

        const int uvW     = (w + 1) / 2;
        const int uvH     = (h + 1) / 2;
        const int ySize   = w * h;
        const int uvSize  = uvW * uvH;
        const int total   = ySize + 2 * uvSize;

        AVBufferRef* buf = av_buffer_alloc(total);
        if (!buf) {
            LOGE("ensureI420: av_buffer_alloc(%d) failed", total);
            if (hwFrame) av_frame_free(&hwFrame);
            return nullptr;
        }

        ctx->frameI420->buf[0]  = buf;
        ctx->frameI420->data[0] = buf->data;
        ctx->frameI420->data[1] = buf->data + ySize;
        ctx->frameI420->data[2] = buf->data + ySize + uvSize;

        ctx->frameI420->linesize[0] = w;
        ctx->frameI420->linesize[1] = uvW;
        ctx->frameI420->linesize[2] = uvW;

        ctx->frameI420->format = AV_PIX_FMT_YUV420P;
        ctx->frameI420->width  = w;
        ctx->frameI420->height = h;
        ctx->srcFmt            = srcFmt;

        ctx->width.store(w);
        ctx->height.store(h);
    }

    sws_scale(ctx->swsCtx,
              src->data,         src->linesize,  0, h,
              ctx->frameI420->data, ctx->frameI420->linesize);
    ctx->frameI420->pts = src->pts;
    if (hwFrame) av_frame_free(&hwFrame);
    return ctx->frameI420;
}

static bool ensureSwr(DecoderCtx* ctx) {
    if (ctx->swrCtx) return true;

    AVChannelLayout outLayout;
    av_channel_layout_default(&outLayout, ctx->audioCodecCtx->ch_layout.nb_channels);

    int ret = swr_alloc_set_opts2(
        &ctx->swrCtx,
        &outLayout,
        AV_SAMPLE_FMT_S16,
        ctx->audioCodecCtx->sample_rate,
        &ctx->audioCodecCtx->ch_layout,
        ctx->audioCodecCtx->sample_fmt,
        ctx->audioCodecCtx->sample_rate,
        0, nullptr);

    av_channel_layout_uninit(&outLayout);

    if (ret < 0) {
        LOGE("swr_alloc_set_opts2 failed");
        return false;
    }

    ret = swr_init(ctx->swrCtx);
    if (ret < 0) {
        LOGE("swr_init failed");
        swr_free(&ctx->swrCtx);
        return false;
    }

    ctx->sampleRate.store(ctx->audioCodecCtx->sample_rate);
    ctx->audioChannels.store(ctx->audioCodecCtx->ch_layout.nb_channels);
    return true;
}

static void processAudioFrame(DecoderCtx* ctx, JNIEnv* env, jobject cb, AVFrame* decodedFrame) {
    if (!ensureSwr(ctx)) return;


    int64_t outSamples = av_rescale_rnd(
        swr_get_delay(ctx->swrCtx, ctx->audioCodecCtx->sample_rate) + decodedFrame->nb_samples,
        ctx->audioCodecCtx->sample_rate, ctx->audioCodecCtx->sample_rate, AV_ROUND_UP);

    int outChannels = ctx->audioCodecCtx->ch_layout.nb_channels;
    int bytesPerSample = av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);
    int outSize = static_cast<int>(outSamples) * outChannels * bytesPerSample;

    if (outSize > ctx->audioBufferSize) {




        int newSize = outSize + 65536;
        uint8_t* newBuf = static_cast<uint8_t*>(av_realloc(ctx->audioBuffer, newSize));
        if (!newBuf) {
            LOGE("av_realloc(%d) failed — dropping audio frame", newSize);
            return;
        }
        ctx->audioBuffer     = newBuf;
        ctx->audioBufferSize = newSize;
    }

    uint8_t* outData[1] = { ctx->audioBuffer };
    int converted = swr_convert(ctx->swrCtx, outData, static_cast<int>(outSamples),
                                const_cast<const uint8_t**>(decodedFrame->data), decodedFrame->nb_samples);

    if (converted > 0) {
        int deliveredBytes = converted * outChannels * bytesPerSample;



        jlong audioPtsUs = 0;
        if (decodedFrame->pts != AV_NOPTS_VALUE) {
            audioPtsUs = av_rescale_q(decodedFrame->pts,
                                      ctx->audioTimeBase, {1, 1000000});
        } else if (decodedFrame->best_effort_timestamp != AV_NOPTS_VALUE) {
            audioPtsUs = av_rescale_q(decodedFrame->best_effort_timestamp,
                                      ctx->audioTimeBase, {1, 1000000});
        }

        fireOnAudio(env, cb, ctx->audioBuffer, deliveredBytes,
                    ctx->audioCodecCtx->sample_rate, outChannels, converted, audioPtsUs);
    }
}

static void decodeLoop(DecoderCtx* ctx) {
    bool    didAttach = false;
    JNIEnv* env       = attachCurrentThread(didAttach);
    if (!env) {
        LOGE("decodeLoop: failed to attach to JVM");
        LDECE("[decodeLoop] JVM attach failed");
        return;
    }
    LDECI("[decodeLoop] thread started  url=%s", ctx->url.c_str());

    while (ctx->running.load()) {
        if (ctx->hotSwapping.load()) {
            std::string newUrl;
            {
                std::lock_guard<std::mutex> lk(ctx->swapMu);
                newUrl = ctx->hotSwapUrl;
                ctx->hotSwapping.store(false);
            }
            LOGI("hot-swap -> %s", newUrl.c_str());
            LDECI("[decodeLoop] hotSwap  old=%s  new=%s", ctx->url.c_str(), newUrl.c_str());
            closeStream(ctx);
            ctx->url = newUrl;

            while (ctx->running.load() && !openStream(ctx)) {
                LOGE("hot-swap openStream failed - retry in 2 s");
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            if (!ctx->running.load()) break;
        }

        int ret = av_read_frame(ctx->fmtCtx, ctx->packet);

        if (ret == AVERROR_EOF) {
            env->CallVoidMethod(ctx->callback, g_onEof);
            if (env->ExceptionCheck()) env->ExceptionClear();

            const bool isLocalFile =
                !ctx->url.empty() && (
                    ctx->url[0] == '/' ||
                    (ctx->url.size() >= 7 && ctx->url.substr(0, 7) == "file://")
                );

            const bool isLive =
                !isLocalFile && (
                    (ctx->fmtCtx->duration == AV_NOPTS_VALUE) ||
                    (ctx->url.size() >= 4 && ctx->url.substr(0, 4) == "rtsp") ||
                    (ctx->url.size() >= 4 && ctx->url.substr(0, 4) == "rtmp") ||
                    (ctx->url.size() >= 3 && ctx->url.substr(0, 3) == "udp") ||
                    (ctx->url.size() >= 3 && ctx->url.substr(0, 3) == "rtp") ||
                    (ctx->url.size() >= 3 && ctx->url.substr(0, 3) == "srt")
                );

            LDECD("[decodeLoop] EOF  isLocalFile=%d  isLive=%d  url=%s  duration=%lld",
                  isLocalFile, isLive, ctx->url.c_str(),
                  static_cast<long long>(ctx->fmtCtx ? ctx->fmtCtx->duration : -1));

            if (!isLive) {
                LDECD("[decodeLoop] file EOF - seeking to start for loop playback");
                avformat_seek_file(ctx->fmtCtx, -1, INT64_MIN, 0, INT64_MAX, 0);
                if (ctx->codecCtx) avcodec_flush_buffers(ctx->codecCtx);
                if (ctx->audioCodecCtx) avcodec_flush_buffers(ctx->audioCodecCtx);
            } else {
                LOGI("live stream EOF - reconnecting in 2 s");
                LDECI("[decodeLoop] live EOF - will reconnect url=%s", ctx->url.c_str());
                std::this_thread::sleep_for(std::chrono::seconds(2));
                closeStream(ctx);
                while (ctx->running.load() && !openStream(ctx)) {
                    LOGE("reconnect failed - retry in 3 s");
                    LDECE("[decodeLoop] reconnect failed url=%s", ctx->url.c_str());
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                }
                if (!ctx->running.load()) break;
            }
            continue;
        }

        if (ret == AVERROR(EAGAIN)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (ret < 0) {
            char errBuf[256];
            av_strerror(ret, errBuf, sizeof(errBuf));
            LOGE("av_read_frame: %s", errBuf);
            jstring msg = env->NewStringUTF(errBuf);
            env->CallVoidMethod(ctx->callback, g_onError, static_cast<jint>(ret), msg);
            if (msg) env->DeleteLocalRef(msg);
            if (env->ExceptionCheck()) env->ExceptionClear();

            closeStream(ctx);
            while (ctx->running.load() && !openStream(ctx)) {
                LOGE("Reconnect failed - retry in 3 s");
                std::this_thread::sleep_for(std::chrono::seconds(3));
            }
            if (!ctx->running.load()) break;
            continue;
        }


        if (ctx->packet->stream_index == ctx->videoIdx && ctx->codecCtx) {

            auto drainVideo = [&]() {
                while (ctx->running.load()) {
                    ret = avcodec_receive_frame(ctx->codecCtx, ctx->frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) {
                        char errBuf[256];
                        av_strerror(ret, errBuf, sizeof(errBuf));
                        LOGE("avcodec_receive_frame (video): %s", errBuf);
                        break;
                    }

                    AVFrame* i420 = ensureI420(ctx, ctx->frame);
                    if (i420) {
                        jlong ptsUs = 0;
                        if (ctx->frame->pts != AV_NOPTS_VALUE) {
                            ptsUs = av_rescale_q(ctx->frame->pts, ctx->timeBase, {1, 1000000});
                        }
                        fireOnFrame(env, ctx->callback, i420, ptsUs);
                    }
                    av_frame_unref(ctx->frame);
                }
            };

            ret = avcodec_send_packet(ctx->codecCtx, ctx->packet);
            if (ret == AVERROR(EAGAIN)) {




                drainVideo();
                ret = avcodec_send_packet(ctx->codecCtx, ctx->packet);
            }
            av_packet_unref(ctx->packet);

            if (ret < 0) {
                char errBuf[256];
                av_strerror(ret, errBuf, sizeof(errBuf));
                LOGE("avcodec_send_packet (video): %s", errBuf);
                continue;
            }

            drainVideo();
        }

        else if (ctx->packet->stream_index == ctx->audioIdx && ctx->audioCodecCtx) {
            auto drainAudio = [&]() {
                while (ctx->running.load()) {
                    ret = avcodec_receive_frame(ctx->audioCodecCtx, ctx->audioFrame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) {
                        char errBuf[256];
                        av_strerror(ret, errBuf, sizeof(errBuf));
                        LOGE("avcodec_receive_frame (audio): %s", errBuf);
                        break;
                    }

                    processAudioFrame(ctx, env, ctx->callback, ctx->audioFrame);
                    av_frame_unref(ctx->audioFrame);
                }
            };

            ret = avcodec_send_packet(ctx->audioCodecCtx, ctx->packet);
            if (ret == AVERROR(EAGAIN)) {

                drainAudio();
                ret = avcodec_send_packet(ctx->audioCodecCtx, ctx->packet);
            }
            av_packet_unref(ctx->packet);

            if (ret < 0) {
                char errBuf[256];
                av_strerror(ret, errBuf, sizeof(errBuf));
                LOGE("avcodec_send_packet (audio): %s", errBuf);
                continue;
            }

            drainAudio();
        }
        else {
            av_packet_unref(ctx->packet);
        }
    }

    if (didAttach) detachCurrentThread();
}

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;

    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {


        return JNI_ERR;
    }




    if (av_jni_set_java_vm(vm, nullptr) < 0) {
        LOGE("JNI_OnLoad: av_jni_set_java_vm failed - hardware decoders will not work");
        LDECE("[JNI_OnLoad] av_jni_set_java_vm failed");
    } else {
        LOGI("JNI_OnLoad: JVM registered with FFmpeg MediaCodec layer");
        LDECI("[JNI_OnLoad] JVM registered - hardware decoders enabled");
    }

    av_log_set_callback(ffmpeg_log_callback);





    jclass localIface = env->FindClass("com/itsme/amkush/ffmpeg/FFmpegDecoder$FrameCallback");
    if (localIface) {
        g_frameCallbackClass = static_cast<jclass>(env->NewGlobalRef(localIface));

        g_onFrameAvailable = env->GetMethodID(
            g_frameCallbackClass, "onFrameAvailable",
            "(Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;Ljava/nio/ByteBuffer;IIJ)V");
        g_onError = env->GetMethodID(
            g_frameCallbackClass, "onError", "(ILjava/lang/String;)V");
        g_onEof = env->GetMethodID(
            g_frameCallbackClass, "onEof", "()V");



        g_onAudioFrame = env->GetMethodID(
            g_frameCallbackClass, "onAudioFrameWithPts",
            "(Ljava/nio/ByteBuffer;IIIJ)V");

        g_methodsCached = (g_onFrameAvailable && g_onError && g_onEof && g_onAudioFrame);
        if (!g_methodsCached) {
            LOGE("JNI_OnLoad: FAILED to cache one or more FrameCallback method IDs");
            LDECE("[JNI_OnLoad] Method ID cache FAILED");
        } else {
            LDECI("[JNI_OnLoad] FrameCallback method IDs cached from interface OK");
        }

        env->DeleteLocalRef(localIface);
    } else {
        LOGE("JNI_OnLoad: Could not find FFmpegDecoder$FrameCallback interface class");
        LDECE("[JNI_OnLoad] FrameCallback interface not found");
    }

    return JNI_VERSION_1_6;
}

extern "C"
JNIEXPORT jlong JNICALL
Java_com_itsme_amkush_ffmpeg_FFmpegDecoder_open(
    JNIEnv* env, jclass, jstring urlStr, jobject cb)
{
    if (!g_methodsCached) {
        LOGE("open: JNI method IDs not cached - cannot proceed");
        return 0;
    }

    const char* url = env->GetStringUTFChars(urlStr, nullptr);
    if (!url) return 0;

    auto* ctx = new DecoderCtx();
    ctx->url      = url;
    ctx->callback = env->NewGlobalRef(cb);
    env->ReleaseStringUTFChars(urlStr, url);

    if (!openStream(ctx)) {
        LOGE("open: openStream failed for url=%s", ctx->url.c_str());
        env->DeleteGlobalRef(ctx->callback);
        delete ctx;
        return 0;
    }

    ctx->thread = std::thread(decodeLoop, ctx);
    return reinterpret_cast<jlong>(ctx);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_itsme_amkush_ffmpeg_FFmpegDecoder_close(
    JNIEnv* env, jclass, jlong handle)
{
    auto* ctx = reinterpret_cast<DecoderCtx*>(handle);
    if (!ctx) return;

    ctx->running.store(false);


    ctx->abortIo.store(true);
    if (ctx->thread.joinable()) ctx->thread.join();

    closeStream(ctx);

    if (ctx->callback) {
        env->DeleteGlobalRef(ctx->callback);
        ctx->callback = nullptr;
    }
    delete ctx;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_itsme_amkush_ffmpeg_FFmpegDecoder_hotSwap(
    JNIEnv* env, jclass, jlong handle, jstring urlStr)
{
    auto* ctx = reinterpret_cast<DecoderCtx*>(handle);
    if (!ctx || !ctx->running.load()) return JNI_FALSE;

    const char* url = env->GetStringUTFChars(urlStr, nullptr);
    if (!url) return JNI_FALSE;

    {
        std::lock_guard<std::mutex> lk(ctx->swapMu);
        ctx->hotSwapUrl = url;
        ctx->hotSwapping.store(true);
    }
    env->ReleaseStringUTFChars(urlStr, url);
    return JNI_TRUE;
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_itsme_amkush_ffmpeg_FFmpegDecoder_getWidth(
    JNIEnv*, jclass, jlong handle)
{
    auto* ctx = reinterpret_cast<DecoderCtx*>(handle);
    return ctx ? ctx->width.load() : 0;
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_itsme_amkush_ffmpeg_FFmpegDecoder_getHeight(
    JNIEnv*, jclass, jlong handle)
{
    auto* ctx = reinterpret_cast<DecoderCtx*>(handle);
    return ctx ? ctx->height.load() : 0;
}

extern "C"
JNIEXPORT jboolean JNICALL
Java_com_itsme_amkush_ffmpeg_FFmpegDecoder_isUsingHardwareDecoder(
    JNIEnv*, jclass, jlong handle)
{
    auto* ctx = reinterpret_cast<DecoderCtx*>(handle);
    return ctx ? ctx->usingHwAccel : false;
}

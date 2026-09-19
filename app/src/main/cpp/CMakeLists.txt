cmake_minimum_required(VERSION 3.22.1)
project("facegate" LANGUAGES C CXX)

# GStreamer Android SDK built by the CI workflow with Cerbero.
if(NOT DEFINED GSTREAMER_ROOT)
    if(DEFINED ENV{GSTREAMER_ROOT})
        set(GSTREAMER_ROOT "$ENV{GSTREAMER_ROOT}")
    else()
        message(FATAL_ERROR "GSTREAMER_ROOT is not set. Configure gstreamer.dir in local.properties.")
    endif()
endif()
if(NOT EXISTS "${GSTREAMER_ROOT}")
    message(FATAL_ERROR "GStreamer SDK not found at: ${GSTREAMER_ROOT}")
endif()
# CI stores one SDK per Android ABI below the artifact root.
if(EXISTS "${GSTREAMER_ROOT}/${ANDROID_ABI}/share/cmake")
    set(GSTREAMER_ROOT "${GSTREAMER_ROOT}/${ANDROID_ABI}")
endif()
if(NOT EXISTS "${GSTREAMER_ROOT}/share/cmake")
    message(FATAL_ERROR "GStreamer SDK for ${ANDROID_ABI} not found below ${GSTREAMER_ROOT}")
endif()

list(APPEND CMAKE_MODULE_PATH "${GSTREAMER_ROOT}/share/cmake")
set(GStreamer_ROOT_DIR "${GSTREAMER_ROOT}")
set(GStreamer_USE_STATIC_LIBS ON)
set(GSTREAMER_NDK_BUILD_PATH "${GSTREAMER_ROOT}/share/gst-android/ndk-build")

# GStreamer is statically registered into a mobile bridge library. These
# categories provide uridecodebin, RTSP/RTP, local containers, Android
# MediaCodec decoding, videoconvert/videoscale, and app/audio/video APIs.
include("${GSTREAMER_NDK_BUILD_PATH}/plugins.cmake")
# [V73] RTSP + RTMP only. Owner directive: drop every other streaming
# protocol. The Cerbero groups previously linked the whole _NET set
# (dtls, netsim, rist, rtmp2, rtpmanagerbad, rtponvif, sctp, sdpelem,
# srtp, srt, webrtc, soup/http) plus the streaming demuxers that hide in
# _CODECS (hls, dash, smoothstreaming, rfbsrc) — none usable by the
# producer. Explicit minimal net list: rtspsrc needs rtp (depayloaders),
# rtpmanager (jitterbuffer) and udp (RTP transport); tcp kept for
# interleaved; rtmp = the librtmp-based plugin for rtmp:// sources.
# CODECS keeps videoparsersbad (h264parse) and androidmedia (MediaCodec);
# only the streaming demuxers are trimmed from it.
# [V77] NET is back to the FULL Cerbero group. The V73 explicit minimal
# list (rtsp rtp rtpmanager udp tcp rtmp) shipped on the Tab A8 as: RTSP
# negotiated caps + delivered exactly ONE frame per generation, then the
# freeze watchdog looped forever (14.unisoc.1 log) — something in the
# dropped net plugins (dtls/srtp/rtprtx*/sdpelem/...) is part of the
# rtspsrc receive path after all. Correctness beats a few MB: only the
# streaming DEMUXERS stay removed, which is what "no hls etc." means.
set(GSTREAMER_PLUGINS_CODECS_TRIMMED ${GSTREAMER_PLUGINS_CODECS})
list(REMOVE_ITEM GSTREAMER_PLUGINS_CODECS_TRIMMED
     hls dash smoothstreaming rfbsrc)
set(GSTREAMER_PLUGINS
    ${GSTREAMER_PLUGINS_CORE}
    ${GSTREAMER_PLUGINS_PLAYBACK}
    ${GSTREAMER_PLUGINS_CODECS_TRIMMED}
    ${GSTREAMER_PLUGINS_CODECS_RESTRICTED}
    ${GSTREAMER_PLUGINS_CODECS_GPL}
    ${GSTREAMER_PLUGINS_NET}
)
set(GStreamer_EXTRA_DEPS
    gstreamer-app-1.0
    gstreamer-audio-1.0
    gstreamer-video-1.0
    gstreamer-pbutils-1.0
    gstreamer-rtp-1.0
    gstreamer-rtsp-1.0
    gstreamer-sdp-1.0
    glib-2.0
)
set(G_IO_MODULES openssl)
# Use the module's uppercase inputs so its Gradle-specific path handling
# does not reinterpret these already-absolute destinations.
set(GSTREAMER_JAVA_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../java")
set(GSTREAMER_ASSETS_DIR "${CMAKE_CURRENT_LIST_DIR}/../assets")
# [V83 stealth-fix] V82 tried set_target_properties(OUTPUT_NAME) AFTER
# find_package — silently ignored because the module itself sets
# LIBRARY_OUTPUT_NAME (which wins over OUTPUT_NAME). The module's documented
# knob is GStreamer_Mobile_MODULE_NAME, honoured only when defined BEFORE
# find_package (its own defaulting is `if(NOT DEFINED ...)`). Result:
# libgstreamer_android.so → libamkush.so, and consumers' DT_NEEDED picks the
# new name up automatically because they link the CMake target.
set(GStreamer_Mobile_MODULE_NAME "amkush")
find_package(GStreamerMobile COMPONENTS
    ${GSTREAMER_PLUGINS}
    ca_certificates
    REQUIRED
)

# The generated Android bridge source includes GLib headers directly. The
# GStreamerMobile module exposes GStreamer include paths to consumers, but
# does not apply GLib's generated include path to its own source target.
target_include_directories(GStreamerMobile PRIVATE
    "${GSTREAMER_ROOT}/include/glib-2.0"
    "${GSTREAMER_ROOT}/lib/glib-2.0/include"
)

# Cerbero installs the plugin archives but does not generate one pkg-config
# module per plugin. The current FindGStreamerMobile module consumes those
# modules to create its plugin targets, so the CI staging step supplies the
# small metadata files. Keep every built static archive in the mobile bridge's
# link group as well: plugin-specific dependencies (RTMP, SRT, image codecs,
# OpenSSL, and their transitive libraries) are not all in gstreamer-1.0.pc.
file(GLOB GSTREAMER_PREFIX_STATIC_LIBS CONFIGURE_DEPENDS
    "${GSTREAMER_ROOT}/lib/*.a"
    "${GSTREAMER_ROOT}/lib/gstreamer-1.0/*.a"
    "${GSTREAMER_ROOT}/lib/gio/modules/*.a"
)
if(GSTREAMER_PREFIX_STATIC_LIBS)
    target_link_libraries(GStreamerMobile PRIVATE
        "-Wl,--start-group"
        ${GSTREAMER_PREFIX_STATIC_LIBS}
        "-Wl,--end-group"
    )
endif()

# libyuv is retained for the camera-surface conversion boundary. No legacy
# media scaling libraries are linked.
if(NOT DEFINED LIBYUV_ROOT)
    if(DEFINED ENV{LIBYUV_ROOT})
        set(LIBYUV_ROOT "$ENV{LIBYUV_ROOT}")
    else()
        message(FATAL_ERROR "LIBYUV_ROOT is not set.")
    endif()
endif()
add_library(yuv STATIC IMPORTED)
set_target_properties(yuv PROPERTIES
    IMPORTED_LOCATION "${LIBYUV_ROOT}/${ANDROID_ABI}/libyuv.a"
    INTERFACE_INCLUDE_DIRECTORIES "${LIBYUV_ROOT}/include"
)

find_library(log-lib log REQUIRED)
find_library(android-lib android REQUIRED)
find_library(z-lib z REQUIRED)
find_library(egl-lib EGL REQUIRED)
find_library(glesv2-lib GLESv2 REQUIRED)

# GStreamerMobile statically includes the GL backend from the full Cerbero
# prefix, so its Android system EGL/GLES dependencies must be explicit.
target_link_libraries(GStreamerMobile PRIVATE
    ${android-lib} ${egl-lib} ${glesv2-lib}
)

# GStreamer decoder used by InjectionService and stream preview.
add_library(amkush_decoder SHARED amkush_decoder.cpp)
target_compile_options(amkush_decoder PRIVATE
    -O3 -fPIC -ffast-math -frtti -fexceptions -DNDEBUG
)
target_link_libraries(amkush_decoder
    GStreamer::mobile
    ${log-lib} ${android-lib} ${z-lib}
)
target_compile_features(amkush_decoder PRIVATE cxx_std_14)

# I420/NV12/RGBA conversion at the Java/camera boundary.
add_library(libyuv_wrapper SHARED libyuv_wrapper.cpp)
target_compile_options(libyuv_wrapper PRIVATE
    -O3 -fPIC -ffast-math -frtti -fexceptions -DNDEBUG
)
target_link_libraries(libyuv_wrapper
    yuv ${log-lib} ${android-lib}
)
target_compile_features(libyuv_wrapper PRIVATE cxx_std_14)

# Native producer: GStreamer runs in the application process and feeds the
# existing ashmem ring/socket contract consumed by the camera hook.
add_library(frame_producer SHARED
    amkush_frame_producer.cpp
    activation.cpp
)
target_compile_options(frame_producer PRIVATE
    -O2 -fPIC -fno-sanitize=cfi -DNDEBUG
)

# FRAME_BUILD_ID — git branch + commit SHA injected by the CI workflow so the
# producer logs prove which build is actually running. Falls back to the
# HOOK_PROXY_VERSION-style tag if provided, else "dev-unknown".
if(DEFINED FRAME_BUILD_ID AND NOT FRAME_BUILD_ID STREQUAL "")
    target_compile_definitions(frame_producer PRIVATE
        FRAME_BUILD_ID="${FRAME_BUILD_ID}")
elseif(DEFINED HOOK_PROXY_VERSION AND NOT HOOK_PROXY_VERSION STREQUAL "")
    target_compile_definitions(frame_producer PRIVATE
        FRAME_BUILD_ID="${HOOK_PROXY_VERSION}")
else()
    target_compile_definitions(frame_producer PRIVATE
        FRAME_BUILD_ID="dev-unknown")
endif()
target_link_libraries(frame_producer
    GStreamer::mobile
    ${log-lib} ${android-lib} ${z-lib}
)
target_compile_features(frame_producer PRIVATE cxx_std_17)

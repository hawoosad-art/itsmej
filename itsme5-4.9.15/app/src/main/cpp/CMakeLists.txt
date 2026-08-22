cmake_minimum_required(VERSION 3.22.1)
  project("facegate" CXX)

  # ── FFmpeg prebuilt root directory ────────────────────────────────────────────
  if(NOT DEFINED FFMPEG_ROOT)
      if(DEFINED ENV{FFMPEG_ROOT})
          set(FFMPEG_ROOT $ENV{FFMPEG_ROOT})
      else()
          set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/../ffmpeg")
          message(WARNING "FFMPEG_ROOT not set — defaulting to ${FFMPEG_ROOT}")
      endif()
  endif()

  set(FFMPEG_ABI_DIR "${FFMPEG_ROOT}/${ANDROID_ABI}")

  if(NOT EXISTS "${FFMPEG_ABI_DIR}/lib")
      message(FATAL_ERROR
          "FFmpeg libraries not found at: ${FFMPEG_ABI_DIR}/lib\n"
          "Download prebuilts from the ffmpeg-builder workflow or set "
          "ffmpeg.dir in local.properties.")
  endif()

  # ── OpenSSL prebuilt root directory ───────────────────────────────────────────
  if(NOT DEFINED OPENSSL_ROOT)
      if(DEFINED ENV{OPENSSL_ROOT})
          set(OPENSSL_ROOT $ENV{OPENSSL_ROOT})
      else()
          set(OPENSSL_ROOT "${CMAKE_SOURCE_DIR}/../openssl-android")
          message(WARNING "OPENSSL_ROOT not set — defaulting to ${OPENSSL_ROOT}")
      endif()
  endif()

  set(OPENSSL_ABI_DIR "${OPENSSL_ROOT}/${ANDROID_ABI}")

  # ── FFmpeg prebuilt static libs ───────────────────────────────────────────────
  foreach(_lib avformat avcodec avutil swresample swscale)
      add_library(${_lib} STATIC IMPORTED)
      set_target_properties(${_lib} PROPERTIES
          IMPORTED_LOCATION "${FFMPEG_ABI_DIR}/lib/lib${_lib}.a"
      )
  endforeach()

  # ── OpenSSL prebuilt static libs ──────────────────────────────────────────────
  add_library(ssl STATIC IMPORTED)
  set_target_properties(ssl PROPERTIES
      IMPORTED_LOCATION "${OPENSSL_ABI_DIR}/lib/libssl.a"
  )

  add_library(crypto STATIC IMPORTED)
  set_target_properties(crypto PROPERTIES
      IMPORTED_LOCATION "${OPENSSL_ABI_DIR}/lib/libcrypto.a"
  )

  # ── System libraries ──────────────────────────────────────────────────────────
  find_library(log-lib     log)
  find_library(android-lib android)
  find_library(z-lib       z)

  # ── ffmpeg_decoder.so ─────────────────────────────────────────────────────────
  add_library(ffmpeg_decoder SHARED ffmpeg_decoder.cpp)

  target_include_directories(ffmpeg_decoder PRIVATE
      "${FFMPEG_ABI_DIR}/include"
  )

  target_compile_options(ffmpeg_decoder PRIVATE
      -O3 -fPIC -ffast-math -frtti -fexceptions -DNDEBUG
  )

  target_link_libraries(ffmpeg_decoder
      avformat avcodec avutil swresample swscale
      ssl crypto
      mediandk ${z-lib} ${log-lib} ${android-lib}
  )

  target_compile_features(ffmpeg_decoder PRIVATE cxx_std_14)

  # ── libyuv_wrapper.so ─────────────────────────────────────────────────────────
  add_library(libyuv_wrapper SHARED swscale_wrapper.cpp)

  target_include_directories(libyuv_wrapper PRIVATE
      "${FFMPEG_ABI_DIR}/include"
  )

  target_compile_options(libyuv_wrapper PRIVATE
      -O3 -fPIC -ffast-math -frtti -fexceptions -DNDEBUG
  )

  target_link_libraries(libyuv_wrapper
      swscale avutil ${log-lib} ${android-lib}
  )

  target_compile_features(libyuv_wrapper PRIVATE cxx_std_14)

  # ── frame_producer.so ─────────────────────────────────────────────────────────
  # Includes activation.cpp: native license enforcement + JNI HTTP + anti-tamper.
  # Includes license/license_client.cpp: HMAC-verified license client (APK side).
  # URLs are XOR-obfuscated; no plaintext server address in the binary.
  add_library(frame_producer SHARED
      frame_producer.cpp
      activation.cpp
      license/license_client.cpp
  )

  target_include_directories(frame_producer PRIVATE
      "${FFMPEG_ABI_DIR}/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/license"
  )

  target_compile_options(frame_producer PRIVATE
      -O2 -fPIC -fno-sanitize=cfi -DNDEBUG
  )

  target_link_libraries(frame_producer
      avformat avcodec avutil swresample swscale
      ssl crypto
      mediandk ${z-lib} ${log-lib} ${android-lib}
  )

  target_compile_features(frame_producer PRIVATE cxx_std_17)

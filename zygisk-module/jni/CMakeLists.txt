cmake_minimum_required(VERSION 3.22.1)
project("amkush_hook" CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Compiler flags ────────────────────────────────────────────────────────────
# -static-libstdc++: bake the entire C++ STL into libhookProxy.so so it is
# self-contained and injects cleanly into any process namespace.
#
# Root cause of the injection failure WITHOUT this flag:
#   dlopen failed: library "libc++_shared.so" not found, needed by libhookProxy.so
# cameraserver runs in a restricted linker namespace that cannot resolve the
# app's libc++_shared.so, so the dynamic C++ STL dependency must be eliminated.
set(CMAKE_CXX_FLAGS          "${CMAKE_CXX_FLAGS} -fno-sanitize=cfi -fvisibility=hidden -fno-exceptions -fno-rtti -Wall -Wno-unused-parameter -Wno-unused-command-line-argument -static-libstdc++")
set(CMAKE_C_FLAGS            "${CMAKE_C_FLAGS}   -fno-sanitize=cfi -fvisibility=hidden -Wall")
set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -static-libstdc++")

# ── Source files ────────────────────────────────────────────────────────────
# NOTE: main.cpp no longer includes zygisk.hpp. The Zygisk AmkushModule and
# REGISTER_ZYGISK_MODULE have been removed. This library is now injected
# exclusively via ptrace (amkush_injector / AndKittyInjector --pid).
# NOTE: license/license_client.cpp was REMOVED from the hook build. It was
# dead code (never called by any hook source) and it embedded the HMAC
# secret + plaintext server URL into libhookProxy.so — a binary that runs
# inside cameraserver. License validation lives app-side only.
set(HOOK_SOURCES
    main.cpp
    hook_proxy.cpp
    symbol_resolver.cpp
    stream_map.cpp
    frame_source.cpp
    frame_inject.cpp
    crash_guard.cpp
    ipc_socket.cpp
)

add_library(hookProxy SHARED ${HOOK_SOURCES})

target_include_directories(hookProxy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# ── Version marker ───────────────────────────────────────────────────────────
# HOOK_PROXY_VERSION is injected at build time by the CI workflow as a CMake
# -D variable (e.g. -DHOOK_PROXY_VERSION="V4.8.5.4-a4c45e7"), derived from the
# git branch + commit so it is always accurate and never needs a manual bump.
# main.cpp falls back to "dev-unknown" if this is not provided.
if(DEFINED HOOK_PROXY_VERSION AND NOT HOOK_PROXY_VERSION STREQUAL "")
    target_compile_definitions(hookProxy PRIVATE
        HOOK_PROXY_VERSION="${HOOK_PROXY_VERSION}")
endif()

# ── System libraries ────────────────────────────────────────────────────────
find_library(LOG_LIB     log)
find_library(ANDROID_LIB android)

# std::vector and all other C++ STL symbols are satisfied by -static-libstdc++
# above. libc++_shared.so is intentionally NOT linked dynamically.
target_link_libraries(hookProxy ${LOG_LIB} ${ANDROID_LIB})

# ── ShadowHook — STATIC ONLY ─────────────────────────────────────────────────
# ShadowHook MUST be linked statically because cameraserver's linker namespace
# does not include the app's lib paths. A shared libshadowhook.so would fail
# to dlopen in cameraserver's context.
#
# ShadowHook only supports armeabi-v7a and arm64-v8a. x86_64 is intentionally
# excluded.
#
# CONFIRMED: the Maven Central AAR (shadowhook-2.0.1.aar) contains only .so
# files in its prefab/ and jni/ directories — NO .a files. Therefore the CI
# workflow builds ShadowHook from source (bytedance/android-inline-hook) with
# SHARED→STATIC patching before handing off to this CMakeLists.
if(DEFINED SHADOWHOOK_ROOT)
    message(STATUS "ShadowHook: prebuilt at ${SHADOWHOOK_ROOT}/${ANDROID_ABI}/")
    target_include_directories(hookProxy PRIVATE "${SHADOWHOOK_ROOT}/include")

    set(SH_STATIC "${SHADOWHOOK_ROOT}/${ANDROID_ABI}/libshadowhook.a")
    if(EXISTS "${SH_STATIC}")
        message(STATUS "ShadowHook: linking STATIC ${SH_STATIC}")
        add_library(sh_lib STATIC IMPORTED)
        set_target_properties(sh_lib PROPERTIES IMPORTED_LOCATION "${SH_STATIC}")
        target_link_libraries(hookProxy sh_lib)
    else()
        # The .so fallback has been intentionally removed.
        # If you see this error, the CI workflow did not produce libshadowhook.a.
        message(FATAL_ERROR
            "ShadowHook STATIC library not found: ${SH_STATIC}\n"
            "The shared-lib fallback has been removed — libhookProxy.so MUST link\n"
            "ShadowHook statically so it works inside cameraserver's linker namespace.\n"
            "Re-run the CI workflow to rebuild ShadowHook from source as a static lib.")
    endif()
else()
    # Gradle/Prefab path: rely on find_package (AAR must provide static variant).
    find_package(shadowhook REQUIRED CONFIG)
    target_link_libraries(hookProxy shadowhook::shadowhook)
    message(STATUS "ShadowHook: via find_package (prefab)")
endif()

# ── libyuv (MANDATORY — universal YUV plane handling) ────────────────────────
if(DEFINED LIBYUV_ROOT)
    message(STATUS "libyuv: prebuilt at ${LIBYUV_ROOT}")
    target_include_directories(hookProxy PRIVATE "${LIBYUV_ROOT}/include")
    add_library(yuv_lib STATIC IMPORTED)
    set_target_properties(yuv_lib PROPERTIES IMPORTED_LOCATION "${LIBYUV_ROOT}/${ANDROID_ABI}/libyuv.a")
    target_link_libraries(hookProxy yuv_lib)
else()
    find_library(LIBYUV_LIB yuv PATHS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libyuv/libs/${ANDROID_ABI})
    if(LIBYUV_LIB)
        target_link_libraries(hookProxy ${LIBYUV_LIB})
        target_include_directories(hookProxy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libyuv/include)
    else()
        message(FATAL_ERROR "libyuv not found — MANDATORY for frame_inject.cpp.\nSet LIBYUV_ROOT or place prebuilt at third_party/libyuv/")
    endif()
endif()

# ── libjpeg-turbo (MANDATORY — JPEG snapshot injection) ─────────────────────
if(DEFINED TURBOJPEG_ROOT)
    message(STATUS "turbojpeg: prebuilt at ${TURBOJPEG_ROOT}")
    target_include_directories(hookProxy PRIVATE "${TURBOJPEG_ROOT}/include")
    add_library(tj_lib STATIC IMPORTED)
    set_target_properties(tj_lib PROPERTIES IMPORTED_LOCATION "${TURBOJPEG_ROOT}/${ANDROID_ABI}/libturbojpeg.a")
    target_link_libraries(hookProxy tj_lib)
else()
    find_library(TJ_LIB turbojpeg)
    if(TJ_LIB)
        target_link_libraries(hookProxy ${TJ_LIB})
    else()
        message(FATAL_ERROR "turbojpeg not found — MANDATORY for frame_inject.cpp JPEG injection.\nSet TURBOJPEG_ROOT or ensure turbojpeg is in the system linker path.")
    endif()
endif()

set_target_properties(hookProxy PROPERTIES OUTPUT_NAME "hookProxy")
message(STATUS "hookProxy configured — ABI=${ANDROID_ABI} (Zygisk: REMOVED, ShadowHook: STATIC, C++STL: STATIC)")

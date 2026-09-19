

#include <android/log.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

#include "hook_proxy.h"
#include "frame_source.h"
#include "frame_inject.h"
#include "ipc_socket.h"
#include "crash_guard.h"

#define TAG "amkush/main"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* HOOK_PROXY_VERSION — a unique marker embedded in every build of libhookProxy.so.
 * It is set automatically at build time by the CI workflow from the git branch +
 * commit SHA (passed as -DHOOK_PROXY_VERSION="<branch>-<sha>"). If the build does
 * not pass it, we fall back to a placeholder so the marker is always present.
 * Logged at load time so you can confirm the .so running in cameraserver is the
 * fresh build and not an old/cached copy. */
#ifndef HOOK_PROXY_VERSION
#define HOOK_PROXY_VERSION "dev-unknown"
#endif

static atomic_int g_init_started = 0;

static void do_camera_hook_init(const char *via) {
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_init_started, &expected, 1)) {
        LOGI("hooks already initializing (via=%s) version=%s — skipping duplicate load (REBOOT needed for new build)", via, HOOK_PROXY_VERSION);
        return;
    }

    LOGI("=== amkush injection start  via=%s  pid=%d ===", via, (int)getpid());

    LOGI("[1/4] crash_guard_init ...");
    if (crash_guard_init() != 0) {
        LOGE("[1/4] crash_guard_init FAILED");
        atomic_store(&g_init_started, 0);
        return;
    }
    LOGI("[1/4] crash_guard_init OK");

    LOGI("[2/4] frame_inject_init ...");
    if (frame_inject_init() != 0) {
        LOGE("[2/4] frame_inject_init FAILED");
        crash_guard_cleanup();
        atomic_store(&g_init_started, 0);
        return;
    }
    LOGI("[2/4] frame_inject_init OK");

    LOGI("[3/4] hook_proxy_install ...");
    if (hook_proxy_install() != 0) {
        LOGE("[3/4] hook_proxy_install FAILED — virtual camera inactive");
        crash_guard_cleanup();
        frame_inject_destroy();
        atomic_store(&g_init_started, 0);
        return;
    }
    LOGI("[3/4] hook_proxy_install OK");

    LOGI("[4/4] ipc_socket_start ...");
    if (ipc_socket_start() != 0) {
        LOGE("[4/4] ipc_socket_start FAILED — cleaning up hooks");
        hook_proxy_uninstall();
        frame_inject_destroy();
        crash_guard_cleanup();
        atomic_store(&g_init_started, 0);
        return;
    }
    LOGI("[4/4] ipc_socket_start OK");

    LOGI("=== amkush virtual camera ACTIVE  via=%s ===", via);
}

#include <dirent.h>

static pid_t find_cameraserver_pid(void) {
    DIR *procdir = opendir("/proc");
    if (!procdir) return -1;

    struct dirent *ent;
    pid_t found = -1;
    while ((ent = readdir(procdir)) != nullptr) {
        const char *dname = ent->d_name;
        if (dname[0] < '1' || dname[0] > '9') continue;
        bool all_digits = true;
        for (int i = 0; dname[i]; i++) {
            if (dname[i] < '0' || dname[i] > '9') { all_digits = false; break; }
        }
        if (!all_digits) continue;

        char cmdline_path[64];
        snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%s/cmdline", dname);
        FILE *f = fopen(cmdline_path, "r");
        if (!f) continue;
        char cmdline[256];
        memset(cmdline, 0, sizeof(cmdline));
        size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, f);
        fclose(f);
        if (n == 0) continue;


        const char *base = cmdline;
        for (size_t i = 0; i < n; i++) {
            if (cmdline[i] == '/') base = cmdline + i + 1;
            if (cmdline[i] == '\0') break;
        }
        if (strcmp(base, "cameraserver") == 0) {
            found = (pid_t)atoi(dname);
            break;
        }
    }
    closedir(procdir);
    return found;
}

static bool verify_has_libcameraservice(pid_t pid) {
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", (int)pid);
    FILE *f = fopen(maps_path, "r");
    if (!f) return false;

    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "libcameraservice.so")) {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

static bool is_cameraserver_process(void) {
    char exe_path[256];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len > 0) {
        exe_path[len] = '\0';
        char *slash = strrchr(exe_path, '/');
        char *base  = slash ? slash + 1 : exe_path;
        bool match  = (strcmp(base, "cameraserver") == 0);
        LOGI("exe check: path='%s' base='%s' match=%d pid=%d",
             exe_path, base, (int)match, (int)getpid());
        return match;
    }
    LOGW("readlink /proc/self/exe failed: %s — pid=%d", strerror(errno), (int)getpid());
    return false;
}

static void *init_thread_main(void *) {
    do_camera_hook_init("ptrace");
    return nullptr;
}

__attribute__((constructor))
static void on_library_load(void) {
    LOGI("════════════════════════════════════════");
    LOGI("libhookProxy.so loaded  pid=%d  version=%s [V5-UNISOC-DELAY-ARTIFACT]", (int)getpid(), HOOK_PROXY_VERSION);
    LOGI("  if you see an OLD version here, the stale .so was loaded — "
         "not the fresh build (version=%s [V5] ) — REBOOT device to load new Zygisk module", HOOK_PROXY_VERSION);
    LOGI("════════════════════════════════════════");

    if (!is_cameraserver_process()) {
        LOGW("not cameraserver — skipping init (wrong process)");
        return;
    }


    if (!verify_has_libcameraservice((pid_t)getpid())) {
        /* Android 16 (API 36): cameraserver may no longer map
         * libcameraservice.so by that name, so the old hard abort left the
         * virtual camera dead. Proceed anyway — we already confirmed this
         * process IS cameraserver (exe name), and hook install does a full
         * ELF scan across the loaded libraries.
         *
         * IMPORTANT: do NOT sleep/retry here. This constructor runs
         * SYNCHRONOUSLY inside the injector's remote dlopen, whose call
         * timeout is 5000ms. A blocking loop (V4.9.12 tried 8s) makes the
         * injector time out and abandon/kill the target before hook init
         * can run — cameraserver stays alive but never installs hooks, so
         * the real camera still shows. Log and start the init thread
         * immediately (it is detached and does the slow work off the
         * constructor). */
        LOGW("libcameraservice.so not mapped by that name (Android 16?) — proceeding with full ELF scan anyway");
    } else {
        LOGI("confirmed cameraserver process with libcameraservice.so — starting init thread");
    }

    pthread_t t;
    int rc = pthread_create(&t, nullptr, init_thread_main, nullptr);
    if (rc != 0) {
        LOGE("pthread_create(init) failed: %s", strerror(rc));
        return;
    }
    pthread_detach(t);
}

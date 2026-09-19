

#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dirent.h>

#define POLL_INTERVAL_SEC      2
#define INJECT_VERIFY_WAIT_SEC 3
#define MAX_INJECT_FAILURES    5
#define SELINUX_ENFORCE_PATH   "/sys/fs/selinux/enforce"

static char g_injector[512];
static char g_hook_lib[512];
static char g_log_path[512];
static char g_stop_path[512];


    #define WLOG_TAG "amkush/watchdog"
    static void wlog(const char *fmt, ...) __attribute__((format(printf,1,2)));
    static void wlog(const char *fmt, ...) {
      va_list ap; va_start(ap, fmt);
      __android_log_vprint(ANDROID_LOG_INFO, WLOG_TAG, fmt, ap);
      va_end(ap);
    }
    static void wlogw(const char *fmt, ...) __attribute__((format(printf,1,2)));
    static void wlogw(const char *fmt, ...) {
      va_list ap; va_start(ap, fmt);
      __android_log_vprint(ANDROID_LOG_WARN, WLOG_TAG, fmt, ap);
      va_end(ap);
    }
    static void wloge(const char *fmt, ...) __attribute__((format(printf,1,2)));
    static void wloge(const char *fmt, ...) {
      va_list ap; va_start(ap, fmt);
      __android_log_vprint(ANDROID_LOG_ERROR, WLOG_TAG, fmt, ap);
      va_end(ap);
    }

static pid_t get_cameraserver_pid(void) {
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *e;

    while ((e = readdir(d)) != NULL) {
        pid_t pid = (pid_t)atoi(e->d_name);
        if (pid <= 0) continue;

        char cmdline_path[64];
        snprintf(cmdline_path, sizeof(cmdline_path), "/proc/%d/cmdline", pid);
        FILE *f = fopen(cmdline_path, "r");
        if (!f) continue;
        char buf[256];
        memset(buf, 0, sizeof(buf));
        fread(buf, 1, sizeof(buf)-1, f);
        fclose(f);


        char *last_slash = strrchr(buf, '/');
        char *base = last_slash ? last_slash + 1 : buf;


        char *end = base + strlen(base) - 1;
        while (end > base && (*end == ' ' || *end == '\0' || *end == '\n')) {
            *end = '\0';
            end--;
        }


        if (strcmp(base, "cameraserver") == 0) {
            closedir(d);
            return pid;
        }
    }
    closedir(d);
    return 0;
}

/* V4.9.16 liveness probe: is the hook actually serving frames?
 * The abstract AF_UNIX socket "\0amkush_frame_fd" is bound ONLY by libhookProxy,
 * and only once its detached init thread has finished. A hook mapping alone does
 * not prove the injection works — if init fails, the .so stays mapped but nothing
 * binds the socket, and frame_producer gets "Connection refused". Abstract sockets
 * appear in /proc/net/unix prefixed with '@'. Returns:
 *   1  live     0  confirmed absent   -1  unreadable (unknown) */
static int hook_socket_live(void) {
    FILE *f = fopen("/proc/net/unix", "r");
    if (!f) return -1;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "@amkush_frame_fd")) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static int is_hooked(pid_t pid) {
    int sock = hook_socket_live();
    if (sock > 0) return 1;         /* IPC socket live → hook is serving frames */
    if (sock == 0) return 0;        /* socket confirmed absent → dead, re-inject */
    /* sock < 0: /proc/net/unix unreadable → fall back to the old maps heuristic */
    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
    FILE *f = fopen(maps_path, "r");
    if (!f) return 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "hookProxy")) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

static int selinux_get_enforce(void) {
    FILE *f = fopen(SELINUX_ENFORCE_PATH, "r");
    if (!f) return -1;
    int v = -1;
    fscanf(f, "%d", &v);
    fclose(f);
    return v;
}
static void selinux_set_enforce(int val) {
    FILE *f = fopen(SELINUX_ENFORCE_PATH, "w");
    if (!f) return;
    fprintf(f, "%d\n", val);
    fclose(f);
}

static int run_injector(pid_t target_pid) {
    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d", target_pid);


    const char *argv[] = {
        g_injector,
        "--pid",  pid_str,
        "--libs", g_hook_lib,
        "--memfd",
        "--timeout", "5000",
        NULL
    };



      int pipefd[2] = { -1, -1 };
      if (pipe(pipefd) < 0) {
          wloge("pipe() failed: %s", strerror(errno));

      }

      pid_t child = fork();
      if (child < 0) {
          wloge("fork() failed: %s", strerror(errno));
          if (pipefd[0] >= 0) { close(pipefd[0]); close(pipefd[1]); }
          return -1;
      }
      if (child == 0) {

          if (pipefd[1] >= 0) {
              close(pipefd[0]);
              dup2(pipefd[1], STDOUT_FILENO);
              dup2(pipefd[1], STDERR_FILENO);
              close(pipefd[1]);
          } else {
              int devnull = open("/dev/null", O_WRONLY);
              if (devnull >= 0) {
                  dup2(devnull, STDOUT_FILENO);
                  dup2(devnull, STDERR_FILENO);
                  close(devnull);
              }
          }
          execv(g_injector, (char *const *)argv);
          _exit(127);
      }


      if (pipefd[1] >= 0) close(pipefd[1]);
      if (pipefd[0] >= 0) {
          char linebuf[512];
          int  pos = 0;
          char c;
          ssize_t n;
          while ((n = read(pipefd[0], &c, 1)) == 1) {
              if (c == '\n' || pos >= (int)sizeof(linebuf) - 1) {
                  linebuf[pos] = '\0';
                  if (pos > 0)
                      __android_log_print(ANDROID_LOG_INFO, "amkush/injector",
                                          "%s", linebuf);
                  pos = 0;
              } else {
                  linebuf[pos++] = c;
              }
          }
          if (pos > 0) {
              linebuf[pos] = '\0';
              __android_log_print(ANDROID_LOG_INFO, "amkush/injector", "%s", linebuf);
          }
          close(pipefd[0]);
      }

      int status = 0;
      waitpid(child, &status, 0);
      return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

static int inject_with_selinux(pid_t pid) {
    int enforce = selinux_get_enforce();
    wlog("injecting PID=%d  SELinux=%s", pid,
         enforce == 1 ? "enforcing" : enforce == 0 ? "permissive" : "unknown");

    int rc = run_injector(pid);

    if (rc == 0 && is_hooked(pid)) {
        wlog("injection succeeded (SELinux=%d)", enforce);
        return 0;
    }

    if (enforce == 1) {
        wlogw("injection failed under enforcing SELinux (rc=%d) — retrying permissive", rc);
        selinux_set_enforce(0);
        usleep(200000);
        rc = run_injector(pid);
        selinux_set_enforce(1);
        wlog("SELinux restored to enforcing");

        if (rc == 0 && is_hooked(pid)) {
            wlog("injection succeeded after permissive window");
            return 0;
        }
        wloge("injection failed even in permissive (rc=%d)", rc);
    } else {
        wlogw("injection failed in permissive/unknown SELinux (rc=%d)", rc);
    }
    return -1;
}

static int should_stop(void) {
    return (access(g_stop_path, F_OK) == 0);
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr,
            "Usage: %s <injector_path> <hook_lib_path> <log_path>\n"
            "  injector_path  — path to amkush_injector64 or amkush_injector32\n"
            "  hook_lib_path  — path to libhookProxy.so\n"
            "  log_path       — file to write daemon logs into\n",
            argv[0]);
        return 1;
    }

    snprintf(g_injector,  sizeof(g_injector),  "%s", argv[1]);
    snprintf(g_hook_lib,  sizeof(g_hook_lib),  "%s", argv[2]);
    snprintf(g_log_path,  sizeof(g_log_path),  "%s", argv[3]);


    {
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s", g_log_path);
        char *slash = strrchr(tmp, '/');
        if (slash) { *slash = '\0'; snprintf(g_stop_path, sizeof(g_stop_path), "%s/watchdog.stop", tmp); }
        else        { snprintf(g_stop_path, sizeof(g_stop_path), "watchdog.stop"); }
    }


    if (access(g_injector, X_OK) != 0) {
        fprintf(stderr, "ERROR: injector not executable: %s\n", g_injector);
        return 1;
    }


    if (daemon(1 , 0 ) != 0) {
        fprintf(stderr, "daemon() failed: %s\n", strerror(errno));
        return 1;
    }

    wlog("=== watchdog daemon started ===");
    wlog("injector : %s", g_injector);
    wlog("hook lib : %s", g_hook_lib);

    int fail_count = 0;

    while (1) {
        sleep(POLL_INTERVAL_SEC);

        if (should_stop()) {
            wlog("stop file detected — exiting");
            break;
        }

        pid_t cs_pid = get_cameraserver_pid();
        if (cs_pid <= 0) continue;

        if (is_hooked(cs_pid)) {
            fail_count = 0;
            continue;
        }

        wlog("hook missing from cameraserver PID=%d — injecting", cs_pid);

        if (inject_with_selinux(cs_pid) == 0) {
            sleep(INJECT_VERIFY_WAIT_SEC);
            if (is_hooked(cs_pid)) {
                wlog("hook confirmed active in cameraserver PID=%d", cs_pid);
                fail_count = 0;
            } else {
                wlogw("WARNING: injector returned success but hook not in maps");
                fail_count++;
            }
        } else {
            fail_count++;
            wlog("injection failed (total failures: %d)", fail_count);
            if (fail_count >= MAX_INJECT_FAILURES) {
                wloge("FATAL: %d consecutive failures — watchdog giving up", fail_count);
                break;
            }
        }
    }

    wlog("=== watchdog daemon exiting ===");
    return 0;
}

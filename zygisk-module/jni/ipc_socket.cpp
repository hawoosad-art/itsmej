

#include "ipc_socket.h"
#include "frame_source.h"
#include "hook_proxy.h"

#include <atomic>
#include <android/log.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#define TAG "amkush/ipc_socket"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)

#define IPC_SHUTDOWN_SIGNAL ((uint8_t)0xFF)

static pthread_t         g_ipc_thread;
static int               g_listen_fd = -1;
static std::atomic<bool> g_ipc_running{false};

static int recv_fd(int sock) {
    char buf[1];
    struct iovec iov = { buf, 1 };
    char cmsg_buf[CMSG_SPACE(sizeof(int))];
    struct msghdr msg = {};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    ssize_t n = recvmsg(sock, &msg, 0);
    if (n <= 0) {
        LOGE("recvmsg failed: %s", strerror(errno));
        return -1;
    }

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
        LOGE("No SCM_RIGHTS in recvmsg");
        return -1;
    }

    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    return fd;
}

static void kick_old_server(const struct sockaddr_un *addr, socklen_t addr_len) {
    int kick = socket(AF_UNIX, SOCK_STREAM, 0);
    if (kick < 0) {
        LOGW("kick_old_server: socket() failed: %s", strerror(errno));
        return;
    }


    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(kick, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(kick, (const struct sockaddr *)addr, addr_len) == 0) {
        uint8_t sig = IPC_SHUTDOWN_SIGNAL;
        ssize_t sent = send(kick, &sig, 1, MSG_NOSIGNAL);
        if (sent == 1) {
            LOGI("kick_old_server: shutdown signal sent to old IPC server");
        } else {
            LOGW("kick_old_server: send() returned %zd (%s)", sent, strerror(errno));
        }
    } else {
        LOGW("kick_old_server: connect() failed (%s) — old server may already be gone",
             strerror(errno));
    }
    close(kick);
}

static void *ipc_thread(void *) {

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;



    memcpy(addr.sun_path, AMKUSH_SOCKET_NAME, sizeof(AMKUSH_SOCKET_NAME) - 1);
    socklen_t addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                                    + sizeof(AMKUSH_SOCKET_NAME) - 1);

    g_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        LOGE("socket() failed: %s", strerror(errno));
        return nullptr;
    }

    int reuse = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));










    bool bound = false;
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (bind(g_listen_fd, (struct sockaddr *)&addr, addr_len) == 0) {
            bound = true;
            break;
        }

        if (errno != EADDRINUSE) {
            LOGE("bind() failed (attempt %d): %s", attempt + 1, strerror(errno));
            break;
        }

        LOGW("bind() EADDRINUSE (attempt %d/6) — kicking old IPC server", attempt + 1);
        kick_old_server(&addr, addr_len);
        usleep(80000);
    }

    if (!bound) {
        LOGE("bind() failed after all attempts — IPC server cannot start");
        close(g_listen_fd);
        g_listen_fd = -1;
        return nullptr;
    }

    if (listen(g_listen_fd, 2) < 0) {
        LOGE("listen() failed: %s", strerror(errno));
        close(g_listen_fd);
        g_listen_fd = -1;
        return nullptr;
    }


    LOGI("Listening on abstract socket '%s'", &AMKUSH_SOCKET_NAME[1]);


    {
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(g_listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    while (g_ipc_running) {
        int client = accept(g_listen_fd, nullptr, nullptr);
        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            LOGW("accept() failed: %s", strerror(errno));
            continue;
        }






















        uint8_t probe = 0;
        ssize_t peeked = recv(client, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
        if (peeked == 1 && probe == IPC_SHUTDOWN_SIGNAL) {
            LOGI("Received shutdown signal from new injection instance "
                 "— uninstalling hooks to clear trampolines (BUG-15 fix)...");
            close(client);




            hook_proxy_uninstall();





            usleep(60000);

            LOGI("hook_proxy_uninstall done — releasing socket for new instance");
            break;
        }

        LOGI("Client connected — receiving fd");
        int fd = recv_fd(client);
        close(client);

        if (fd >= 0) {
            int r = frame_source_init(fd);
            if (r == 0) {
                LOGI("Frame source initialized from received fd=%d", fd);
            } else {
                LOGE("frame_source_init failed for fd=%d", fd);
                close(fd);
            }
        }
    }

    close(g_listen_fd);
    g_listen_fd = -1;
    return nullptr;
}

int ipc_socket_start(void) {
    if (g_ipc_running.load()) return 0;
    g_ipc_running.store(true);

    int r = pthread_create(&g_ipc_thread, nullptr, ipc_thread, nullptr);
    if (r != 0) {
        LOGE("pthread_create(ipc_thread) failed: %s", strerror(r));
        g_ipc_running.store(false);
        return -1;
    }
    return 0;
}

void ipc_socket_stop(void) {
    g_ipc_running.store(false);
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
    }
    pthread_join(g_ipc_thread, nullptr);
}

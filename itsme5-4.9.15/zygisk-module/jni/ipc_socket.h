#pragma once

#define AMKUSH_SOCKET_NAME  "\0amkush_frame_fd"

#ifdef __cplusplus
extern "C" {
#endif

int  ipc_socket_start(void);
void ipc_socket_stop(void);

#ifdef __cplusplus
}
#endif

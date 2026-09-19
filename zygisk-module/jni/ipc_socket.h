#pragma once

#define ITSANON_SOCKET_NAME  "\0itsanon_frame_fd"

#ifdef __cplusplus
extern "C" {
#endif

int  ipc_socket_start(void);
void ipc_socket_stop(void);

#ifdef __cplusplus
}
#endif

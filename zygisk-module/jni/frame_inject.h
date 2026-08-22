#pragma once
#include <stdbool.h>
#include "include/camera3_compat.h"
#include "stream_map.h"
#include "frame_source.h"

#ifdef __cplusplus
extern "C" {
#endif

int  frame_inject_init(void);
void frame_inject_destroy(void);
bool frame_inject_one(const camera3_stream_buffer_t *buf,
                      StreamRole                     role,
                      const FrameData               *src);

#ifdef __cplusplus
}
#endif

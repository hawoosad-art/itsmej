#pragma once
#include <stdbool.h>
#include "include/camera3_compat.h"

typedef enum {
    STREAM_ROLE_UNKNOWN    = 0,
    STREAM_ROLE_PREVIEW    = 1,
    STREAM_ROLE_VIDEO      = 2,
    STREAM_ROLE_SNAPSHOT   = 3,
    STREAM_ROLE_YUV_ANALYSIS = 4,
    STREAM_ROLE_ML_THUMB   = 5,
    STREAM_ROLE_RAW        = 6,
    STREAM_ROLE_HDR        = 7,
} StreamRole;

#ifdef __cplusplus
extern "C" {
#endif

void       stream_map_rebuild(camera3_stream_configuration_t *cfg);

void       stream_map_clear(void);

StreamRole stream_map_get_role(const camera3_stream_t *stream);

bool       stream_map_should_inject(StreamRole role, int buffer_status);

int        stream_modify_usage_for_injection(camera3_stream_configuration_t *cfg);

#ifdef __cplusplus
}
#endif

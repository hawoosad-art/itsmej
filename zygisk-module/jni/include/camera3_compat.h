

#pragma once

#include <stdint.h>

#ifndef ANDROID_DATASPACE_UNKNOWN
typedef int32_t android_dataspace_t;
#define ANDROID_DATASPACE_UNKNOWN   0
#define ANDROID_DATASPACE_JFIF      0x101
#define ANDROID_DATASPACE_V0_JFIF   0x101
#endif

#ifndef _NATIVE_HANDLE_H
#define _NATIVE_HANDLE_H

typedef struct native_handle {
    int version;
    int numFds;
    int numInts;
    int data[0];
} native_handle_t;

#endif

#ifndef ANDROID_HARDWARE_BUFFER_H
typedef native_handle_t* buffer_handle_t;
#endif

struct camera_metadata;
typedef struct camera_metadata camera_metadata_t;

#ifndef GRALLOC_USAGE_SW_READ_OFTEN
#define GRALLOC_USAGE_SW_READ_OFTEN       0x00000003U
#endif
#ifndef GRALLOC_USAGE_SW_WRITE_OFTEN
#define GRALLOC_USAGE_SW_WRITE_OFTEN      0x00000030U
#endif
#ifndef GRALLOC_USAGE_SW_WRITE_RARELY
#define GRALLOC_USAGE_SW_WRITE_RARELY     0x00000020U
#endif
#ifndef GRALLOC_USAGE_PROTECTED
#define GRALLOC_USAGE_PROTECTED           0x00004000U
#endif

#ifndef GRALLOC_USAGE_HW_TEXTURE
#define GRALLOC_USAGE_HW_TEXTURE          0x00000100U
#endif
#ifndef GRALLOC_USAGE_HW_COMPOSER
#define GRALLOC_USAGE_HW_COMPOSER         0x00000800U
#endif
#ifndef GRALLOC_USAGE_HW_VIDEO_ENCODER
#define GRALLOC_USAGE_HW_VIDEO_ENCODER    0x00010000U
#endif
#ifndef GRALLOC_USAGE_HW_CAMERA_WRITE
#define GRALLOC_USAGE_HW_CAMERA_WRITE     0x00020000U
#endif
#ifndef GRALLOC_USAGE_HW_CAMERA_READ
#define GRALLOC_USAGE_HW_CAMERA_READ      0x00040000U
#endif

#ifndef HAL_PIXEL_FORMAT_RGBA_8888
#define HAL_PIXEL_FORMAT_RGBA_8888        1
#endif
#ifndef HAL_PIXEL_FORMAT_BLOB
#define HAL_PIXEL_FORMAT_BLOB             0x21
#endif
#ifndef HAL_PIXEL_FORMAT_RAW16
#define HAL_PIXEL_FORMAT_RAW16            0x20
#endif
#ifndef HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED
#define HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED 0x22
#endif
#ifndef HAL_PIXEL_FORMAT_YCBCR_420_888
#define HAL_PIXEL_FORMAT_YCBCR_420_888    0x23
#endif
#ifndef HAL_PIXEL_FORMAT_YCrCb_420_SP
#define HAL_PIXEL_FORMAT_YCrCb_420_SP     0x11
#endif
#ifndef HAL_PIXEL_FORMAT_YCBCR_P010
#define HAL_PIXEL_FORMAT_YCBCR_P010       0x36
#endif

#ifndef CAMERA3_JPEG_BLOB_ID
#define CAMERA3_JPEG_BLOB_ID              0x00FF
#endif

typedef struct camera3_jpeg_blob {
    uint16_t jpeg_blob_id;
    uint16_t reserved;
    uint32_t jpeg_size;
} camera3_jpeg_blob_t;

typedef enum camera3_stream_type {
    CAMERA3_STREAM_OUTPUT        = 0,
    CAMERA3_STREAM_INPUT         = 1,
    CAMERA3_STREAM_BIDIRECTIONAL = 2,
} camera3_stream_type_t;

typedef struct camera3_stream {
    int                     stream_type;
    uint32_t                width;
    uint32_t                height;
    int                     format;
    uint32_t                usage;
    uint32_t                max_buffers;
    void                   *priv;
    android_dataspace_t     data_space;
    int32_t                 rotation;
#if __ANDROID_API__ >= 29
    const char             *physical_camera_id;
    uint32_t                reserved[51];
#else
    uint32_t                reserved[52];
#endif
} camera3_stream_t;

typedef struct camera3_stream_configuration {
    uint32_t                    num_streams;
    camera3_stream_t          **streams;
    uint32_t                    operation_mode;
    const camera_metadata_t    *session_parameters;
} camera3_stream_configuration_t;

typedef enum camera3_buffer_status {
    CAMERA3_BUFFER_STATUS_OK    = 0,
    CAMERA3_BUFFER_STATUS_ERROR = 1,
} camera3_buffer_status_t;

typedef struct camera3_stream_buffer {
    camera3_stream_t   *stream;
    buffer_handle_t    *buffer;
    int                 status;
    int                 acquire_fence;
    int                 release_fence;
} camera3_stream_buffer_t;

typedef struct camera3_capture_result {
    uint32_t                        frame_number;
    const camera_metadata_t        *result;
    uint32_t                        num_output_buffers;
    const camera3_stream_buffer_t  *output_buffers;
    const camera3_stream_buffer_t  *input_buffer;
    uint32_t                        partial_result;
    uint32_t                        num_physcam_metadata;
    const char                    **physcam_ids;
    const camera_metadata_t       **physcam_metadata;
} camera3_capture_result_t;

typedef enum camera3_msg_type {
    CAMERA3_MSG_ERROR   = 1,
    CAMERA3_MSG_SHUTTER = 2,
} camera3_msg_type_t;

typedef struct camera3_error_msg {
    uint32_t          frame_number;
    camera3_stream_t *error_stream;
    int32_t           error_code;
} camera3_error_msg_t;

typedef struct camera3_shutter_msg {
    uint32_t frame_number;
    uint64_t timestamp;
} camera3_shutter_msg_t;

typedef struct camera3_notify_msg {
    int type;
    union {
        camera3_error_msg_t   error;
        camera3_shutter_msg_t shutter;
        uint8_t               generic[32];
    } message;
} camera3_notify_msg_t;

struct camera3_buffer_request;
struct camera3_stream_buffer_ret;
typedef int camera3_buffer_request_status_t;

typedef struct camera3_callback_ops {
    void (*process_capture_result)(const struct camera3_callback_ops *,
                                   const camera3_capture_result_t *);
    void (*notify)(const struct camera3_callback_ops *,
                   const struct camera3_notify_msg *);
    camera3_buffer_request_status_t (*request_stream_buffers)(
        const struct camera3_callback_ops *,
        uint32_t num_buffer_reqs,
        const struct camera3_buffer_request *buffer_reqs,
        uint32_t *num_returned_buf_reqs,
        struct camera3_stream_buffer_ret *returned_buf_reqs);
    void (*return_stream_buffers)(const struct camera3_callback_ops *,
                                  uint32_t num_buffers,
                                  const camera3_stream_buffer_t *const *buffers);
} camera3_callback_ops_t;

struct camera3_device;
struct camera3_capture_request;
struct camera3_stream_buffer_set;
struct vendor_tag_query_ops;

typedef struct camera3_device_ops {
    int (*initialize)(const struct camera3_device *,
                      const camera3_callback_ops_t *callback_ops);
    int (*configure_streams)(const struct camera3_device *,
                             camera3_stream_configuration_t *stream_list);
    int (*register_stream_buffers)(const struct camera3_device *,
                                   const struct camera3_stream_buffer_set *buffer_set);
    const camera_metadata_t* (*construct_default_request_settings)(
                                    const struct camera3_device *,
                                    int type);
    int (*process_capture_request)(const struct camera3_device *,
                                   struct camera3_capture_request *request);
    void (*get_metadata_vendor_tag_ops)(const struct camera3_device*,
                                        struct vendor_tag_query_ops* ops);
    void (*dump)(const struct camera3_device *, int fd);
    int (*flush)(const struct camera3_device *);


    void *reserved[8];
} camera3_device_ops_t;

#ifndef HARDWARE_DEVICE_TAG
#define HARDWARE_DEVICE_TAG 0xD0D0D0D0U
struct hw_module_t;
typedef struct hw_device_t {
    uint32_t            tag;
    uint32_t            version;
    struct hw_module_t *module;
    uint32_t            reserved[12];
    int (*close)(struct hw_device_t *);
} hw_device_t;
#endif

typedef struct camera3_device {
    struct hw_device_t common;
    camera3_device_ops_t *ops;
    void *priv;
} camera3_device_t;

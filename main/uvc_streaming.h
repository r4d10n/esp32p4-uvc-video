/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "camera_pipeline.h"
#include "encoder_manager.h"
#include "usb_device_uvc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STREAM_FORMAT_YUY2,
    STREAM_FORMAT_MJPEG,
    STREAM_FORMAT_H264,
} stream_format_t;

typedef struct {
    /* Camera */
    camera_ctx_t camera;

    /* Encoders (opened once, started/stopped per stream) */
    encoder_ctx_t jpeg_enc;
    encoder_ctx_t h264_enc;

    /* Active stream state */
    stream_format_t active_format;
    encoder_ctx_t *active_encoder;   /* NULL for YUY2 (no encoding) */
    bool streaming;

    /* UVC frame buffer */
    uvc_fb_t fb;
    uint32_t pending_cam_buf_idx;  /* Camera buffer held during YUY2 streaming */
} uvc_stream_ctx_t;

/**
 * @brief Initialize the full UVC streaming pipeline
 *
 * Opens camera, encoders, and registers UVC device callbacks.
 */
esp_err_t uvc_stream_init(uvc_stream_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

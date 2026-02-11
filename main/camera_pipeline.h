/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAM_BUFFER_COUNT    2
#define ISP_NUM_PROFILES    6

typedef struct {
    int cap_fd;                             /* V4L2 capture device fd (/dev/video0) */
    uint8_t *cap_buffer[CAM_BUFFER_COUNT];  /* MMAP'd capture buffers */
    uint32_t cap_buf_size[CAM_BUFFER_COUNT];
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;                  /* Current ISP output format */
} camera_ctx_t;

/**
 * @brief Initialize the video subsystem (MIPI CSI + ISP + sensor)
 */
esp_err_t camera_init(void);

/**
 * @brief Open the camera capture device and query capabilities
 */
esp_err_t camera_open(camera_ctx_t *ctx);

/**
 * @brief Configure camera format, allocate buffers, and start streaming
 *
 * @param ctx       Camera context
 * @param width     Desired width
 * @param height    Desired height
 * @param pixfmt    V4L2 pixel format for ISP output (e.g. V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_YUV420)
 */
esp_err_t camera_start(camera_ctx_t *ctx, uint32_t width, uint32_t height, uint32_t pixfmt);

/**
 * @brief Stop streaming and release buffers
 */
esp_err_t camera_stop(camera_ctx_t *ctx);

/**
 * @brief Dequeue a captured frame. Returns buffer index.
 */
esp_err_t camera_dequeue(camera_ctx_t *ctx, uint32_t *buf_index, uint32_t *bytesused);

/**
 * @brief Re-queue a buffer after processing
 */
esp_err_t camera_enqueue(camera_ctx_t *ctx, uint32_t buf_index);

/**
 * @brief Apply an ISP color profile (CCM + WB + gamma + sharpen)
 *
 * @param profile_idx  Profile index 0..ISP_NUM_PROFILES-1
 */
void camera_apply_isp_profile(int profile_idx);

#ifdef __cplusplus
}
#endif

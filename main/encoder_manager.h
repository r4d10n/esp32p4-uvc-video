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

typedef enum {
    ENCODER_TYPE_JPEG,
    ENCODER_TYPE_H264,
} encoder_type_t;

typedef struct {
    int m2m_fd;                 /* V4L2 M2M device fd */
    encoder_type_t type;
    uint8_t *capture_buffer;    /* MMAP'd encoded output buffer */
    uint32_t capture_buf_size;
    uint32_t width;
    uint32_t height;
    uint32_t input_pixfmt;      /* Pixel format fed into encoder */

    /* H.264 params (0 = use defaults in encoder_start).
     * Set these before calling encoder_start() to override. */
    int h264_i_period;          /* IDR interval (default: 1 = all IDR) */
    int h264_bitrate;           /* Target bitrate in bps (default: auto) */
    int h264_min_qp;            /* Min QP (default: 20) */
    int h264_max_qp;            /* Max QP (default: 40) */
} encoder_ctx_t;

/**
 * @brief Open and configure a hardware encoder
 *
 * @param ctx       Encoder context to initialize
 * @param type      ENCODER_TYPE_JPEG or ENCODER_TYPE_H264
 */
esp_err_t encoder_open(encoder_ctx_t *ctx, encoder_type_t type);

/**
 * @brief Configure encoder input/output formats and start streaming
 *
 * @param ctx       Encoder context
 * @param width     Frame width
 * @param height    Frame height
 * @param input_fmt V4L2 pixel format of raw input
 */
esp_err_t encoder_start(encoder_ctx_t *ctx, uint32_t width, uint32_t height, uint32_t input_fmt);

/**
 * @brief Stop encoder streaming and release buffers
 */
esp_err_t encoder_stop(encoder_ctx_t *ctx);

/**
 * @brief Encode a single frame
 *
 * @param ctx           Encoder context
 * @param raw_buf       Raw frame data (from camera)
 * @param raw_len       Raw frame size in bytes
 * @param[out] enc_buf  Pointer to encoded output (valid until next call)
 * @param[out] enc_len  Size of encoded output
 */
esp_err_t encoder_encode(encoder_ctx_t *ctx, uint8_t *raw_buf, uint32_t raw_len,
                         uint8_t **enc_buf, uint32_t *enc_len);

#ifdef __cplusplus
}
#endif

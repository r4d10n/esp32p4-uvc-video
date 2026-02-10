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

/**
 * @brief Set H.264 encoder parameters via V4L2 controls
 *
 * @param m2m_fd    H.264 M2M device fd
 * @param bitrate   Target bitrate in bps
 * @param i_period  I-frame period
 * @param min_qp    Minimum QP
 * @param max_qp    Maximum QP
 */
esp_err_t uvc_ctrl_set_h264_params(int m2m_fd, int bitrate, int i_period,
                                    int min_qp, int max_qp);

/**
 * @brief Set JPEG encoder quality
 *
 * @param m2m_fd    JPEG M2M device fd
 * @param quality   Quality 1-100
 */
esp_err_t uvc_ctrl_set_jpeg_quality(int m2m_fd, int quality);

#ifdef __cplusplus
}
#endif

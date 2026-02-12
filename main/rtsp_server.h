/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the RTSP server
 *
 * Spawns a FreeRTOS task that listens on TCP port 554 for RTSP clients.
 * Handles OPTIONS, DESCRIBE, SETUP, PLAY, and TEARDOWN.
 * When PLAY is active, streams H.264 over RTP/UDP to the client.
 *
 * Supports one active client at a time.
 *
 * @return ESP_OK on success
 */
esp_err_t rtsp_server_start(void);

/**
 * @brief Feed an H.264 frame to the RTSP server for RTP streaming
 *
 * Called from the UVC streaming pipeline after H.264 encoding.
 * Copies the frame and signals the RTP sender. Non-blocking.
 *
 * @param data  H.264 Annex-B frame data
 * @param len   Frame length in bytes
 */
void rtsp_server_feed_h264(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

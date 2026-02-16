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
 * When no UVC stream is active, the RTSP server self-captures:
 * it drives the camera and H.264 encoder directly. When UVC starts
 * streaming, RTSP yields the hardware and relies on feed_h264().
 *
 * Supports one active client at a time.
 *
 * @param uvc_ctx  Pointer to uvc_stream_ctx_t (camera + encoder contexts)
 * @return ESP_OK on success
 */
esp_err_t rtsp_server_start(void *uvc_ctx);

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

/**
 * @brief Notify RTSP that UVC is about to start using the camera/encoder
 *
 * Call BEFORE camera_start(). Blocks until RTSP self-capture has stopped.
 */
void rtsp_server_notify_uvc_start(void);

/**
 * @brief Notify RTSP that UVC has stopped using the camera/encoder
 *
 * Call AFTER camera_stop(). RTSP will resume self-capture if PLAY is active.
 */
void rtsp_server_notify_uvc_stop(void);

#ifdef __cplusplus
}
#endif

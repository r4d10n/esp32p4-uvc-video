/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP32-P4 UVC 1.5 Webcam - OV5647 over MIPI CSI
 *
 * Supports three simultaneous output formats:
 *   - YUY2 (uncompressed YUV422)
 *   - MJPEG (hardware JPEG encoder)
 *   - H.264 (hardware H.264 encoder, UVC 1.5 frame-based)
 *
 * Target board: Olimex ESP32-P4-DevKit
 */

#include "esp_log.h"
#include "sdkconfig.h"
#include "camera_pipeline.h"
#include "uvc_controls.h"
#include "uvc_streaming.h"

static const char *TAG = "app_main";

void app_main(void)
{
    ESP_LOGI(TAG, "=== ESP32-P4 UVC 1.5 Webcam ===");
    ESP_LOGI(TAG, "Sensor: OV5647 (MIPI CSI 2-lane)");
    ESP_LOGI(TAG, "Board:  Olimex ESP32-P4-DevKit");

    /* Phase 1: Initialize camera + ISP + sensor via esp_video.
     * Note: esp_video_init() is not idempotent (registers ISP device),
     * so this must not be called in a retry loop. */
    esp_err_t ret = camera_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Check hardware: OV5647 ribbon cable, I2C wiring, camera power.");
        return;
    }

    /* Phase 2: Initialize the full UVC streaming pipeline */
    static uvc_stream_ctx_t stream_ctx;
    ret = uvc_stream_init(&stream_ctx);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UVC stream init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Phase 3: Apply default encoder parameters from Kconfig */
    uvc_ctrl_set_jpeg_quality(stream_ctx.jpeg_enc.m2m_fd,
                              CONFIG_UVC_JPEG_QUALITY);
    uvc_ctrl_set_h264_params(stream_ctx.h264_enc.m2m_fd,
                             CONFIG_UVC_H264_BITRATE,
                             CONFIG_UVC_H264_I_PERIOD,
                             CONFIG_UVC_H264_MIN_QP,
                             CONFIG_UVC_H264_MAX_QP);

    ESP_LOGI(TAG, "UVC device ready - connect USB to host");
}

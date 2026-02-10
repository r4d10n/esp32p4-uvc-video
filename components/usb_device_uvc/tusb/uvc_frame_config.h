/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Multi-format frame configuration for UVC webcam.
 * Defines per-format frame tables used by both descriptors and runtime.
 *
 * Resolution must match the OV5647 sensor's configured mode.
 * The CSI V4L2 driver does not support runtime resolution changes;
 * S_FMT only allows changing pixel format, not width/height.
 *
 * Current sensor config: 800x800 RAW8 50fps
 * (CONFIG_CAMERA_OV5647_MIPI_RAW8_800X800_50FPS)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  max_fps;
} uvc_frame_info_t;

/* Format 1: YUY2 (Uncompressed) - bFormatIndex=1
 * Limited to lower fps due to raw frame size vs USB HS bulk bandwidth.
 * 800x800 YUY2 = 1,280,000 bytes/frame. At 15fps = ~19.2MB/s. */
#define YUY2_FRAME_COUNT   1
static const uvc_frame_info_t uvc_yuy2_frames[YUY2_FRAME_COUNT] = {
    { 800,  800,  15 },
};

/* Format 2: MJPEG - bFormatIndex=2 */
#define MJPEG_FRAME_COUNT  1
static const uvc_frame_info_t uvc_mjpeg_frames[MJPEG_FRAME_COUNT] = {
    { 800,  800,  50 },
};

/* Format 3: H.264 (Frame-Based) - bFormatIndex=3 */
#define H264_FRAME_COUNT   1
static const uvc_frame_info_t uvc_h264_frames[H264_FRAME_COUNT] = {
    { 800,  800,  50 },
};

#define UVC_NUM_FORMATS  3

/*
 * Look up frame info by (bFormatIndex, bFrameIndex).
 * Both indices are 1-based per UVC spec.
 * Returns NULL if out of range.
 */
static inline const uvc_frame_info_t *uvc_get_frame_info(uint8_t format_index, uint8_t frame_index)
{
    if (frame_index == 0) return NULL;
    switch (format_index) {
    case 1:
        if (frame_index > YUY2_FRAME_COUNT) return NULL;
        return &uvc_yuy2_frames[frame_index - 1];
    case 2:
        if (frame_index > MJPEG_FRAME_COUNT) return NULL;
        return &uvc_mjpeg_frames[frame_index - 1];
    case 3:
        if (frame_index > H264_FRAME_COUNT) return NULL;
        return &uvc_h264_frames[frame_index - 1];
    default:
        return NULL;
    }
}

/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Multi-format frame configuration for UVC webcam.
 * Defines per-format frame tables used by both descriptors and runtime.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  max_fps;
} uvc_frame_info_t;

/* Format 1: YUY2 (Uncompressed) - bFormatIndex=1 */
#define YUY2_FRAME_COUNT   2
static const uvc_frame_info_t uvc_yuy2_frames[YUY2_FRAME_COUNT] = {
    { 640,  480,  30 },
    { 800,  640,  15 },
};

/* Format 2: MJPEG - bFormatIndex=2 */
#define MJPEG_FRAME_COUNT  5
static const uvc_frame_info_t uvc_mjpeg_frames[MJPEG_FRAME_COUNT] = {
    { 640,  480,  30 },
    { 800,  640,  50 },
    { 800,  800,  50 },
    { 1280, 960,  45 },
    { 1920, 1080, 30 },
};

/* Format 3: H.264 (Frame-Based) - bFormatIndex=3 */
#define H264_FRAME_COUNT   4
static const uvc_frame_info_t uvc_h264_frames[H264_FRAME_COUNT] = {
    { 640,  480,  30 },
    { 800,  640,  50 },
    { 1280, 960,  45 },
    { 1920, 1080, 30 },
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

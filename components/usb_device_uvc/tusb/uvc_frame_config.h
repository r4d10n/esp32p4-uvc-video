/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Multi-format, multi-resolution frame configuration for UVC webcam.
 * Defines per-format frame tables used by both descriptors and runtime.
 *
 * The OV5647 sensor captures at 800x800 RAW8 50fps (compile-time Kconfig).
 * The CSI V4L2 driver does NOT support runtime resolution changes.
 * Smaller resolutions are achieved via software center-crop from 800x800.
 *
 * Current sensor config: 800x800 RAW8 50fps
 * (CONFIG_CAMERA_OV5647_MIPI_RAW8_800X800_50FPS)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* Native camera capture resolution (fixed by sensor Kconfig) */
#define CAMERA_CAPTURE_WIDTH   800
#define CAMERA_CAPTURE_HEIGHT  800

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  max_fps;
} uvc_frame_info_t;

/* Format 1: UYVY (Uncompressed) - bFormatIndex=1
 * UYVY = 2 bytes/pixel. Bandwidth-limited over USB HS bulk:
 *   800x800 @ 15fps = 19.2 MB/s
 *   640x480 @ 30fps = 18.4 MB/s
 *   320x240 @ 50fps =  7.7 MB/s  */
#define UYVY_FRAME_COUNT   3
static const uvc_frame_info_t uvc_uyvy_frames[UYVY_FRAME_COUNT] = {
    { 800,  800,  15 },
    { 640,  480,  30 },
    { 320,  240,  50 },
};

/* Format 2: MJPEG - bFormatIndex=2
 * Compressed — bandwidth is not a concern at any resolution. */
#define MJPEG_FRAME_COUNT  3
static const uvc_frame_info_t uvc_mjpeg_frames[MJPEG_FRAME_COUNT] = {
    { 800,  800,  50 },
    { 640,  480,  50 },
    { 320,  240,  50 },
};

/* Format 3: H.264 (Frame-Based) - bFormatIndex=3
 * Compressed — bandwidth is not a concern at any resolution. */
#define H264_FRAME_COUNT   3
static const uvc_frame_info_t uvc_h264_frames[H264_FRAME_COUNT] = {
    { 800,  800,  50 },
    { 640,  480,  50 },
    { 320,  240,  50 },
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
        if (frame_index > UYVY_FRAME_COUNT) return NULL;
        return &uvc_uyvy_frames[frame_index - 1];
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

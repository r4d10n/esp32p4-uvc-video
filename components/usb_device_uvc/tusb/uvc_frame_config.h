/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Multi-format, multi-resolution frame configuration for UVC webcam.
 * Defines per-format frame tables used by both descriptors and runtime.
 *
 * The OV5647 sensor captures at 1920x1080 RAW10 30fps (compile-time Kconfig).
 * The CSI V4L2 driver does NOT support runtime resolution changes.
 * Smaller resolutions are achieved via software center-crop from 1920x1080.
 *
 * Current sensor config: 1920x1080 RAW10 30fps
 * (CONFIG_CAMERA_OV5647_MIPI_RAW10_1920X1080_30FPS)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* Native camera capture resolution (fixed by sensor Kconfig) */
#define CAMERA_CAPTURE_WIDTH   1920
#define CAMERA_CAPTURE_HEIGHT  1080

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  max_fps;
} uvc_frame_info_t;

/* Format 1: UYVY (Uncompressed) - bFormatIndex=1
 * UYVY = 2 bytes/pixel. Bandwidth-limited over USB HS bulk:
 *   1920x1080 @ ~9fps  = 39.6 MB/s (exceeds practical USB HS bulk)
 *   640x480   @ 30fps  = 18.4 MB/s
 *   320x240   @ 30fps  =  4.6 MB/s
 * Skip 1080p UYVY — 4.1MB/frame is impractical over USB HS. */
#define UYVY_FRAME_COUNT   2
static const uvc_frame_info_t uvc_uyvy_frames[UYVY_FRAME_COUNT] = {
    { 640,  480,  30 },
    { 320,  240,  30 },
};

/* Format 2: MJPEG - bFormatIndex=2
 * Compressed — bandwidth is not a concern at any resolution. */
#define MJPEG_FRAME_COUNT  3
static const uvc_frame_info_t uvc_mjpeg_frames[MJPEG_FRAME_COUNT] = {
    { 1920, 1080, 30 },
    { 1280,  720, 30 },
    {  640,  480, 30 },
};

/* Format 3: H.264 (Frame-Based) - bFormatIndex=3
 * Compressed — bandwidth is not a concern at any resolution. */
#define H264_FRAME_COUNT   3
static const uvc_frame_info_t uvc_h264_frames[H264_FRAME_COUNT] = {
    { 1920, 1080, 30 },
    { 1280,  720, 30 },
    {  640,  480, 30 },
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

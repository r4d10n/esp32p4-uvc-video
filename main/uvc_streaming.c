/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "linux/videodev2.h"
#include <sys/ioctl.h>
#include "usb_device_uvc.h"
#include "uvc_streaming.h"

/* Frame counts from uvc_descriptors.h (avoid pulling in tusb.h here) */
#define YUY2_FRAME_COUNT   2
#define MJPEG_FRAME_COUNT  5
#define H264_FRAME_COUNT   4

static const char *TAG = "uvc_stream";

/*
 * Map the UVC format enum (from host negotiation) to our internal format,
 * then choose the right ISP output pixel format for the camera.
 *
 * The key insight: each output format needs a DIFFERENT pixel format
 * from the ISP, because each encoder accepts different inputs:
 *
 *   YUY2  -> ISP outputs YUYV directly (no encoder needed)
 *   MJPEG -> ISP outputs UYVY or RGB (JPEG encoder input)
 *   H.264 -> ISP outputs YUV420 (H.264 encoder input)
 */
static uint32_t get_camera_pixfmt_for_format(stream_format_t fmt)
{
    switch (fmt) {
    case STREAM_FORMAT_YUY2:
        return V4L2_PIX_FMT_YUYV;
    case STREAM_FORMAT_MJPEG:
        return V4L2_PIX_FMT_UYVY;   /* JPEG HW encoder accepts UYVY */
    case STREAM_FORMAT_H264:
        return V4L2_PIX_FMT_YUV420; /* H.264 HW encoder needs YUV420 */
    default:
        return V4L2_PIX_FMT_YUYV;
    }
}

static stream_format_t uvc_format_to_stream_format(uvc_format_t uvc_fmt)
{
    switch (uvc_fmt) {
    case UVC_FORMAT_JPEG:
        return STREAM_FORMAT_MJPEG;
    case UVC_FORMAT_H264:
        return STREAM_FORMAT_H264;
    default:
        /* UVC_FORMAT_UNCOMPR or any uncompressed */
        return STREAM_FORMAT_YUY2;
    }
}

/*
 * Called when the USB host starts video streaming.
 * The host has already negotiated format/frame/fps via VS Probe/Commit.
 *
 * We must: configure camera -> configure encoder (if needed) -> start both.
 */
static esp_err_t on_stream_start(uvc_format_t uvc_format, int width, int height, int rate, void *cb_ctx)
{
    uvc_stream_ctx_t *ctx = (uvc_stream_ctx_t *)cb_ctx;

    ctx->active_format = uvc_format_to_stream_format(uvc_format);
    uint32_t cam_pixfmt = get_camera_pixfmt_for_format(ctx->active_format);

    ESP_LOGI(TAG, "Stream start: %dx%d @%dfps format=%d",
             width, height, rate, ctx->active_format);

    /* Start camera capture with the appropriate ISP output format */
    ESP_RETURN_ON_ERROR(camera_start(&ctx->camera, width, height, cam_pixfmt),
                        TAG, "Camera start failed");

    /* Start the appropriate encoder (skip for YUY2 - no encoding) */
    ctx->active_encoder = NULL;
    switch (ctx->active_format) {
    case STREAM_FORMAT_MJPEG:
        ESP_RETURN_ON_ERROR(encoder_start(&ctx->jpeg_enc, width, height, cam_pixfmt),
                            TAG, "JPEG encoder start failed");
        ctx->active_encoder = &ctx->jpeg_enc;
        break;
    case STREAM_FORMAT_H264:
        ESP_RETURN_ON_ERROR(encoder_start(&ctx->h264_enc, width, height, cam_pixfmt),
                            TAG, "H.264 encoder start failed");
        ctx->active_encoder = &ctx->h264_enc;
        break;
    case STREAM_FORMAT_YUY2:
        /* No encoder - ISP output goes directly to USB */
        break;
    }

    ctx->streaming = true;
    return ESP_OK;
}

static void on_stream_stop(void *cb_ctx)
{
    uvc_stream_ctx_t *ctx = (uvc_stream_ctx_t *)cb_ctx;

    ESP_LOGI(TAG, "Stream stop");
    ctx->streaming = false;

    if (ctx->active_encoder) {
        encoder_stop(ctx->active_encoder);
        ctx->active_encoder = NULL;
    }
    camera_stop(&ctx->camera);
}

/*
 * Called by TinyUSB when the host wants the next frame.
 * This is the hot path - called at frame rate (e.g. 30 times/sec).
 *
 * Pipeline:
 *   1. Dequeue raw frame from camera
 *   2. If encoded format: feed through HW encoder, get compressed output
 *      If YUY2: use raw frame directly
 *   3. Fill uvc_fb_t and return it
 */
static uvc_fb_t *on_fb_get(void *cb_ctx)
{
    uvc_stream_ctx_t *ctx = (uvc_stream_ctx_t *)cb_ctx;
    uint32_t buf_idx, bytesused;

    /* 1. Capture a frame from camera */
    if (camera_dequeue(&ctx->camera, &buf_idx, &bytesused) != ESP_OK) {
        ESP_LOGE(TAG, "Camera dequeue failed");
        return NULL;
    }

    uint8_t *frame_data = ctx->camera.cap_buffer[buf_idx];
    uint32_t frame_len = bytesused;

    /* 2. Encode if needed */
    if (ctx->active_encoder) {
        uint8_t *enc_buf;
        uint32_t enc_len;
        if (encoder_encode(ctx->active_encoder, frame_data, frame_len,
                           &enc_buf, &enc_len) != ESP_OK) {
            ESP_LOGE(TAG, "Encode failed");
            camera_enqueue(&ctx->camera, buf_idx);
            return NULL;
        }
        /* Re-queue camera buffer immediately since encoder has copied/consumed it */
        camera_enqueue(&ctx->camera, buf_idx);
        frame_data = enc_buf;
        frame_len = enc_len;
    } else {
        /*
         * YUY2 path: we must NOT re-queue until fb_return_cb.
         * Store the index so fb_return knows which buffer to release.
         */
        ctx->pending_cam_buf_idx = buf_idx;
    }

    /* 3. Fill the UVC frame buffer */
    ctx->fb.buf    = frame_data;
    ctx->fb.len    = frame_len;
    ctx->fb.width  = ctx->camera.width;
    ctx->fb.height = ctx->camera.height;

    switch (ctx->active_format) {
    case STREAM_FORMAT_MJPEG:
        ctx->fb.format = UVC_FORMAT_JPEG;
        break;
    case STREAM_FORMAT_H264:
        ctx->fb.format = UVC_FORMAT_H264;
        break;
    case STREAM_FORMAT_YUY2:
        ctx->fb.format = UVC_FORMAT_UNCOMPR;
        break;
    }

    int64_t us = esp_timer_get_time();
    ctx->fb.timestamp.tv_sec  = us / 1000000UL;
    ctx->fb.timestamp.tv_usec = us % 1000000UL;

    return &ctx->fb;
}

/*
 * Called after the USB stack has transmitted the frame.
 * For encoded formats, re-queue the encoder's capture buffer.
 * For YUY2, re-queue the camera buffer we held.
 */
static void on_fb_return(uvc_fb_t *fb, void *cb_ctx)
{
    uvc_stream_ctx_t *ctx = (uvc_stream_ctx_t *)cb_ctx;

    if (ctx->active_encoder) {
        /* Re-queue encoder capture buffer for next encode */
        struct v4l2_buffer buf = {
            .index  = 0,
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };
        ioctl(ctx->active_encoder->m2m_fd, VIDIOC_QBUF, &buf);
    } else {
        /* YUY2: release the camera buffer */
        camera_enqueue(&ctx->camera, ctx->pending_cam_buf_idx);
    }
}

esp_err_t uvc_stream_init(uvc_stream_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));

    /* Open camera */
    ESP_RETURN_ON_ERROR(camera_open(&ctx->camera), TAG, "Camera open failed");

    /* Open both encoders (they stay idle until a stream starts) */
    ESP_RETURN_ON_ERROR(encoder_open(&ctx->jpeg_enc, ENCODER_TYPE_JPEG),
                        TAG, "JPEG encoder open failed");
    ESP_RETURN_ON_ERROR(encoder_open(&ctx->h264_enc, ENCODER_TYPE_H264),
                        TAG, "H.264 encoder open failed");

    /* Configure UVC device with our callbacks */
    uvc_device_config_t uvc_config = {
        .start_cb     = on_stream_start,
        .fb_get_cb    = on_fb_get,
        .fb_return_cb = on_fb_return,
        .stop_cb      = on_stream_stop,
        .cb_ctx       = ctx,
    };

    /*
     * Allocate UVC transfer buffer.
     * Size needs to be large enough for the largest uncompressed frame
     * (YUY2 800x640 = 1,024,000 bytes) or the largest expected compressed frame.
     */
    uvc_config.uvc_buffer_size = 1920 * 1080 * 2;
    uvc_config.uvc_buffer = malloc(uvc_config.uvc_buffer_size);
    ESP_RETURN_ON_FALSE(uvc_config.uvc_buffer, ESP_ERR_NO_MEM, TAG,
                        "Failed to allocate UVC buffer");

    ESP_RETURN_ON_ERROR(uvc_device_config(0, &uvc_config), TAG, "UVC config failed");
    ESP_RETURN_ON_ERROR(uvc_device_init(), TAG, "UVC init failed");

    ESP_LOGI(TAG, "UVC streaming pipeline initialized");
    ESP_LOGI(TAG, "  Formats: YUY2 (%d frames), MJPEG (%d frames), H.264 (%d frames)",
             YUY2_FRAME_COUNT, MJPEG_FRAME_COUNT, H264_FRAME_COUNT);

    return ESP_OK;
}

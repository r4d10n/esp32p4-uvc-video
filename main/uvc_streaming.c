/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "linux/videodev2.h"
#include <sys/ioctl.h>
#include "usb_device_uvc.h"
#include "uvc_streaming.h"
#include "uvc_frame_config.h"
#include "rtsp_server.h"

static const char *TAG = "uvc_stream";

/* Largest uncompressed frame: UYVY 1920x1080 = 2 bytes/pixel = 4,147,200 bytes */
#define UVC_MAX_FRAME_BUFFER_SIZE  (CAMERA_CAPTURE_WIDTH * CAMERA_CAPTURE_HEIGHT * 2)

/* ---- Software center-crop functions ------------------------------------ */

/*
 * Center-crop a UYVY frame (2 bytes/pixel, packed YUV422).
 * Each 4-byte macro-pixel covers 2 horizontal pixels (U0 Y0 V0 Y1),
 * so x_off is forced even to avoid splitting a macro-pixel.
 */
static void center_crop_uyvy(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                              uint8_t *dst, uint32_t dst_w, uint32_t dst_h)
{
    uint32_t x_off = ((src_w - dst_w) / 2) & ~1u;
    uint32_t y_off = (src_h - dst_h) / 2;
    uint32_t src_stride = src_w * 2;
    uint32_t dst_stride = dst_w * 2;

    const uint8_t *src_row = src + (y_off * src_stride) + (x_off * 2);
    for (uint32_t y = 0; y < dst_h; y++) {
        memcpy(dst + y * dst_stride, src_row + y * src_stride, dst_stride);
    }
}

/*
 * Center-crop a YUV420 planar (I420) frame.
 * Y plane: full resolution, U and V planes: half in each dimension.
 * Offsets forced even to align with chroma subsampling.
 */
static void center_crop_yuv420(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                uint8_t *dst, uint32_t dst_w, uint32_t dst_h)
{
    uint32_t x_off = ((src_w - dst_w) / 2) & ~1u;
    uint32_t y_off = ((src_h - dst_h) / 2) & ~1u;

    /* Y plane */
    const uint8_t *src_y = src + y_off * src_w + x_off;
    uint8_t *dst_y = dst;
    for (uint32_t y = 0; y < dst_h; y++) {
        memcpy(dst_y + y * dst_w, src_y + y * src_w, dst_w);
    }

    /* U plane (quarter resolution) */
    uint32_t src_uv_stride = src_w / 2;
    uint32_t dst_uv_w = dst_w / 2;
    uint32_t dst_uv_h = dst_h / 2;
    uint32_t uv_x_off = x_off / 2;
    uint32_t uv_y_off = y_off / 2;

    const uint8_t *src_u = src + (src_w * src_h) + uv_y_off * src_uv_stride + uv_x_off;
    uint8_t *dst_u = dst + (dst_w * dst_h);
    for (uint32_t y = 0; y < dst_uv_h; y++) {
        memcpy(dst_u + y * dst_uv_w, src_u + y * src_uv_stride, dst_uv_w);
    }

    /* V plane (quarter resolution) */
    uint32_t src_uv_plane_size = (src_w / 2) * (src_h / 2);
    const uint8_t *src_v = src + (src_w * src_h) + src_uv_plane_size
                         + uv_y_off * src_uv_stride + uv_x_off;
    uint8_t *dst_v = dst + (dst_w * dst_h) + (dst_uv_w * dst_uv_h);
    for (uint32_t y = 0; y < dst_uv_h; y++) {
        memcpy(dst_v + y * dst_uv_w, src_v + y * src_uv_stride, dst_uv_w);
    }
}

/* ---- Format mapping ---------------------------------------------------- */

/*
 * Map the UVC format enum to our internal format, then choose the right
 * ISP output pixel format for the camera.
 *
 *   UYVY  -> ISP outputs UYVY directly (no encoder needed)
 *   MJPEG -> ISP outputs UYVY (JPEG HW encoder input)
 *   H.264 -> ISP outputs YUV420 (H.264 HW encoder input)
 */
static uint32_t get_camera_pixfmt_for_format(stream_format_t fmt)
{
    switch (fmt) {
    case STREAM_FORMAT_YUY2:
        return V4L2_PIX_FMT_UYVY;   /* ISP outputs UYVY, sent raw to host */
    case STREAM_FORMAT_MJPEG:
        return V4L2_PIX_FMT_UYVY;   /* JPEG HW encoder accepts UYVY */
    case STREAM_FORMAT_H264:
        return V4L2_PIX_FMT_YUV420; /* H.264 HW encoder needs YUV420 */
    default:
        return V4L2_PIX_FMT_UYVY;
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

/* ---- Stream lifecycle -------------------------------------------------- */

/* Forward declaration — on_stream_start may call on_stream_stop for seamless
 * format/resolution switches without an explicit host stop. */
static void on_stream_stop(void *cb_ctx);

/*
 * Called when the USB host starts video streaming.
 * The host has already negotiated format/frame/fps via VS Probe/Commit.
 *
 * Camera always captures at CAMERA_CAPTURE_WIDTH x CAMERA_CAPTURE_HEIGHT
 * (sensor is fixed). If the negotiated resolution is smaller, we allocate
 * a crop staging buffer and center-crop each frame before encoding/sending.
 */
static esp_err_t on_stream_start(uvc_format_t uvc_format, int width, int height, int rate, void *cb_ctx)
{
    uvc_stream_ctx_t *ctx = (uvc_stream_ctx_t *)cb_ctx;

    /*
     * The host may send a new VS_COMMIT (format/resolution change) without
     * an explicit stream stop.  Tear down the previous stream first.
     */
    if (ctx->streaming) {
        ESP_LOGW(TAG, "Stream still active — stopping previous stream before restart");
        on_stream_stop(cb_ctx);
    }

    ctx->active_format = uvc_format_to_stream_format(uvc_format);
    ctx->negotiated_width = width;
    ctx->negotiated_height = height;
    uint32_t cam_pixfmt = get_camera_pixfmt_for_format(ctx->active_format);

    ESP_LOGI(TAG, "Stream start: %dx%d @%dfps format=%d (capture %dx%d)",
             width, height, rate, ctx->active_format,
             CAMERA_CAPTURE_WIDTH, CAMERA_CAPTURE_HEIGHT);

    /* Tell RTSP to yield camera/encoder if self-capturing */
    rtsp_server_notify_uvc_start();

    /* Camera always captures at native sensor resolution */
    esp_err_t ret = camera_start(&ctx->camera, CAMERA_CAPTURE_WIDTH,
                                  CAMERA_CAPTURE_HEIGHT, cam_pixfmt);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera start failed");
        return ret;
    }

    /* Allocate crop buffer if negotiated resolution differs from capture */
    bool needs_crop = (width != CAMERA_CAPTURE_WIDTH || height != CAMERA_CAPTURE_HEIGHT);
    if (needs_crop) {
        if (cam_pixfmt == V4L2_PIX_FMT_YUV420) {
            ctx->crop_buf_size = width * height * 3 / 2;
        } else {
            ctx->crop_buf_size = width * height * 2;
        }
        ctx->crop_buf = heap_caps_aligned_alloc(64, ctx->crop_buf_size,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!ctx->crop_buf) {
            ESP_LOGE(TAG, "Failed to allocate crop buffer (%lu bytes)",
                     (unsigned long)ctx->crop_buf_size);
            camera_stop(&ctx->camera);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "Crop buffer: %lu bytes (center-crop from %dx%d to %dx%d)",
                 (unsigned long)ctx->crop_buf_size,
                 CAMERA_CAPTURE_WIDTH, CAMERA_CAPTURE_HEIGHT, width, height);
    }

    /* Start the appropriate encoder (skip for UYVY - no encoding) */
    ctx->active_encoder = NULL;
    switch (ctx->active_format) {
    case STREAM_FORMAT_MJPEG:
        ret = encoder_start(&ctx->jpeg_enc, width, height, cam_pixfmt);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "JPEG encoder start failed");
            goto err_encoder;
        }
        ctx->active_encoder = &ctx->jpeg_enc;
        break;
    case STREAM_FORMAT_H264:
        ret = encoder_start(&ctx->h264_enc, width, height, cam_pixfmt);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "H.264 encoder start failed");
            goto err_encoder;
        }
        ctx->active_encoder = &ctx->h264_enc;
        break;
    case STREAM_FORMAT_YUY2:
        /* No encoder - ISP output goes directly to USB */
        break;
    }

    ctx->streaming = true;
    return ESP_OK;

err_encoder:
    if (ctx->crop_buf) {
        free(ctx->crop_buf);
        ctx->crop_buf = NULL;
        ctx->crop_buf_size = 0;
    }
    camera_stop(&ctx->camera);
    return ret;
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

    if (ctx->crop_buf) {
        free(ctx->crop_buf);
        ctx->crop_buf = NULL;
        ctx->crop_buf_size = 0;
    }

    /* Tell RTSP it can resume self-capture */
    rtsp_server_notify_uvc_stop();
}

/*
 * Called by TinyUSB when the host wants the next frame.
 * This is the hot path - called at frame rate.
 *
 * Pipeline:
 *   1. Dequeue raw frame from camera (always CAMERA_CAPTURE_* resolution)
 *   2. If negotiated resolution < capture: center-crop into staging buffer
 *   3. If encoded format: feed through HW encoder, get compressed output
 *      If UYVY raw: use frame directly (or cropped buffer)
 *   4. Fill uvc_fb_t and return it
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

    uint8_t *raw_data = ctx->camera.cap_buffer[buf_idx];
    uint32_t raw_len = bytesused;

    /* 2. Center-crop if negotiated resolution < capture resolution */
    if (ctx->crop_buf) {
        if (ctx->active_format == STREAM_FORMAT_H264) {
            center_crop_yuv420(raw_data, CAMERA_CAPTURE_WIDTH, CAMERA_CAPTURE_HEIGHT,
                               ctx->crop_buf, ctx->negotiated_width, ctx->negotiated_height);
            raw_len = ctx->negotiated_width * ctx->negotiated_height * 3 / 2;
        } else {
            center_crop_uyvy(raw_data, CAMERA_CAPTURE_WIDTH, CAMERA_CAPTURE_HEIGHT,
                             ctx->crop_buf, ctx->negotiated_width, ctx->negotiated_height);
            raw_len = ctx->negotiated_width * ctx->negotiated_height * 2;
        }
        raw_data = ctx->crop_buf;
        /* Flush CPU cache to PSRAM so encoder/USB DMA sees the cropped data */
        esp_cache_msync(ctx->crop_buf, (raw_len + 63) & ~63,
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        /* Camera buffer can be re-queued immediately since we copied data */
        camera_enqueue(&ctx->camera, buf_idx);
        buf_idx = UINT32_MAX;  /* Sentinel: already re-queued */
    }

    /* 3. Encode if needed */
    uint8_t *frame_data = raw_data;
    uint32_t frame_len = raw_len;

    if (ctx->active_encoder) {
        uint8_t *enc_buf;
        uint32_t enc_len;
        if (encoder_encode(ctx->active_encoder, raw_data, raw_len,
                           &enc_buf, &enc_len) != ESP_OK) {
            ESP_LOGE(TAG, "Encode failed");
            if (buf_idx != UINT32_MAX) {
                camera_enqueue(&ctx->camera, buf_idx);
            }
            return NULL;
        }
        /* Re-queue camera buffer since encoder has consumed it */
        if (buf_idx != UINT32_MAX) {
            camera_enqueue(&ctx->camera, buf_idx);
        }
        frame_data = enc_buf;
        frame_len = enc_len;
    } else if (buf_idx != UINT32_MAX) {
        /*
         * UYVY raw, no crop: hold camera buffer until fb_return.
         * Store the index so fb_return knows which buffer to release.
         */
        ctx->pending_cam_buf_idx = buf_idx;
    }
    /* else: UYVY raw with crop — data is in crop_buf, camera already re-queued */

    /* 3b. Feed H.264 frame to RTSP/RTP server (non-blocking copy) */
    if (ctx->active_format == STREAM_FORMAT_H264 && frame_len > 0) {
        rtsp_server_feed_h264(frame_data, frame_len);
    }

    /* 4. Fill the UVC frame buffer */
    ctx->fb.buf    = frame_data;
    ctx->fb.len    = frame_len;
    ctx->fb.width  = ctx->negotiated_width;
    ctx->fb.height = ctx->negotiated_height;

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

    /* Update performance counters */
    ctx->perf_frame_count++;
    ctx->perf_byte_count += frame_len;

    return &ctx->fb;
}

/*
 * Called after the USB stack has transmitted the frame.
 * For encoded formats, re-queue the encoder's capture buffer.
 * For UYVY raw without crop, re-queue the camera buffer we held.
 * For UYVY raw with crop, nothing to do (camera buffer already re-queued).
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
    } else if (!ctx->crop_buf) {
        /* UYVY raw without crop: release the held camera buffer */
        camera_enqueue(&ctx->camera, ctx->pending_cam_buf_idx);
    }
    /* UYVY raw with crop: camera buffer was already re-queued in on_fb_get */
}

/* ---- Initialization ---------------------------------------------------- */

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
     * UVC transfer buffer — must hold the largest possible frame.
     * UYVY 800x800 = 1,280,000 bytes. Compressed frames are always smaller.
     */
    uvc_config.uvc_buffer_size = UVC_MAX_FRAME_BUFFER_SIZE;
    /* 64-byte alignment for L1 cache-line coherency with DWC2 DMA */
    uvc_config.uvc_buffer = heap_caps_aligned_alloc(64, uvc_config.uvc_buffer_size,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(uvc_config.uvc_buffer, ESP_ERR_NO_MEM, TAG,
                        "Failed to allocate UVC buffer");

    ESP_RETURN_ON_ERROR(uvc_device_config(0, &uvc_config), TAG, "UVC config failed");
    ESP_RETURN_ON_ERROR(uvc_device_init(), TAG, "UVC init failed");

    ESP_LOGI(TAG, "UVC streaming pipeline initialized");
    ESP_LOGI(TAG, "  Formats: UYVY (%d frames), MJPEG (%d frames), H.264 (%d frames)",
             UYVY_FRAME_COUNT, MJPEG_FRAME_COUNT, H264_FRAME_COUNT);
    ESP_LOGI(TAG, "  UVC buffer: %d bytes", UVC_MAX_FRAME_BUFFER_SIZE);

    return ESP_OK;
}

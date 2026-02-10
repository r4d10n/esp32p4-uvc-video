/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_cache.h"
#include "linux/videodev2.h"
#include "linux/v4l2-controls.h"
#include "esp_video_device.h"
#include "encoder_manager.h"

static const char *TAG = "encoder";

esp_err_t encoder_open(encoder_ctx_t *ctx, encoder_type_t type)
{
    const char *devpath;
    struct v4l2_capability cap;

    memset(ctx, 0, sizeof(*ctx));
    ctx->type = type;

    if (type == ENCODER_TYPE_JPEG) {
        devpath = ESP_VIDEO_JPEG_DEVICE_NAME;
    } else {
        devpath = ESP_VIDEO_H264_DEVICE_NAME;
    }

    ctx->m2m_fd = open(devpath, O_RDONLY);
    ESP_RETURN_ON_FALSE(ctx->m2m_fd >= 0, ESP_FAIL, TAG,
                        "Failed to open %s", devpath);

    ESP_RETURN_ON_FALSE(ioctl(ctx->m2m_fd, VIDIOC_QUERYCAP, &cap) == 0,
                        ESP_FAIL, TAG, "QUERYCAP failed on %s", devpath);

    ESP_LOGI(TAG, "Encoder opened: %s (%s)", cap.card, cap.driver);
    return ESP_OK;
}

/*
 * The M2M (mem2mem) device has two sides:
 *   OUTPUT  = raw frames we feed IN to the encoder
 *   CAPTURE = encoded frames we read OUT from the encoder
 *
 * We use USERPTR for the output side (zero-copy from camera buffers)
 * and MMAP for the capture side (encoder writes compressed data).
 */
esp_err_t encoder_start(encoder_ctx_t *ctx, uint32_t width, uint32_t height, uint32_t input_fmt)
{
    uint32_t output_fmt = (ctx->type == ENCODER_TYPE_JPEG) ?
                           V4L2_PIX_FMT_JPEG : V4L2_PIX_FMT_H264;

    ctx->width = width;
    ctx->height = height;
    ctx->input_pixfmt = input_fmt;

    /* Configure M2M output (raw input to encoder) */
    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .fmt.pix = {
            .width       = width,
            .height      = height,
            .pixelformat = input_fmt,
        },
    };
    ESP_RETURN_ON_FALSE(ioctl(ctx->m2m_fd, VIDIOC_S_FMT, &fmt) == 0,
                        ESP_FAIL, TAG, "S_FMT output failed");

    struct v4l2_requestbuffers req = {
        .count  = 1,
        .type   = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory = V4L2_MEMORY_USERPTR,
    };
    ESP_RETURN_ON_FALSE(ioctl(ctx->m2m_fd, VIDIOC_REQBUFS, &req) == 0,
                        ESP_FAIL, TAG, "REQBUFS output failed");

    /* Configure H.264 encoder parameters before starting */
    if (ctx->type == ENCODER_TYPE_H264) {
        struct v4l2_ext_control ctrls_arr[] = {
            { .id = V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, .value = 1 },    /* every frame is IDR */
            { .id = V4L2_CID_MPEG_VIDEO_BITRATE,       .value = 2000000 },
            { .id = V4L2_CID_MPEG_VIDEO_H264_MIN_QP,   .value = 20 },
            { .id = V4L2_CID_MPEG_VIDEO_H264_MAX_QP,   .value = 40 },
        };
        struct v4l2_ext_controls ctrls = {
            .ctrl_class = V4L2_CID_CODEC_CLASS,
            .count      = sizeof(ctrls_arr) / sizeof(ctrls_arr[0]),
            .controls   = ctrls_arr,
        };
        if (ioctl(ctx->m2m_fd, VIDIOC_S_EXT_CTRLS, &ctrls) != 0) {
            ESP_LOGW(TAG, "H.264 ext ctrls set failed (non-fatal)");
        } else {
            ESP_LOGI(TAG, "H.264: GOP=1 (all IDR), bitrate=2Mbps, QP=20-40");
        }
    }

    /* Configure M2M capture (encoded output from encoder) */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = output_fmt;
    ESP_RETURN_ON_FALSE(ioctl(ctx->m2m_fd, VIDIOC_S_FMT, &fmt) == 0,
                        ESP_FAIL, TAG, "S_FMT capture failed");

    memset(&req, 0, sizeof(req));
    req.count  = 1;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_RETURN_ON_FALSE(ioctl(ctx->m2m_fd, VIDIOC_REQBUFS, &req) == 0,
                        ESP_FAIL, TAG, "REQBUFS capture failed");

    /* MMAP the capture buffer */
    struct v4l2_buffer buf = {
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
        .index  = 0,
    };
    ESP_RETURN_ON_FALSE(ioctl(ctx->m2m_fd, VIDIOC_QUERYBUF, &buf) == 0,
                        ESP_FAIL, TAG, "QUERYBUF capture failed");

    ctx->capture_buffer = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                               MAP_SHARED, ctx->m2m_fd, buf.m.offset);
    ESP_RETURN_ON_FALSE(ctx->capture_buffer != MAP_FAILED,
                        ESP_FAIL, TAG, "mmap capture failed");
    ctx->capture_buf_size = buf.length;

    /* Queue capture buffer and start both streams */
    ESP_RETURN_ON_FALSE(ioctl(ctx->m2m_fd, VIDIOC_QBUF, &buf) == 0,
                        ESP_FAIL, TAG, "QBUF capture failed");

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(ctx->m2m_fd, VIDIOC_STREAMON, &type) == 0,
                        ESP_FAIL, TAG, "STREAMON capture failed");
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ESP_RETURN_ON_FALSE(ioctl(ctx->m2m_fd, VIDIOC_STREAMON, &type) == 0,
                        ESP_FAIL, TAG, "STREAMON output failed");

    ESP_LOGI(TAG, "%s encoder started: %lux%lu",
             ctx->type == ENCODER_TYPE_JPEG ? "JPEG" : "H.264",
             (unsigned long)width, (unsigned long)height);
    return ESP_OK;
}

esp_err_t encoder_stop(encoder_ctx_t *ctx)
{
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(ctx->m2m_fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(ctx->m2m_fd, VIDIOC_STREAMOFF, &type);

    if (ctx->capture_buffer && ctx->capture_buffer != MAP_FAILED) {
        munmap(ctx->capture_buffer, ctx->capture_buf_size);
        ctx->capture_buffer = NULL;
    }

    ESP_LOGI(TAG, "Encoder stopped");
    return ESP_OK;
}

esp_err_t encoder_encode(encoder_ctx_t *ctx, uint8_t *raw_buf, uint32_t raw_len,
                         uint8_t **enc_buf, uint32_t *enc_len)
{
    /* Feed raw frame into encoder (USERPTR - zero-copy) */
    struct v4l2_buffer out_buf = {
        .index     = 0,
        .type      = V4L2_BUF_TYPE_VIDEO_OUTPUT,
        .memory    = V4L2_MEMORY_USERPTR,
        .m.userptr = (unsigned long)raw_buf,
        .length    = raw_len,
    };
    ESP_RETURN_ON_FALSE(ioctl(ctx->m2m_fd, VIDIOC_QBUF, &out_buf) == 0,
                        ESP_FAIL, TAG, "QBUF output failed");

    /* Wait for encoded output */
    struct v4l2_buffer cap_buf = {
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ESP_RETURN_ON_FALSE(ioctl(ctx->m2m_fd, VIDIOC_DQBUF, &cap_buf) == 0,
                        ESP_FAIL, TAG, "DQBUF capture failed");

    /* Reclaim the output buffer */
    ESP_RETURN_ON_FALSE(ioctl(ctx->m2m_fd, VIDIOC_DQBUF, &out_buf) == 0,
                        ESP_FAIL, TAG, "DQBUF output failed");

    /*
     * Invalidate CPU data cache for the capture buffer region.
     * The H.264/JPEG encoder writes via DMA, bypassing the CPU cache.
     * Without this, CPU reads stale cache lines → corrupted NAL units.
     */
    /* Round up to cache line size (64 bytes) for alignment requirement */
    uint32_t aligned_len = (cap_buf.bytesused + 63) & ~63;
    esp_cache_msync(ctx->capture_buffer, aligned_len,
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    *enc_buf = ctx->capture_buffer;
    *enc_len = cap_buf.bytesused;

    return ESP_OK;
}

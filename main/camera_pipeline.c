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
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "board_olimex_p4.h"
#include "camera_pipeline.h"

static const char *TAG = "cam_pipe";

esp_err_t camera_init(void)
{
    static esp_video_init_csi_config_t csi_config = {
        .sccb_config = {
            .init_sccb = true,
            .i2c_config = {
                .port    = BOARD_I2C_PORT,
                .scl_pin = BOARD_I2C_SCL_PIN,
                .sda_pin = BOARD_I2C_SDA_PIN,
            },
            .freq = BOARD_I2C_FREQ,
        },
        .reset_pin    = BOARD_CAM_RESET_PIN,
        .pwdn_pin     = BOARD_CAM_PWDN_PIN,
        .dont_init_ldo = BOARD_CSI_DONT_INIT_LDO,
    };

    static const esp_video_init_config_t video_config = {
        .csi = &csi_config,
    };

    ESP_LOGI(TAG, "Initializing video subsystem (MIPI CSI + ISP + OV5647)");
    return esp_video_init(&video_config);
}

esp_err_t camera_open(camera_ctx_t *ctx)
{
    struct v4l2_capability cap;

    memset(ctx, 0, sizeof(*ctx));
    ctx->cap_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    ESP_RETURN_ON_FALSE(ctx->cap_fd >= 0, ESP_FAIL, TAG, "Failed to open %s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);

    ESP_RETURN_ON_FALSE(ioctl(ctx->cap_fd, VIDIOC_QUERYCAP, &cap) == 0,
                        ESP_FAIL, TAG, "QUERYCAP failed");

    ESP_LOGI(TAG, "Camera: %s (%s)", cap.card, cap.driver);
    ESP_LOGI(TAG, "  Capabilities: 0x%08lx", (unsigned long)cap.capabilities);

    /* Enumerate available formats */
    struct v4l2_fmtdesc fmtdesc = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    };
    ESP_LOGI(TAG, "  Available formats:");
    while (ioctl(ctx->cap_fd, VIDIOC_ENUM_FMT, &fmtdesc) == 0) {
        ESP_LOGI(TAG, "    [%lu] %s (0x%08lx)", (unsigned long)fmtdesc.index,
                 fmtdesc.description, (unsigned long)fmtdesc.pixelformat);
        fmtdesc.index++;
    }

    return ESP_OK;
}

esp_err_t camera_start(camera_ctx_t *ctx, uint32_t width, uint32_t height, uint32_t pixfmt)
{
    struct v4l2_format fmt = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix = {
            .width       = width,
            .height      = height,
            .pixelformat = pixfmt,
        },
    };

    ESP_LOGI(TAG, "Setting format %lux%lu pixfmt=0x%08lx",
             (unsigned long)width, (unsigned long)height, (unsigned long)pixfmt);
    ESP_RETURN_ON_FALSE(ioctl(ctx->cap_fd, VIDIOC_S_FMT, &fmt) == 0,
                        ESP_FAIL, TAG, "S_FMT failed");

    ctx->width = fmt.fmt.pix.width;
    ctx->height = fmt.fmt.pix.height;
    ctx->pixel_format = fmt.fmt.pix.pixelformat;
    ESP_LOGI(TAG, "Negotiated: %lux%lu", (unsigned long)ctx->width, (unsigned long)ctx->height);

    /* Request buffers */
    struct v4l2_requestbuffers req = {
        .count  = CAM_BUFFER_COUNT,
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ESP_RETURN_ON_FALSE(ioctl(ctx->cap_fd, VIDIOC_REQBUFS, &req) == 0,
                        ESP_FAIL, TAG, "REQBUFS failed");

    /* Map and queue buffers */
    for (int i = 0; i < CAM_BUFFER_COUNT; i++) {
        struct v4l2_buffer buf = {
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index  = i,
        };
        ESP_RETURN_ON_FALSE(ioctl(ctx->cap_fd, VIDIOC_QUERYBUF, &buf) == 0,
                            ESP_FAIL, TAG, "QUERYBUF %d failed", i);

        ctx->cap_buffer[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, ctx->cap_fd, buf.m.offset);
        ESP_RETURN_ON_FALSE(ctx->cap_buffer[i] != MAP_FAILED,
                            ESP_FAIL, TAG, "mmap %d failed", i);
        ctx->cap_buf_size[i] = buf.length;

        ESP_RETURN_ON_FALSE(ioctl(ctx->cap_fd, VIDIOC_QBUF, &buf) == 0,
                            ESP_FAIL, TAG, "QBUF %d failed", i);
    }

    /* Start streaming */
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(ctx->cap_fd, VIDIOC_STREAMON, &type) == 0,
                        ESP_FAIL, TAG, "STREAMON failed");

    ESP_LOGI(TAG, "Camera streaming started (%lu buffers)", (unsigned long)CAM_BUFFER_COUNT);
    return ESP_OK;
}

esp_err_t camera_stop(camera_ctx_t *ctx)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(ctx->cap_fd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < CAM_BUFFER_COUNT; i++) {
        if (ctx->cap_buffer[i] && ctx->cap_buffer[i] != MAP_FAILED) {
            munmap(ctx->cap_buffer[i], ctx->cap_buf_size[i]);
            ctx->cap_buffer[i] = NULL;
        }
    }

    ESP_LOGI(TAG, "Camera streaming stopped");
    return ESP_OK;
}

esp_err_t camera_dequeue(camera_ctx_t *ctx, uint32_t *buf_index, uint32_t *bytesused)
{
    struct v4l2_buffer buf = {
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };

    ESP_RETURN_ON_FALSE(ioctl(ctx->cap_fd, VIDIOC_DQBUF, &buf) == 0,
                        ESP_FAIL, TAG, "DQBUF failed");

    *buf_index = buf.index;
    *bytesused = buf.bytesused;
    return ESP_OK;
}

esp_err_t camera_enqueue(camera_ctx_t *ctx, uint32_t buf_index)
{
    struct v4l2_buffer buf = {
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
        .index  = buf_index,
    };

    ESP_RETURN_ON_FALSE(ioctl(ctx->cap_fd, VIDIOC_QBUF, &buf) == 0,
                        ESP_FAIL, TAG, "QBUF failed");
    return ESP_OK;
}

/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "esp_video_isp_ioctl.h"
#include "esp_ldo_regulator.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "board_olimex_p4.h"
#include "camera_pipeline.h"

static const char *TAG = "cam_pipe";

static esp_ldo_channel_handle_t s_ldo_handle;

esp_err_t camera_init(void)
{
    /*
     * Step 1: Power up MIPI PHY via internal LDO.
     * Must happen before any MIPI CSI or sensor operations.
     */
    ESP_LOGI(TAG, "Enabling MIPI PHY LDO (chan=%d, %dmV)", BOARD_CSI_LDO_CHAN, BOARD_CSI_LDO_MV);
    esp_ldo_channel_config_t ldo_cfg = {
        .chan_id = BOARD_CSI_LDO_CHAN,
        .voltage_mv = BOARD_CSI_LDO_MV,
    };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &s_ldo_handle),
                        TAG, "LDO init failed");

    /*
     * Step 2: Provide master clock (XCLK) to OV5647 sensor.
     * Without this clock the sensor's I2C slave is dead and
     * detection will fail silently in esp_video_init().
     */
    ESP_LOGI(TAG, "Starting XCLK: GPIO%d @ %d Hz", BOARD_CAM_XCLK_PIN, BOARD_CAM_XCLK_FREQ);
    esp_cam_sensor_xclk_handle_t xclk_handle;
    ESP_RETURN_ON_ERROR(
        esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER, &xclk_handle),
        TAG, "XCLK allocate failed");

    esp_cam_sensor_xclk_config_t xclk_cfg = {
        .esp_clock_router_cfg = {
            .xclk_pin = BOARD_CAM_XCLK_PIN,
            .xclk_freq_hz = BOARD_CAM_XCLK_FREQ,
        },
    };
    ESP_RETURN_ON_ERROR(esp_cam_sensor_xclk_start(xclk_handle, &xclk_cfg),
                        TAG, "XCLK start failed");

    /* Let sensor PLL lock after clock is applied */
    vTaskDelay(pdMS_TO_TICKS(20));

    /*
     * Step 3: Initialize video subsystem (I2C/SCCB, sensor detect, ISP, codecs).
     * LDO is already on, so we tell esp_video not to init it again.
     */
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
        .reset_pin     = BOARD_CAM_RESET_PIN,
        .pwdn_pin      = BOARD_CAM_PWDN_PIN,
        .dont_init_ldo = BOARD_CSI_DONT_INIT_LDO,
    };

    static const esp_video_init_config_t video_config = {
        .csi = &csi_config,
    };

    ESP_LOGI(TAG, "Initializing video subsystem (MIPI CSI + ISP + OV5647)");
    ESP_LOGI(TAG, "  I2C port=%d SDA=%d SCL=%d freq=%d",
             BOARD_I2C_PORT, BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN, BOARD_I2C_FREQ);

    esp_err_t ret = esp_video_init(&video_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /*
     * Step 4: Verify the CSI capture device was actually created.
     * esp_video_init() returns OK even when no sensor is detected
     * (it also creates codec devices that succeed independently).
     */
    int test_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    if (test_fd < 0) {
        ESP_LOGE(TAG, "Sensor not detected: %s does not exist", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
        ESP_LOGE(TAG, "Check: (1) OV5647 ribbon cable seated?");
        ESP_LOGE(TAG, "       (2) I2C SDA=GPIO%d SCL=GPIO%d correct?",
                 BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN);
        ESP_LOGE(TAG, "       (3) XCLK on GPIO%d reaching sensor?", BOARD_CAM_XCLK_PIN);
        return ESP_ERR_NOT_FOUND;
    }
    close(test_fd);

    ESP_LOGI(TAG, "Camera sensor detected, %s ready", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
    return ESP_OK;
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

/*
 * ISP color profiles derived from Raspberry Pi libcamera OV5647 tuning.
 * Each profile has a CCM tuned for a specific color temperature range
 * and matching white balance gains.
 *
 * Source: https://github.com/raspberrypi/libcamera ov5647.json
 */
typedef struct {
    const char *name;
    float ccm[3][3];
    float wb_red_gain;
    float wb_blue_gain;
} isp_color_profile_t;

static const isp_color_profile_t s_profiles[] = {
    [0] = {  /* 2873K: Incandescent / Tungsten */
        .name = "Tungsten",
        .ccm = {
            {  1.88195f, -0.26249f, -0.61946f },
            { -0.40081f,  1.77632f, -0.37551f },
            {  0.00257f, -0.75415f,  1.75158f },
        },
        .wb_red_gain  = 1.50f,
        .wb_blue_gain = 1.76f,
    },
    [1] = {  /* 3725K: Warm Indoor */
        .name = "Indoor-Warm",
        .ccm = {
            {  1.94343f, -0.50885f, -0.43458f },
            { -0.38988f,  1.85523f, -0.46535f },
            { -0.00887f, -0.74623f,  1.75510f },
        },
        .wb_red_gain  = 1.46f,
        .wb_blue_gain = 1.49f,
    },
    [2] = {  /* 5095K: Fluorescent / Office */
        .name = "Fluorescent",
        .ccm = {
            {  2.00666f, -0.63316f, -0.37350f },
            { -0.40071f,  1.94742f, -0.54671f },
            { -0.03109f, -0.83048f,  1.86157f },
        },
        .wb_red_gain  = 1.37f,
        .wb_blue_gain = 1.33f,
    },
    [3] = {  /* 6015K: Daylight / Outdoor */
        .name = "Daylight",
        .ccm = {
            {  1.99726f, -0.63965f, -0.35761f },
            { -0.40616f,  1.94421f, -0.53805f },
            { -0.01886f, -0.73970f,  1.75855f },
        },
        .wb_red_gain  = 1.30f,
        .wb_blue_gain = 1.24f,
    },
    [4] = {  /* 6865K: Cloudy / Overcast */
        .name = "Cloudy",
        .ccm = {
            {  2.05107f, -0.68023f, -0.37084f },
            { -0.42693f,  1.93461f, -0.50768f },
            { -0.01654f, -0.69652f,  1.71306f },
        },
        .wb_red_gain  = 1.26f,
        .wb_blue_gain = 1.21f,
    },
    [5] = {  /* 7600K: Cool Daylight / Shade */
        .name = "Shade",
        .ccm = {
            {  2.06599f, -0.39161f, -0.67439f },
            { -0.43251f,  1.92138f, -0.48887f },
            { -0.01948f, -0.77319f,  1.79267f },
        },
        .wb_red_gain  = 1.22f,
        .wb_blue_gain = 1.19f,
    },
};

#define ISP_NUM_PROFILES  (sizeof(s_profiles) / sizeof(s_profiles[0]))
#define ISP_DEFAULT_PROFILE 3   /* Daylight - broadest appeal across lighting conditions */

static void camera_apply_isp_profile(int cap_fd, int profile_idx)
{
    if (profile_idx >= ISP_NUM_PROFILES) {
        profile_idx = ISP_DEFAULT_PROFILE;
    }
    const isp_color_profile_t *p = &s_profiles[profile_idx];

    /* ISP controls go to the ISP device, not the CSI capture device */
    int fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    if (fd < 0) {
        ESP_LOGW(TAG, "Cannot open ISP device %s, skipping color config",
                 ESP_VIDEO_ISP1_DEVICE_NAME);
        return;
    }

    ESP_LOGI(TAG, "Applying ISP profile [%d] '%s' via %s",
             profile_idx, p->name, ESP_VIDEO_ISP1_DEVICE_NAME);

    /* Color Correction Matrix */
    esp_video_isp_ccm_t ccm = { .enable = true };
    memcpy(ccm.matrix, p->ccm, sizeof(p->ccm));

    struct v4l2_ext_control ccm_ctrl = {
        .id = V4L2_CID_USER_ESP_ISP_CCM, .size = sizeof(ccm), .ptr = &ccm,
    };
    struct v4l2_ext_controls ctrls = { .count = 1, .controls = &ccm_ctrl };
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) == 0) {
        ESP_LOGI(TAG, "  CCM applied");
    } else {
        ESP_LOGW(TAG, "  CCM set failed");
    }

    /* White Balance gains */
    esp_video_isp_wb_t wb = {
        .enable = true, .red_gain = p->wb_red_gain, .blue_gain = p->wb_blue_gain,
    };

    struct v4l2_ext_control wb_ctrl = {
        .id = V4L2_CID_USER_ESP_ISP_WB, .size = sizeof(wb), .ptr = &wb,
    };
    ctrls.controls = &wb_ctrl;
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) == 0) {
        ESP_LOGI(TAG, "  WB applied (R=%.2f B=%.2f)", wb.red_gain, wb.blue_gain);
    } else {
        ESP_LOGW(TAG, "  WB set failed");
    }

    /* Gamma Correction: sRGB-like curve (gamma ~2.2)
     * x values must match esp_isp_gamma_fill_curve_points() pattern:
     * - pt[0].x must be > 0 (x_prev starts at 0, validation: x_i > x_prev)
     * - each x delta must be a power of 2 (delta & (delta-1) == 0)
     * - pt[15].x must be exactly 255 (validated, then treated as 256 internally)
     * Valid sequence: 16, 32, 48, ..., 240, 255 (deltas: all 16, last treated as 16) */
    esp_video_isp_gamma_t gamma = {
        .enable = true,
        .points = {
            { .x =  16, .y =  72 },
            { .x =  32, .y =  99 },
            { .x =  48, .y = 119 },
            { .x =  64, .y = 136 },
            { .x =  80, .y = 151 },
            { .x =  96, .y = 164 },
            { .x = 112, .y = 175 },
            { .x = 128, .y = 186 },
            { .x = 144, .y = 197 },
            { .x = 160, .y = 206 },
            { .x = 176, .y = 215 },
            { .x = 192, .y = 224 },
            { .x = 208, .y = 232 },
            { .x = 224, .y = 240 },
            { .x = 240, .y = 248 },
            { .x = 255, .y = 255 },
        },
    };

    struct v4l2_ext_control gamma_ctrl = {
        .id = V4L2_CID_USER_ESP_ISP_GAMMA, .size = sizeof(gamma), .ptr = &gamma,
    };
    ctrls.controls = &gamma_ctrl;
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) == 0) {
        ESP_LOGI(TAG, "  Gamma applied (sRGB ~2.2)");
    } else {
        ESP_LOGW(TAG, "  Gamma set failed");
    }

    /* Sharpening: moderate edge enhancement */
    esp_video_isp_sharpen_t sharpen = {
        .enable = true,
        .h_thresh = 40,
        .l_thresh = 10,
        .h_coeff = 1.5f,
        .m_coeff = 0.5f,
        .matrix = {
            { 1, 2, 1 },
            { 2, 4, 2 },
            { 1, 2, 1 },
        },
    };

    struct v4l2_ext_control sharpen_ctrl = {
        .id = V4L2_CID_USER_ESP_ISP_SHARPEN, .size = sizeof(sharpen), .ptr = &sharpen,
    };
    ctrls.controls = &sharpen_ctrl;
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) == 0) {
        ESP_LOGI(TAG, "  Sharpen applied (moderate)");
    } else {
        ESP_LOGW(TAG, "  Sharpen set failed");
    }

    /*
     * BLC (Black Level Correction): OV5647 calibrated at 1024 (10-bit).
     * Not available in ESP-IDF v5.5.1 — the esp_isp_blc_*() functions
     * don't exist yet.  The esp-video-components BLC wrapper compiles
     * but returns ESP_ERR_NOT_SUPPORTED (0x106) at runtime.
     * TODO: Enable when IDF adds BLC driver support.
     */

    close(fd);
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

    /* Apply ISP color correction after streaming is active */
    camera_apply_isp_profile(ctx->cap_fd, ISP_DEFAULT_PROFILE);

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

/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "esp_log.h"
#include "esp_check.h"
#include "linux/videodev2.h"
#include "esp_video_device.h"
#include "usb_device_uvc.h"
#include "uvc_controls.h"
#include "camera_pipeline.h"
#include "esp_video_isp_ioctl.h"

static const char *TAG = "uvc_ctrl";

/* Cached ISP device fd — opened once at init, used by PU control callbacks */
static int s_isp_fd = -1;

static esp_err_t set_ext_ctrl(int fd, uint32_t ctrl_class, uint32_t ctrl_id, int32_t value)
{
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control control;

    memset(&controls, 0, sizeof(controls));
    memset(&control, 0, sizeof(control));

    controls.ctrl_class = ctrl_class;
    controls.count = 1;
    controls.controls = &control;
    control.id = ctrl_id;
    control.value = value;

    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "Failed to set control 0x%08lx = %ld",
                 (unsigned long)ctrl_id, (long)value);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t set_isp_struct_ctrl(int fd, uint32_t ctrl_id, void *data, size_t size)
{
    struct v4l2_ext_controls controls;
    struct v4l2_ext_control control;

    memset(&controls, 0, sizeof(controls));
    memset(&control, 0, sizeof(control));

    controls.ctrl_class = V4L2_CID_USER_CLASS;
    controls.count = 1;
    controls.controls = &control;
    control.id = ctrl_id;
    control.size = size;
    control.ptr = data;

    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "Failed to set ISP control 0x%08lx",
                 (unsigned long)ctrl_id);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t uvc_ctrl_init(void)
{
    if (s_isp_fd >= 0) {
        return ESP_OK;  /* Already initialized */
    }

    s_isp_fd = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    if (s_isp_fd < 0) {
        ESP_LOGW(TAG, "Cannot open ISP device %s for PU controls",
                 ESP_VIDEO_ISP1_DEVICE_NAME);
        return ESP_FAIL;
    }

    /* Sync XU ISP profile default with Kconfig setting */
    uvc_xu_set_default(0x01, CONFIG_ISP_DEFAULT_PROFILE_INDEX);

    ESP_LOGI(TAG, "PU/XU control bridge initialized (ISP fd=%d)", s_isp_fd);
    return ESP_OK;
}

void uvc_ctrl_deinit(void)
{
    if (s_isp_fd >= 0) {
        close(s_isp_fd);
        s_isp_fd = -1;
        ESP_LOGI(TAG, "PU control bridge deinitialized");
    }
}

esp_err_t uvc_ctrl_set_h264_params(int m2m_fd, int bitrate, int i_period,
                                    int min_qp, int max_qp)
{
    ESP_LOGI(TAG, "H.264: bitrate=%d, I-period=%d, QP=%d-%d",
             bitrate, i_period, min_qp, max_qp);

    set_ext_ctrl(m2m_fd, V4L2_CID_CODEC_CLASS,
                 V4L2_CID_MPEG_VIDEO_BITRATE, bitrate);
    set_ext_ctrl(m2m_fd, V4L2_CID_CODEC_CLASS,
                 V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, i_period);
    set_ext_ctrl(m2m_fd, V4L2_CID_CODEC_CLASS,
                 V4L2_CID_MPEG_VIDEO_H264_MIN_QP, min_qp);
    set_ext_ctrl(m2m_fd, V4L2_CID_CODEC_CLASS,
                 V4L2_CID_MPEG_VIDEO_H264_MAX_QP, max_qp);

    return ESP_OK;
}

esp_err_t uvc_ctrl_set_jpeg_quality(int m2m_fd, int quality)
{
    ESP_LOGI(TAG, "JPEG: quality=%d", quality);

    return set_ext_ctrl(m2m_fd, V4L2_CID_JPEG_CLASS,
                        V4L2_CID_JPEG_COMPRESSION_QUALITY, quality);
}

/*
 * ---- UVC PU control -> V4L2 ISP bridge ----
 *
 * Called from the TinyUSB task when the host sends a SET_CUR request
 * for a Processing Unit control. Uses the cached ISP fd to avoid
 * open/close overhead on every control change.
 *
 * UVC PU control selectors (must match usb_device_uvc.c definitions):
 *   0x02 = Brightness   -> V4L2_CID_BRIGHTNESS  on ISP
 *   0x03 = Contrast     -> V4L2_CID_CONTRAST    on ISP
 *   0x04 = Sharpness    -> ISP sharpen h_thresh (0=off, 1-100)
 *   0x06 = Hue          -> V4L2_CID_HUE         on ISP
 *   0x07 = Saturation   -> V4L2_CID_SATURATION  on ISP
 *   0x10 = Gain         -> ISP BF denoise level (0-1=off, 2-20=on)
 */
void uvc_pu_control_set_cb(uint8_t cs, int16_t value)
{
    if (s_isp_fd < 0) {
        ESP_LOGW(TAG, "PU control ignored: ISP not initialized");
        return;
    }

    /* WB Temperature (0x0A) is repurposed as ISP profile selector (0-5) */
    if (cs == 0x0A) {
        ESP_LOGI(TAG, "PU WB Temp -> ISP profile %d", (int)value);
        camera_apply_isp_profile((int)value);
        return;
    }

    uint32_t v4l2_cid;
    switch (cs) {
    case 0x02: v4l2_cid = V4L2_CID_BRIGHTNESS; break;
    case 0x03: v4l2_cid = V4L2_CID_CONTRAST;   break;
    case 0x04: {  /* Sharpness -> ISP sharpen h_thresh */
        esp_video_isp_sharpen_t sharpen = {
            .enable = (value > 0),
            .h_thresh = (uint8_t)value,
            .l_thresh = (uint8_t)(value / 4),
            .h_coeff = 1.5f,
            .m_coeff = 0.5f,
            .matrix = {
                { 1, 2, 1 },
                { 2, 4, 2 },
                { 1, 2, 1 },
            },
        };
        esp_err_t ret = set_isp_struct_ctrl(s_isp_fd, V4L2_CID_USER_ESP_ISP_SHARPEN,
                                             &sharpen, sizeof(sharpen));
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "PU Sharpness -> ISP h_thresh=%d", (int)value);
        }
        return;
    }
    case 0x06: v4l2_cid = V4L2_CID_HUE;        break;
    case 0x07: v4l2_cid = V4L2_CID_SATURATION;  break;
    case 0x10: {  /* Gain -> BF denoising level */
        esp_video_isp_bf_t bf = {
            .enable = (value >= 2),
            .level = (value < 2) ? 2 : (uint8_t)value,
            .matrix = {
                { 1, 2, 1 },
                { 2, 4, 2 },
                { 1, 2, 1 },
            },
        };
        esp_err_t ret = set_isp_struct_ctrl(s_isp_fd, V4L2_CID_USER_ESP_ISP_BF,
                                             &bf, sizeof(bf));
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "PU Gain -> BF denoise level=%d %s",
                     (int)value, bf.enable ? "ON" : "OFF");
        }
        return;
    }
    default:
        ESP_LOGW(TAG, "Unknown PU cs=0x%02x", cs);
        return;
    }

    esp_err_t ret = set_ext_ctrl(s_isp_fd, V4L2_CID_USER_CLASS, v4l2_cid, (int32_t)value);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "PU cs=0x%02x -> V4L2 0x%08lx = %d",
                 cs, (unsigned long)v4l2_cid, (int)value);
    }
}

/*
 * ---- UVC XU control -> ISP profile switch ----
 *
 * Called from the TinyUSB task when the host sends a SET_CUR request
 * for an Extension Unit control. Overrides the weak callback in
 * usb_device_uvc.c.
 *
 * XU control selectors:
 *   0x01 = ISP Profile Select (0=Tungsten .. 5=Shade)
 */
void uvc_xu_control_set_cb(uint8_t cs, uint8_t value)
{
    if (cs == 0x01) {
        ESP_LOGI(TAG, "XU ISP Profile: %u", value);
        camera_apply_isp_profile((int)value);
    } else {
        ESP_LOGW(TAG, "Unknown XU cs=0x%02x", cs);
    }
}

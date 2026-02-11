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
 *   0x06 = Hue          -> V4L2_CID_HUE         on ISP
 *   0x07 = Saturation   -> V4L2_CID_SATURATION  on ISP
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
    case 0x06: v4l2_cid = V4L2_CID_HUE;        break;
    case 0x07: v4l2_cid = V4L2_CID_SATURATION;  break;
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

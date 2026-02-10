/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <sys/ioctl.h>
#include "esp_log.h"
#include "esp_check.h"
#include "linux/videodev2.h"
#include "uvc_controls.h"

static const char *TAG = "uvc_ctrl";

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

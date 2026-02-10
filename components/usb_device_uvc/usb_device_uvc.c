/*
 * SPDX-FileCopyrightText: 2022-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Forked from usb_device_uvc v1.2.0 to add multi-format support.
 * Key change: tud_video_commit_cb uses bFormatIndex for per-format frame lookup.
 */

#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "esp_check.h"
#if CONFIG_TINYUSB_RHPORT_HS
#include "soc/hp_sys_clkrst_reg.h"
#include "soc/hp_system_reg.h"
#endif
#include "esp_private/usb_phy.h"
#include "tusb.h"
#include "usb_device_uvc.h"
#include "uvc_frame_config.h"

static const char *TAG = "usbd_uvc";

#define UVC_CAM_NUM 1

#define TUSB_EVENT_EXIT         (1<<0)
#define TUSB_EVENT_EXIT_DONE    (1<<1)
#define UVC1_EVENT_EXIT         (1<<2)
#define UVC1_EVENT_EXIT_DONE    (1<<3)

typedef struct {
    usb_phy_handle_t phy_hdl;
    bool uvc_init[UVC_CAM_NUM];
    uvc_format_t format[UVC_CAM_NUM];
    uvc_device_config_t user_config[UVC_CAM_NUM];
    TaskHandle_t uvc_task_hdl[UVC_CAM_NUM];
    TaskHandle_t tusb_task_hdl;
    uint32_t interval_ms[UVC_CAM_NUM];
    EventGroupHandle_t event_group;
} uvc_device_t;

static uvc_device_t s_uvc_device;

static void usb_phy_init(void)
{
    usb_phy_config_t phy_conf = {
        .controller = USB_PHY_CTRL_OTG,
        .otg_mode = USB_OTG_MODE_DEVICE,
#if CONFIG_TINYUSB_RHPORT_HS
        .target = USB_PHY_TARGET_EXT,
        .otg_speed = USB_PHY_SPEED_HIGH,
#else
        .target = USB_PHY_TARGET_INT,
#endif
    };
    usb_new_phy(&phy_conf, &s_uvc_device.phy_hdl);
}

static inline uint32_t get_time_millis(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void tusb_device_task(void *arg)
{
    while (1) {
        EventBits_t uxBits = xEventGroupGetBits(s_uvc_device.event_group);
        if (uxBits & TUSB_EVENT_EXIT) {
            ESP_LOGI(TAG, "TUSB task exit");
            break;
        }
        tud_task();
    }
    xEventGroupSetBits(s_uvc_device.event_group, TUSB_EVENT_EXIT_DONE);
    vTaskDelete(NULL);
}

void tud_mount_cb(void)
{
    ESP_LOGI(TAG, "Mount");
}

void tud_umount_cb(void)
{
    ESP_LOGI(TAG, "UN-Mount");
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    if (s_uvc_device.user_config[0].stop_cb) {
        s_uvc_device.user_config[0].stop_cb(s_uvc_device.user_config[0].cb_ctx);
    }
    ESP_LOGI(TAG, "Suspend");
}

void tud_resume_cb(void)
{
    ESP_LOGI(TAG, "Resume");
}

/*
 * Video streaming task: polls TinyUSB streaming state, captures frames,
 * copies into the UVC transfer buffer, and submits via tud_video_n_frame_xfer.
 */
static void video_task(void *arg)
{
    uint32_t start_ms = 0;
    uint32_t frame_num = 0;
    uint32_t frame_len = 0;
    uint32_t already_start = 0;
    uint32_t tx_busy = 0;
    uint8_t *uvc_buffer = s_uvc_device.user_config[0].uvc_buffer;
    uint32_t uvc_buffer_size = s_uvc_device.user_config[0].uvc_buffer_size;
    uvc_fb_t *pic = NULL;

    while (1) {
        EventBits_t uxBits = xEventGroupGetBits(s_uvc_device.event_group);
        if (uxBits & UVC1_EVENT_EXIT) {
            ESP_LOGI(TAG, "UVC task exit");
            break;
        }

        if (!tud_video_n_streaming(0, 0)) {
            already_start = 0;
            frame_num = 0;
            tx_busy = 0;
            vTaskDelay(1);
            continue;
        }

        if (!already_start) {
            already_start = 1;
            start_ms = get_time_millis();
        }

        uint32_t cur = get_time_millis();
        if (cur - start_ms < s_uvc_device.interval_ms[0]) {
            vTaskDelay(1);
            continue;
        }

        if (tx_busy) {
            uint32_t xfer_done = ulTaskNotifyTake(pdTRUE, 1);
            if (xfer_done == 0) {
                continue;
            }
            ++frame_num;
            tx_busy = 0;
        }

        start_ms += s_uvc_device.interval_ms[0];
        pic = s_uvc_device.user_config[0].fb_get_cb(s_uvc_device.user_config[0].cb_ctx);
        if (!pic) {
            ESP_LOGE(TAG, "Failed to capture picture");
            continue;
        }

        if (pic->len > uvc_buffer_size) {
            ESP_LOGW(TAG, "frame size %" PRIu32 " > buffer %" PRIu32 ", dropping",
                     (uint32_t)pic->len, uvc_buffer_size);
            s_uvc_device.user_config[0].fb_return_cb(pic, s_uvc_device.user_config[0].cb_ctx);
            continue;
        }
        frame_len = pic->len;
        memcpy(uvc_buffer, pic->buf, frame_len);
        s_uvc_device.user_config[0].fb_return_cb(pic, s_uvc_device.user_config[0].cb_ctx);
        tx_busy = 1;
        tud_video_n_frame_xfer(0, 0, (void *)uvc_buffer, frame_len);
    }

    xEventGroupSetBits(s_uvc_device.event_group, UVC1_EVENT_EXIT_DONE);
    vTaskDelete(NULL);
}

void tud_video_frame_xfer_complete_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx)
{
    (void)stm_idx;
    xTaskNotifyGive(s_uvc_device.uvc_task_hdl[ctl_idx]);
}

/*
 * Called when the USB host commits a video format via VS_COMMIT_CONTROL.
 * This is where multi-format handling happens: we use bFormatIndex to
 * determine which format (YUY2/MJPEG/H264) and look up the correct
 * per-format frame table for resolution/fps.
 */
int tud_video_commit_cb(uint_fast8_t ctl_idx, uint_fast8_t stm_idx,
                        video_probe_and_commit_control_t const *parameters)
{
    (void)stm_idx;

    uint8_t fmt_idx = parameters->bFormatIndex;
    uint8_t frm_idx = parameters->bFrameIndex;

    ESP_LOGI(TAG, "Commit: bFormatIndex=%u bFrameIndex=%u dwFrameInterval=%" PRIu32,
             fmt_idx, frm_idx, parameters->dwFrameInterval);

    /* Look up frame info from per-format table */
    const uvc_frame_info_t *fi = uvc_get_frame_info(fmt_idx, frm_idx);
    if (!fi) {
        ESP_LOGE(TAG, "Invalid format/frame index: %u/%u", fmt_idx, frm_idx);
        return VIDEO_ERROR_OUT_OF_RANGE;
    }

    /* Map bFormatIndex to uvc_format_t */
    uvc_format_t format;
    switch (fmt_idx) {
    case 1:  format = UVC_FORMAT_UNCOMPR; break;
    case 2:  format = UVC_FORMAT_JPEG;    break;
    case 3:  format = UVC_FORMAT_H264;    break;
    default: return VIDEO_ERROR_OUT_OF_RANGE;
    }

    s_uvc_device.format[ctl_idx] = format;
    s_uvc_device.interval_ms[ctl_idx] = parameters->dwFrameInterval / 10000;

    ESP_LOGI(TAG, "Starting: %ux%u @%ufps format=%d",
             fi->width, fi->height, fi->max_fps, format);

    esp_err_t ret = s_uvc_device.user_config[ctl_idx].start_cb(
        format, fi->width, fi->height, fi->max_fps,
        s_uvc_device.user_config[ctl_idx].cb_ctx);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start_cb failed: %s", esp_err_to_name(ret));
        return VIDEO_ERROR_OUT_OF_RANGE;
    }
    return VIDEO_ERROR_NONE;
}

esp_err_t uvc_device_config(int index, uvc_device_config_t *config)
{
    ESP_RETURN_ON_FALSE(index < UVC_CAM_NUM, ESP_ERR_INVALID_ARG, TAG, "index is invalid");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(config->start_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "start_cb is NULL");
    ESP_RETURN_ON_FALSE(config->fb_get_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "fb_get_cb is NULL");
    ESP_RETURN_ON_FALSE(config->fb_return_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "fb_return_cb is NULL");
    ESP_RETURN_ON_FALSE(config->stop_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "stop_cb is NULL");
    ESP_RETURN_ON_FALSE(config->uvc_buffer != NULL, ESP_ERR_INVALID_ARG, TAG, "uvc_buffer is NULL");
    ESP_RETURN_ON_FALSE(config->uvc_buffer_size > 0, ESP_ERR_INVALID_ARG, TAG, "uvc_buffer_size is 0");

    s_uvc_device.user_config[index] = *config;
    s_uvc_device.interval_ms[index] = 33; /* default 30fps, updated by commit_cb */
    s_uvc_device.uvc_init[index] = true;
    return ESP_OK;
}

esp_err_t uvc_device_init(void)
{
    ESP_RETURN_ON_FALSE(s_uvc_device.uvc_init[0], ESP_ERR_INVALID_STATE, TAG, "uvc device not configured");

    s_uvc_device.event_group = xEventGroupCreate();
    if (s_uvc_device.event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_FAIL;
    }

    usb_phy_init();
    bool usb_init = tusb_init();
    if (!usb_init) {
        ESP_LOGE(TAG, "USB Device Stack Init Fail");
        vEventGroupDelete(s_uvc_device.event_group);
        s_uvc_device.event_group = NULL;
        return ESP_FAIL;
    }

    BaseType_t core_id = (CONFIG_UVC_TINYUSB_TASK_CORE < 0) ? tskNO_AFFINITY : CONFIG_UVC_TINYUSB_TASK_CORE;
    xTaskCreatePinnedToCore(tusb_device_task, "TinyUSB", 4096, NULL,
                            CONFIG_UVC_TINYUSB_TASK_PRIORITY, &s_uvc_device.tusb_task_hdl, core_id);

    core_id = (CONFIG_UVC_CAM1_TASK_CORE < 0) ? tskNO_AFFINITY : CONFIG_UVC_CAM1_TASK_CORE;
    xTaskCreatePinnedToCore(video_task, "UVC", 4096, NULL,
                            CONFIG_UVC_CAM1_TASK_PRIORITY, &s_uvc_device.uvc_task_hdl[0], core_id);

    ESP_LOGI(TAG, "UVC Device Start (Multi-format: YUY2+MJPEG+H264)");
    return ESP_OK;
}

esp_err_t uvc_device_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_uvc_device.uvc_init[0], ESP_ERR_INVALID_STATE, TAG, "uvc device not init");
    ESP_RETURN_ON_FALSE(s_uvc_device.event_group != NULL, ESP_ERR_INVALID_STATE, TAG, "event group is NULL");

    xEventGroupSetBits(s_uvc_device.event_group, UVC1_EVENT_EXIT);
    xEventGroupWaitBits(s_uvc_device.event_group, UVC1_EVENT_EXIT_DONE, pdTRUE, pdTRUE, portMAX_DELAY);

    if (s_uvc_device.user_config[0].stop_cb) {
        s_uvc_device.user_config[0].stop_cb(s_uvc_device.user_config[0].cb_ctx);
    }

    xEventGroupSetBits(s_uvc_device.event_group, TUSB_EVENT_EXIT);
    EventBits_t bits = xEventGroupWaitBits(s_uvc_device.event_group, TUSB_EVENT_EXIT_DONE,
                                           pdTRUE, pdTRUE, pdMS_TO_TICKS(5000));
    if (!(bits & TUSB_EVENT_EXIT_DONE)) {
        ESP_LOGW(TAG, "TinyUSB task exit timeout, force delete");
        if (s_uvc_device.tusb_task_hdl) {
            vTaskDelete(s_uvc_device.tusb_task_hdl);
            s_uvc_device.tusb_task_hdl = NULL;
        }
    }

    vEventGroupDelete(s_uvc_device.event_group);
    s_uvc_device.event_group = NULL;

    tusb_teardown();
    if (s_uvc_device.phy_hdl) {
        usb_del_phy(s_uvc_device.phy_hdl);
        s_uvc_device.phy_hdl = NULL;
    }

    memset(s_uvc_device.uvc_init, 0, sizeof(s_uvc_device.uvc_init));
    ESP_LOGI(TAG, "UVC Device Deinit");
    return ESP_OK;
}

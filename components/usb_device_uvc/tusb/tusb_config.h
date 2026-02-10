/*
 * SPDX-FileCopyrightText: 2019 Ha Thach (tinyusb.org)
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * TinyUSB configuration for multi-format UVC webcam.
 */

#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#include "uvc_frame_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Board Specific ---- */
#ifdef CONFIG_TINYUSB_RHPORT_HS
#   define CFG_TUSB_RHPORT1_MODE    OPT_MODE_DEVICE | OPT_MODE_HIGH_SPEED
#else
#   define CFG_TUSB_RHPORT0_MODE    OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED
#endif

/* ---- Common ---- */
#ifndef CFG_TUSB_MCU
#error CFG_TUSB_MCU must be defined
#endif

#ifndef ESP_PLATFORM
#define ESP_PLATFORM 1
#endif

#ifndef CFG_TUSB_OS
#define CFG_TUSB_OS           OPT_OS_FREERTOS
#endif

#if TU_CHECK_MCU(OPT_MCU_ESP32S2, OPT_MCU_ESP32S3, OPT_MCU_ESP32P4)
#define CFG_TUSB_OS_INC_PATH    freertos/
#endif

#ifndef CFG_TUSB_DEBUG
#define CFG_TUSB_DEBUG        0
#endif

#define CFG_TUD_ENABLED       1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN        __attribute__ ((aligned(4)))
#endif

/* ---- Device ---- */
#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

/* ---- Video Class ---- */
#define CFG_TUD_VIDEO            1
#define CFG_TUD_VIDEO_STREAMING  1

/* Bulk transfer for HS */
#define CFG_TUD_VIDEO_STREAMING_BULK  1

#ifdef CONFIG_TINYUSB_RHPORT_HS
#define CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE  512
#else
#define CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE  64
#endif

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */

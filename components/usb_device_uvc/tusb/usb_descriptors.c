/*
 * SPDX-FileCopyrightText: 2019 Ha Thach (tinyusb.org)
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: MIT
 *
 * USB descriptors for multi-format UVC webcam (Bulk, UVC 1.5).
 */

#include "tusb.h"
#include "usb_descriptors.h"

/* ---- Device Descriptor ---- */
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = CONFIG_TUSB_VID,
    .idProduct          = CONFIG_TUSB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

/* ---- Configuration Descriptor ---- */
uint8_t const desc_fs_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 500),
    TUD_VIDEO_CAPTURE_DESCRIPTOR_MULTIFORMAT_BULK(
        4,                                      /* _stridx */
        ITF_NUM_VIDEO_CONTROL,                  /* _itf */
        EPNUM_VIDEO_IN,                         /* _epin */
        CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE      /* _epsize */
    ),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_fs_configuration;
}

/* ---- String Descriptors ---- */
char const *string_desc_arr[] = {
    (const char[]){ 0x09, 0x04 },   /* 0: English */
    CONFIG_TUSB_MANUFACTURER,        /* 1: Manufacturer */
    CONFIG_TUSB_PRODUCT,             /* 2: Product */
    CONFIG_TUSB_SERIAL_NUM,          /* 3: Serial */
    "UVC Camera",                    /* 4: UVC Interface */
};

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    uint8_t chr_count;

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
            return NULL;
        }
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) {
            chr_count = 31;
        }
        for (uint8_t i = 0; i < chr_count; i++) {
            _desc_str[1 + i] = str[i];
        }
    }

    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}

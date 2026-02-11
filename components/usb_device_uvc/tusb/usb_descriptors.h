/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Multi-format UVC 1.5 descriptor for ESP32-P4 webcam (Bulk mode).
 *
 * Advertises 3 formats simultaneously: UYVY, MJPEG, H.264
 * Each format supports 3 resolutions: 800x800, 640x480, 320x240
 *
 * Descriptor hierarchy:
 *   VideoControl Interface
 *     +-- Camera Terminal (IT)
 *     +-- Processing Unit (PU)
 *     +-- Extension Unit (XU) — ISP Color Profile
 *     +-- Output Terminal (OT)
 *   VideoStreaming Interface (Bulk)
 *     +-- VS Input Header (bNumFormats=3)
 *     +-- Format 1: UYVY (3 frames: 800x800, 640x480, 320x240)
 *     +-- Format 2: MJPEG (3 frames: 800x800, 640x480, 320x240)
 *     +-- Format 3: H.264 (3 frames: 800x800, 640x480, 320x240)
 *     +-- Color Matching
 *     +-- Bulk Endpoint
 */

#ifndef _USB_DESCRIPTORS_H_
#define _USB_DESCRIPTORS_H_

#include "uvc_frame_config.h"

/* ---------- entity IDs --------------------------------------------------- */
#define UVC_ENTITY_CAP_INPUT_TERMINAL   0x01
#define UVC_ENTITY_PROCESSING_UNIT      0x02
#define UVC_ENTITY_CAP_OUTPUT_TERMINAL  0x03
#define UVC_ENTITY_ISP_XU               0x04

/* ---------- clock -------------------------------------------------------- */
#define UVC_CLOCK_FREQUENCY  27000000

/* ---------- helper: frame interval in 100ns units ------------------------ */
#define FI(fps)  (10000000 / (fps))

/* ---------- interface enumeration ---------------------------------------- */
enum {
    ITF_NUM_VIDEO_CONTROL,
    ITF_NUM_VIDEO_STREAMING,
    ITF_NUM_TOTAL
};

#define EPNUM_VIDEO_IN  0x81

/* ---------- Processing Unit descriptor (UVC 1.5 Table 4-6) --------------- */
#define TUD_VIDEO_DESC_PROCESSING_UNIT_LEN  13

#define TUD_VIDEO_DESC_PROCESSING_UNIT(_unitid, _srcid, _bmc0, _bmc1, _bmc2) \
    TUD_VIDEO_DESC_PROCESSING_UNIT_LEN, \
    TUSB_DESC_CS_INTERFACE, VIDEO_CS_ITF_VC_PROCESSING_UNIT, \
    _unitid, _srcid, \
    U16_TO_U8S_LE(0x0000), /* wMaxMultiplier */ \
    0x03,                   /* bControlSize */ \
    _bmc0, _bmc1, _bmc2, \
    0x00,                   /* iProcessing */ \
    0x00                    /* bmVideoStandards */

/*
 * PU bmControls bitmap (UVC 1.5 spec Table 4-6):
 *   Byte 0: D0=Brightness, D1=Contrast, D2=Hue, D3=Saturation,
 *           D4=Sharpness, D5=Gamma, D6=WB Temp, D7=WB Component
 *   Byte 1: D0=Backlight, D1=Gain, D2=PowerLine, D3=HueAuto, ...
 *   Byte 2: D0=AnalogVideoStd, D1=AnalogLock, D2=ContrastAuto
 *
 * Enable: Brightness(D0), Contrast(D1), Hue(D2), Saturation(D3)
 */
#define PU_CTRL_BYTE0  0x0F
#define PU_CTRL_BYTE1  0x00
#define PU_CTRL_BYTE2  0x00

/* ---------- Extension Unit: ISP Color Profile ---------------------------- */
/*
 * Custom GUID for ESP32-P4 ISP Profile Extension Unit.
 * Generated as a unique identifier for this vendor-specific control.
 */
#define XU_ISP_PROFILE_GUID \
    0x01, 0x00, 0x3A, 0x30, \
    0x49, 0x53, 0x50, 0x50, \
    0x80, 0x00, 0x00, 0xAA, \
    0x00, 0x38, 0x9B, 0x71

/*
 * Extension Unit descriptor (UVC 1.5 Table 4-15)
 * bLength            = 26
 * bDescriptorType    = 0x24 (CS_INTERFACE)
 * bDescriptorSubtype = 0x06 (VC_EXTENSION_UNIT)
 * bUnitID            = 0x04
 * guidExtensionCode  = XU_ISP_PROFILE_GUID (16 bytes)
 * bNumControls       = 1
 * bNrInPins          = 1
 * baSourceID[0]      = 0x02 (Processing Unit)
 * bControlSize       = 1
 * bmControls         = 0x01 (bit 0 = ISP Profile Select)
 * iExtension         = 0
 */
#define TUD_VIDEO_DESC_EXTENSION_UNIT_ISP_LEN 26

#define TUD_VIDEO_DESC_EXTENSION_UNIT_ISP \
    TUD_VIDEO_DESC_EXTENSION_UNIT_ISP_LEN, \
    TUSB_DESC_CS_INTERFACE, 0x06, UVC_ENTITY_ISP_XU, \
    XU_ISP_PROFILE_GUID, \
    1, 1, UVC_ENTITY_PROCESSING_UNIT, 1, 0x01, 0x00

/* ---------- Custom VS Input Header for 3 formats ------------------------ */
/*
 * TinyUSB's TUD_VIDEO_DESC_CS_VS_INPUT only works for bNumFormats=1
 * (it emits bControlSize bytes total instead of bNumFormats*bControlSize).
 * We emit raw bytes for the 3-format case.
 *
 * bLength = 13 + (bNumFormats * bControlSize) = 13 + 3*1 = 16
 */
#define VS_INPUT_HDR_LEN  (13 + UVC_NUM_FORMATS * 1)

#define TUD_VIDEO_DESC_CS_VS_INPUT_3FMT(_totallen, _epin, _termlnk) \
    VS_INPUT_HDR_LEN, \
    TUSB_DESC_CS_INTERFACE, \
    0x01, /* VIDEO_CS_ITF_VS_INPUT_HEADER */ \
    UVC_NUM_FORMATS, /* bNumFormats = 3 */ \
    U16_TO_U8S_LE((_totallen) + VS_INPUT_HDR_LEN), /* wTotalLength */ \
    _epin, \
    0x00, /* bmInfo */ \
    _termlnk, \
    0x00, /* bStillCaptureMethod */ \
    0x00, /* bTriggerSupport */ \
    0x00, /* bTriggerUsage */ \
    0x01, /* bControlSize = 1 byte per format */ \
    0x00, /* bmaControls(1) - UYVY */ \
    0x00, /* bmaControls(2) - MJPEG */ \
    0x00  /* bmaControls(3) - H.264 */

/* ---------- total lengths for descriptor computation --------------------- */

/* Video Control inner length (units+terminals after CS_VC header) */
#define VC_TOTAL_INNER_LEN ( \
    TUD_VIDEO_DESC_CAMERA_TERM_LEN + \
    TUD_VIDEO_DESC_PROCESSING_UNIT_LEN + \
    TUD_VIDEO_DESC_EXTENSION_UNIT_ISP_LEN + \
    TUD_VIDEO_DESC_OUTPUT_TERM_LEN \
)

/* Video Streaming inner length (formats+frames+color after VS Input Header) */
#define VS_TOTAL_INNER_LEN ( \
    /* Format 1: UYVY */ \
    TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR_LEN + \
    (UYVY_FRAME_COUNT * TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_CONT_LEN) + \
    /* Format 2: MJPEG */ \
    TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN + \
    (MJPEG_FRAME_COUNT * TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN) + \
    /* Format 3: H.264 frame-based */ \
    TUD_VIDEO_DESC_CS_VS_FMT_FRAME_BASED_LEN + \
    (H264_FRAME_COUNT * TUD_VIDEO_DESC_CS_VS_FRM_FRAME_BASED_CONT_LEN) + \
    /* Color matching */ \
    TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN \
)

/* Full UVC function descriptor length (IAD through endpoint) */
#define UVC_DESC_TOTAL_LEN ( \
    TUD_VIDEO_DESC_IAD_LEN + \
    /* VC interface */ \
    TUD_VIDEO_DESC_STD_VC_LEN + \
    (TUD_VIDEO_DESC_CS_VC_LEN + 1) + \
    VC_TOTAL_INNER_LEN + \
    /* VS interface (bulk: single alt with endpoint) */ \
    TUD_VIDEO_DESC_STD_VS_LEN + \
    VS_INPUT_HDR_LEN + \
    VS_TOTAL_INNER_LEN + \
    7 /* Bulk Endpoint */ \
)

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + UVC_DESC_TOTAL_LEN)

/* UYVY format GUID: FourCC 'UYVY' + standard MS suffix */
#define TUD_VIDEO_GUID_UYVY \
    0x55,0x59,0x56,0x59,0x00,0x00,0x10,0x00,0x80,0x00,0x00,0xAA,0x00,0x38,0x9B,0x71

/* UYVY format helper (like TinyUSB's YUY2 helper but with UYVY GUID) */
#define TUD_VIDEO_DESC_CS_VS_FMT_UYVY(_fmtidx, _numfmtdesc, _frmidx, _asrx, _asry, _interlace, _cp) \
  TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR(_fmtidx, _numfmtdesc, TUD_VIDEO_GUID_UYVY, 16, _frmidx, _asrx, _asry, _interlace, _cp)

/*
 * Multi-format UVC descriptor (Bulk transfer, UVC 1.5).
 *
 * _stridx: string descriptor index
 * _itf:    starting interface number (VC interface)
 * _epin:   bulk endpoint IN address
 * _epsize: max endpoint packet size (512 for HS bulk)
 */
#define TUD_VIDEO_CAPTURE_DESCRIPTOR_MULTIFORMAT_BULK(_stridx, _itf, _epin, _epsize) \
    /* ---- IAD ---- */ \
    TUD_VIDEO_DESC_IAD(_itf, 0x02, _stridx), \
    \
    /* ==== Video Control Interface ==== */ \
    TUD_VIDEO_DESC_STD_VC(_itf, 0, _stridx), \
    TUD_VIDEO_DESC_CS_VC( \
        0x0150, /* UVC 1.5 */ \
        VC_TOTAL_INNER_LEN, \
        UVC_CLOCK_FREQUENCY, \
        _itf + 1 \
    ), \
    TUD_VIDEO_DESC_CAMERA_TERM( \
        UVC_ENTITY_CAP_INPUT_TERMINAL, 0, 0, \
        0, 0, 0, 0x00 \
    ), \
    TUD_VIDEO_DESC_PROCESSING_UNIT( \
        UVC_ENTITY_PROCESSING_UNIT, \
        UVC_ENTITY_CAP_INPUT_TERMINAL, \
        PU_CTRL_BYTE0, PU_CTRL_BYTE1, PU_CTRL_BYTE2 \
    ), \
    TUD_VIDEO_DESC_EXTENSION_UNIT_ISP, \
    TUD_VIDEO_DESC_OUTPUT_TERM( \
        UVC_ENTITY_CAP_OUTPUT_TERMINAL, \
        VIDEO_TT_STREAMING, 0, \
        UVC_ENTITY_ISP_XU, 0 \
    ), \
    \
    /* ==== Video Streaming Interface (Bulk, single alt) ==== */ \
    TUD_VIDEO_DESC_STD_VS(_itf + 1, 0, 1, _stridx), \
    \
    /* VS Input Header: 3 formats */ \
    TUD_VIDEO_DESC_CS_VS_INPUT_3FMT( \
        VS_TOTAL_INNER_LEN, \
        _epin, \
        UVC_ENTITY_CAP_OUTPUT_TERMINAL \
    ), \
    \
    /* ---- Format 1: UYVY (Uncompressed) ---- */ \
    TUD_VIDEO_DESC_CS_VS_FMT_UYVY( \
        1, UYVY_FRAME_COUNT, 1, 0, 0, 0, 0 \
    ), \
    /* Frame 1: 800x800 @15fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_CONT( \
        1, 0, 800, 800, \
        800*800*2, 800*800*2*15, 800*800*2, \
        FI(15), FI(15), FI(15), FI(15) \
    ), \
    /* Frame 2: 640x480 @30fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_CONT( \
        2, 0, 640, 480, \
        640*480*2, 640*480*2*30, 640*480*2, \
        FI(30), FI(30), FI(30), FI(30) \
    ), \
    /* Frame 3: 320x240 @50fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_CONT( \
        3, 0, 320, 240, \
        320*240*2, 320*240*2*50, 320*240*2, \
        FI(50), FI(50), FI(50), FI(50) \
    ), \
    \
    /* ---- Format 2: MJPEG ---- */ \
    TUD_VIDEO_DESC_CS_VS_FMT_MJPEG( \
        2, MJPEG_FRAME_COUNT, 0, 1, 0, 0, 0, 0 \
    ), \
    /* Frame 1: 800x800 @50fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT( \
        1, 0, 800, 800, \
        800*800*16, 800*800*16*50, 800*800*16/8, \
        FI(50), FI(50), FI(50), FI(50) \
    ), \
    /* Frame 2: 640x480 @50fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT( \
        2, 0, 640, 480, \
        640*480*16, 640*480*16*50, 640*480*16/8, \
        FI(50), FI(50), FI(50), FI(50) \
    ), \
    /* Frame 3: 320x240 @50fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT( \
        3, 0, 320, 240, \
        320*240*16, 320*240*16*50, 320*240*16/8, \
        FI(50), FI(50), FI(50), FI(50) \
    ), \
    \
    /* ---- Format 3: H.264 (Frame-Based) ---- */ \
    TUD_VIDEO_DESC_CS_VS_FMT_FRAME_BASED( \
        3, H264_FRAME_COUNT, \
        TUD_VIDEO_GUID_H264, 16, 1, 0, 0, 0, 0, 1 \
    ), \
    /* Frame 1: 800x800 @50fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_FRAME_BASED_CONT( \
        1, 0, 800, 800, \
        800*800*16, 800*800*16*50, \
        FI(50), 0, FI(50), FI(50), FI(50) \
    ), \
    /* Frame 2: 640x480 @50fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_FRAME_BASED_CONT( \
        2, 0, 640, 480, \
        640*480*16, 640*480*16*50, \
        FI(50), 0, FI(50), FI(50), FI(50) \
    ), \
    /* Frame 3: 320x240 @50fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_FRAME_BASED_CONT( \
        3, 0, 320, 240, \
        320*240*16, 320*240*16*50, \
        FI(50), 0, FI(50), FI(50), FI(50) \
    ), \
    \
    /* Color Matching */ \
    TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING( \
        VIDEO_COLOR_PRIMARIES_BT709, \
        VIDEO_COLOR_XFER_CH_BT709, \
        VIDEO_COLOR_COEF_SMPTE170M \
    ), \
    \
    /* Bulk Endpoint (wMaxPacketSize=512 for HS, regardless of EP_BUFSIZE) */ \
    TUD_VIDEO_DESC_EP_BULK(_epin, 512, 1)

#endif /* _USB_DESCRIPTORS_H_ */

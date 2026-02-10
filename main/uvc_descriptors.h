/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Custom multi-format UVC 1.5 descriptor for ESP32-P4 webcam.
 * Advertises 3 formats simultaneously: YUY2, MJPEG, H.264
 *
 * Descriptor hierarchy:
 *   VideoControl Interface
 *     ├── Camera Terminal (IT) - exposure controls
 *     ├── Processing Unit (PU) - image adjustments
 *     └── Output Terminal (OT)
 *   VideoStreaming Interface
 *     ├── VS Input Header (bNumFormats=3)
 *     ├── Format 1: YUY2 (Uncompressed)
 *     │     ├── 640x480 @30fps
 *     │     └── 800x640 @15fps
 *     ├── Format 2: MJPEG
 *     │     ├── 640x480 @30fps
 *     │     ├── 800x640 @50fps
 *     │     ├── 800x800 @50fps
 *     │     ├── 1280x960 @45fps
 *     │     └── 1920x1080 @30fps
 *     ├── Format 3: H.264 (Frame-Based)
 *     │     ├── 640x480 @30fps
 *     │     ├── 800x640 @50fps
 *     │     ├── 1280x960 @45fps
 *     │     └── 1920x1080 @30fps
 *     └── Color Matching
 */

#pragma once

#include "tusb.h"
#include "usb_device_uvc.h"

/* ---------- entity IDs --------------------------------------------------- */
#define UVC_ENTITY_CAP_INPUT_TERMINAL   0x01
#define UVC_ENTITY_PROCESSING_UNIT      0x02
#define UVC_ENTITY_CAP_OUTPUT_TERMINAL  0x03

/* ---------- clock -------------------------------------------------------- */
#define UVC_CLOCK_FREQUENCY  27000000

/* ---------- frame counts per format -------------------------------------- */
#define YUY2_FRAME_COUNT     2   /* 640x480, 800x640 */
#define MJPEG_FRAME_COUNT    5   /* 640x480, 800x640, 800x800, 1280x960, 1920x1080 */
#define H264_FRAME_COUNT     4   /* 640x480, 800x640, 1280x960, 1920x1080 */

/* ---------- helper: frame interval in 100ns units ------------------------ */
#define FI(fps)  (10000000 / (fps))

/* ---------- Processing Unit descriptor length (no auto controls) --------- */
/*  bLength=13, bDescriptorType=0x24, bDescriptorSubtype=0x05,
 *  bUnitID, bSourceID, wMaxMultiplier(2), bControlSize(3),
 *  bmControls(3), iProcessing, bmVideoStandards */
#define TUD_VIDEO_DESC_PROCESSING_UNIT_LEN  13

#define TUD_VIDEO_DESC_PROCESSING_UNIT(_unitid, _srcid, _bmcontrols0, _bmcontrols1, _bmcontrols2) \
    TUD_VIDEO_DESC_PROCESSING_UNIT_LEN, \
    TUSB_DESC_CS_INTERFACE, VIDEO_CS_ITF_VC_PROCESSING_UNIT, \
    _unitid, _srcid, \
    U16_TO_U8S_LE(0x0000), /* wMaxMultiplier */ \
    0x03,                   /* bControlSize */ \
    _bmcontrols0, _bmcontrols1, _bmcontrols2, \
    0x00,                   /* iProcessing */ \
    0x00                    /* bmVideoStandards */

/*
 * Processing Unit bmControls bitmap (UVC 1.5 spec Table 4-6):
 *   Byte 0: D0=Brightness, D1=Contrast, D2=Hue, D3=Saturation,
 *           D4=Sharpness, D5=Gamma, D6=WB Temp, D7=WB Component
 *   Byte 1: D0=Backlight, D1=Gain, D2=PowerLine, D3=HueAuto,
 *           D4=WBAuto, D5=WBCompAuto, D6=DigitalMultiplier,
 *           D7=DigitalMultiplierLimit
 *   Byte 2: D0=AnalogVideoStandard, D1=AnalogVideoLockStatus,
 *           D2=ContrastAuto
 *
 * We enable: Brightness, Contrast, Saturation, Sharpness, Gain
 */
#define PU_CTRL_BYTE0  (0x01 | 0x02 | 0x08 | 0x10)  /* brightness, contrast, saturation, sharpness */
#define PU_CTRL_BYTE1  (0x02)                         /* gain */
#define PU_CTRL_BYTE2  (0x00)

/* ---------- total lengths for descriptor computation --------------------- */

/* Video Control total (inside CS_VC header's wTotalLength) */
#define VC_TOTAL_INNER_LEN ( \
    TUD_VIDEO_DESC_CAMERA_TERM_LEN + \
    TUD_VIDEO_DESC_PROCESSING_UNIT_LEN + \
    TUD_VIDEO_DESC_OUTPUT_TERM_LEN \
)

/* Video Streaming total (inside CS_VS_INPUT header's wTotalLength) */
#define VS_TOTAL_INNER_LEN ( \
    /* Format 1: YUY2 */ \
    TUD_VIDEO_DESC_CS_VS_FMT_UNCOMPR_LEN + \
    (YUY2_FRAME_COUNT * TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_CONT_LEN) + \
    /* Format 2: MJPEG */ \
    TUD_VIDEO_DESC_CS_VS_FMT_MJPEG_LEN + \
    (MJPEG_FRAME_COUNT * TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT_LEN) + \
    /* Format 3: H.264 frame-based */ \
    TUD_VIDEO_DESC_CS_VS_FMT_FRAME_BASED_LEN + \
    (H264_FRAME_COUNT * TUD_VIDEO_DESC_CS_VS_FRM_FRAME_BASED_CONT_LEN) + \
    /* Color matching (shared) */ \
    TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING_LEN \
)

/* Full configuration descriptor length */
#define UVC_DESC_TOTAL_LEN ( \
    TUD_VIDEO_DESC_IAD_LEN + \
    /* VC interface */ \
    TUD_VIDEO_DESC_STD_VC_LEN + \
    (TUD_VIDEO_DESC_CS_VC_LEN + 1) + /* +1 for bInCollection */ \
    VC_TOTAL_INNER_LEN + \
    /* VS interface alt 0 (no endpoint) */ \
    TUD_VIDEO_DESC_STD_VS_LEN + \
    (TUD_VIDEO_DESC_CS_VS_IN_LEN + 3) + /* +3 for bmaControls x bNumFormats */ \
    VS_TOTAL_INNER_LEN + \
    /* VS interface alt 1 (with endpoint) */ \
    TUD_VIDEO_DESC_STD_VS_LEN + \
    7 /* Endpoint descriptor */ \
)

/*
 * Complete multi-format UVC descriptor macro.
 *
 * This is the heart of the UVC 1.5 device - it tells the host OS
 * what formats and resolutions are available. The host negotiates
 * format/frame/fps during the VS probe/commit exchange.
 *
 * _stridx: string descriptor index
 * _itf:    starting interface number
 * _epin:   endpoint IN address
 * _epsize: max endpoint packet size (512 for HS bulk)
 */
#define TUD_VIDEO_CAPTURE_DESCRIPTOR_MULTIFORMAT(_stridx, _itf, _epin, _epsize) \
    /* ---- IAD: group VC + VS interfaces ---- */ \
    TUD_VIDEO_DESC_IAD(_itf, 0x02, _stridx), \
    \
    /* ==== Video Control Interface ==== */ \
    TUD_VIDEO_DESC_STD_VC(_itf, 0, _stridx), \
    TUD_VIDEO_DESC_CS_VC( \
        0x0150, /* UVC 1.5 */ \
        VC_TOTAL_INNER_LEN, \
        UVC_CLOCK_FREQUENCY, \
        _itf + 1 /* bInCollection: VS interface */ \
    ), \
    /* Camera Terminal (input) */ \
    TUD_VIDEO_DESC_CAMERA_TERM( \
        UVC_ENTITY_CAP_INPUT_TERMINAL, 0, 0, \
        0, 0, 0, /* focal length min/max/current */ \
        0x00 /* bmControls - TODO: add exposure */ \
    ), \
    /* Processing Unit */ \
    TUD_VIDEO_DESC_PROCESSING_UNIT( \
        UVC_ENTITY_PROCESSING_UNIT, \
        UVC_ENTITY_CAP_INPUT_TERMINAL, \
        PU_CTRL_BYTE0, PU_CTRL_BYTE1, PU_CTRL_BYTE2 \
    ), \
    /* Output Terminal */ \
    TUD_VIDEO_DESC_OUTPUT_TERM( \
        UVC_ENTITY_CAP_OUTPUT_TERMINAL, \
        VIDEO_TT_STREAMING, 0, \
        UVC_ENTITY_PROCESSING_UNIT, 0 \
    ), \
    \
    /* ==== Video Streaming Interface (alt 0, no EP) ==== */ \
    TUD_VIDEO_DESC_STD_VS(_itf + 1, 0, 0, _stridx), \
    \
    /* VS Input Header: 3 formats */ \
    TUD_VIDEO_DESC_CS_VS_INPUT( \
        3, /* bNumFormats */ \
        VS_TOTAL_INNER_LEN, \
        _epin, \
        0, /* bmInfo */ \
        UVC_ENTITY_CAP_OUTPUT_TERMINAL, \
        0, 0, 0, /* still capture, trigger */ \
        0, 0, 0  /* bmaControls for each format */ \
    ), \
    \
    /* ---- Format 1: YUY2 (Uncompressed) ---- */ \
    TUD_VIDEO_DESC_CS_VS_FMT_YUY2( \
        1, /* bFormatIndex */ \
        YUY2_FRAME_COUNT, \
        1, /* bDefaultFrameIndex */ \
        0, 0, 0, 0 /* aspect ratio, interlace, copy protect */ \
    ), \
    /* YUY2 Frame 1: 640x480 @30fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_CONT( \
        1, 0, 640, 480, \
        640*480*2, 640*480*2*30, \
        640*480*2, \
        FI(30), FI(30), FI(30)*30, FI(30) \
    ), \
    /* YUY2 Frame 2: 800x640 @15fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_UNCOMPR_CONT( \
        2, 0, 800, 640, \
        800*640*2, 800*640*2*15, \
        800*640*2, \
        FI(15), FI(15), FI(15)*15, FI(15) \
    ), \
    \
    /* ---- Format 2: MJPEG ---- */ \
    TUD_VIDEO_DESC_CS_VS_FMT_MJPEG( \
        2, /* bFormatIndex */ \
        MJPEG_FRAME_COUNT, \
        0, /* bmFlags */ \
        1, /* bDefaultFrameIndex */ \
        0, 0, 0, 0 /* aspect, interlace, copy */ \
    ), \
    /* MJPEG Frame 1: 640x480 @30fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT( \
        1, 0, 640, 480, \
        640*480*16, 640*480*16*30, \
        640*480*16/8, \
        FI(30), FI(30), FI(30), FI(30) \
    ), \
    /* MJPEG Frame 2: 800x640 @50fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT( \
        2, 0, 800, 640, \
        800*640*16, 800*640*16*50, \
        800*640*16/8, \
        FI(50), FI(50), FI(50), FI(50) \
    ), \
    /* MJPEG Frame 3: 800x800 @50fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT( \
        3, 0, 800, 800, \
        800*800*16, 800*800*16*50, \
        800*800*16/8, \
        FI(50), FI(50), FI(50), FI(50) \
    ), \
    /* MJPEG Frame 4: 1280x960 @45fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT( \
        4, 0, 1280, 960, \
        1280*960*16, 1280*960*16*45, \
        1280*960*16/8, \
        FI(45), FI(45), FI(45), FI(45) \
    ), \
    /* MJPEG Frame 5: 1920x1080 @30fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_MJPEG_CONT( \
        5, 0, 1920, 1080, \
        1920*1080*16, 1920*1080*16*30, \
        1920*1080*16/8, \
        FI(30), FI(30), FI(30), FI(30) \
    ), \
    \
    /* ---- Format 3: H.264 (Frame-Based) ---- */ \
    TUD_VIDEO_DESC_CS_VS_FMT_FRAME_BASED( \
        3, /* bFormatIndex */ \
        H264_FRAME_COUNT, \
        TUD_VIDEO_GUID_H264, \
        16, /* bBitsPerPixel */ \
        1,  /* bDefaultFrameIndex */ \
        0, 0, 0, 0, /* aspect, interlace, copy */ \
        1  /* bVariableSize = 1 (H.264 is variable) */ \
    ), \
    /* H.264 Frame 1: 640x480 @30fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_FRAME_BASED_CONT( \
        1, 0, 640, 480, \
        640*480*16, 640*480*16*30, \
        FI(30), 0, \
        FI(30), FI(30), FI(30) \
    ), \
    /* H.264 Frame 2: 800x640 @50fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_FRAME_BASED_CONT( \
        2, 0, 800, 640, \
        800*640*16, 800*640*16*50, \
        FI(50), 0, \
        FI(50), FI(50), FI(50) \
    ), \
    /* H.264 Frame 3: 1280x960 @45fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_FRAME_BASED_CONT( \
        3, 0, 1280, 960, \
        1280*960*16, 1280*960*16*45, \
        FI(45), 0, \
        FI(45), FI(45), FI(45) \
    ), \
    /* H.264 Frame 4: 1920x1080 @30fps */ \
    TUD_VIDEO_DESC_CS_VS_FRM_FRAME_BASED_CONT( \
        4, 0, 1920, 1080, \
        1920*1080*16, 1920*1080*16*30, \
        FI(30), 0, \
        FI(30), FI(30), FI(30) \
    ), \
    \
    /* Color Matching (applies to all formats) */ \
    TUD_VIDEO_DESC_CS_VS_COLOR_MATCHING( \
        VIDEO_COLOR_PRIMARIES_BT709, \
        VIDEO_COLOR_XFER_CH_BT709, \
        VIDEO_COLOR_COEF_SMPTE170M \
    ), \
    \
    /* ==== VS Interface alt 1 (with ISO endpoint) ==== */ \
    TUD_VIDEO_DESC_STD_VS(_itf + 1, 1, 1, _stridx), \
    TUD_VIDEO_DESC_EP_ISO(_epin, _epsize, 1)

/* ---------- Resolution lookup table -------------------------------------- */

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  max_fps;
} uvc_frame_info_t;

/* Frame tables indexed by bFrameIndex-1 for each format */
static const uvc_frame_info_t uvc_yuy2_frames[] = {
    { 640,  480,  30 },
    { 800,  640,  15 },
};

static const uvc_frame_info_t uvc_mjpeg_frames[] = {
    { 640,  480,  30 },
    { 800,  640,  50 },
    { 800,  800,  50 },
    { 1280, 960,  45 },
    { 1920, 1080, 30 },
};

static const uvc_frame_info_t uvc_h264_frames[] = {
    { 640,  480,  30 },
    { 800,  640,  50 },
    { 1280, 960,  45 },
    { 1920, 1080, 30 },
};

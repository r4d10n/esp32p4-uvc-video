#!/bin/bash
# Apply TinyUSB patches to managed component.
# Run after: idf.py build (which downloads managed_components)
# These patches are reset by: idf.py fullclean
#
# Part A — UVC entity control callback (video_device.c/h)
#   Adds tud_video_entity_control_xfer_cb() so PU controls (brightness,
#   contrast, saturation, hue) are forwarded to the application instead
#   of silently returning VIDEO_ERROR_NONE (which causes 5-second host
#   timeouts per control).
#
# Part B — DWC2 performance optimizations
#   1. INCR16 AHB burst (dwc2_common.c)  - 4x wider DMA bursts
#   2. DTHRCTL thresholding (dcd_dwc2.c)  - +15% TX throughput
#   3. LEVEL3 ISR priority (dwc2_esp32.h) - minimal ISR latency

set -e
cd "$(dirname "$0")"

VIDEO_DIR="managed_components/espressif__tinyusb/src/class/video"
DWC2_DIR="managed_components/espressif__tinyusb/src/portable/synopsys/dwc2"

if [ ! -d "$VIDEO_DIR" ] || [ ! -d "$DWC2_DIR" ]; then
    echo "ERROR: managed_components not found. Run 'idf.py build' first."
    exit 1
fi

# ── Part A: UVC entity control callback ──────────────────────────────

echo "Applying UVC entity control callback patches..."

# A1. video_device.h — add callback declaration after tud_video_commit_cb
if ! grep -q 'tud_video_entity_control_xfer_cb' "$VIDEO_DIR/video_device.h"; then
    sed -i '/^int tud_video_commit_cb(/,/;/{
        /;/a\
\
/** Invoked when a class-specific control request targets a VC entity\
 *  (Processing Unit, Camera Terminal, etc.).\
 *\
 * @param[in] rhport       Root hub port\
 * @param[in] stage        Control transfer stage (SETUP/DATA/ACK)\
 * @param[in] request      USB setup packet (entity_id in wIndex high byte,\
 *                          control selector in wValue high byte)\
 * @param[in] ctl_idx      Video control interface index\
 * @return video_error_code_t */\
int tud_video_entity_control_xfer_cb(uint8_t rhport, uint8_t stage,\
                                      tusb_control_request_t const *request,\
                                      uint_fast8_t ctl_idx);
    }' "$VIDEO_DIR/video_device.h"
    echo "  [A1] Callback declaration added to video_device.h"
else
    echo "  [A1] Callback already declared in video_device.h (skipped)"
fi

# A2. video_device.c — add weak default after tud_video_commit_cb weak stub
if ! grep -q 'tud_video_entity_control_xfer_cb' "$VIDEO_DIR/video_device.c"; then
    # Add weak stub after the closing brace of tud_video_commit_cb
    sed -i '/^TU_ATTR_WEAK int tud_video_commit_cb/,/^}$/{
        /^}$/a\
\
TU_ATTR_WEAK int tud_video_entity_control_xfer_cb(uint8_t rhport, uint8_t stage,\
                                                    tusb_control_request_t const *request,\
                                                    uint_fast8_t ctl_idx) {\
  (void) rhport;\
  (void) stage;\
  (void) request;\
  (void) ctl_idx;\
  return VIDEO_ERROR_INVALID_REQUEST;\
}
    }' "$VIDEO_DIR/video_device.c"

    # Replace entity stub: VIDEO_ERROR_NONE -> callback invocation
    # Match the line AFTER _find_desc_entity(...entity_id) to target the right return
    sed -i '/_find_desc_entity.*entity_id/{
        n
        s/return VIDEO_ERROR_NONE;/return tud_video_entity_control_xfer_cb(rhport, stage, request, ctl_idx);/
    }' "$VIDEO_DIR/video_device.c"

    echo "  [A2] Weak stub + entity dispatch patched in video_device.c"
else
    echo "  [A2] Entity callback already present in video_device.c (skipped)"
fi

# ── Part B: DWC2 performance optimizations ───────────────────────────

echo "Applying DWC2 performance patches..."

# B1. INCR16 burst: HBSTLEN_2 (INCR4) -> HBSTLEN_4 (INCR16)
sed -i 's/GAHBCFG_DMAEN | GAHBCFG_HBSTLEN_2/GAHBCFG_DMAEN | GAHBCFG_HBSTLEN_4/' \
    "$DWC2_DIR/dwc2_common.c"
echo "  [B1] INCR16 AHB burst applied to dwc2_common.c"

# B2. DTHRCTL TX thresholding (add after gahbcfg setup in dcd_init)
if ! grep -q 'dthrctl' "$DWC2_DIR/dcd_dwc2.c"; then
    sed -i '/dwc2->gahbcfg = gahbcfg;/a\
\
  // DMA threshold control for HS DMA mode.\
  // NONISOTHREN + TXTHRLEN=8 yields ~15% SOURCE throughput improvement.\
  // Threshold must be <= smallest TX FIFO (EP0 = 16 words) for enumeration.\
  if (is_highspeed \&\& is_dma) {\
    dwc2->dthrctl = DTHRCTL_NONISOTHREN |\
                    (8u << DTHRCTL_TXTHRLEN_Pos);\
  }' "$DWC2_DIR/dcd_dwc2.c"
    echo "  [B2] DTHRCTL thresholding applied to dcd_dwc2.c"
else
    echo "  [B2] DTHRCTL already present in dcd_dwc2.c (skipped)"
fi

# B3. LEVEL3 ISR priority
sed -i 's/ESP_INTR_FLAG_LOWMED/ESP_INTR_FLAG_LEVEL3/' \
    "$DWC2_DIR/dwc2_esp32.h"
echo "  [B3] LEVEL3 ISR priority applied to dwc2_esp32.h"

echo "Done. All TinyUSB patches applied."

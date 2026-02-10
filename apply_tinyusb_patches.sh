#!/bin/bash
# Apply DWC2 performance patches to managed TinyUSB component.
# Run after: idf.py build (which downloads managed_components)
# These patches are reset by: idf.py fullclean
#
# Optimizations (validated at 38 MB/s in usb-hs-bulk-speed-test):
#   1. INCR16 AHB burst (dwc2_common.c)  - 4x wider DMA bursts
#   2. DTHRCTL thresholding (dcd_dwc2.c)  - +15% TX throughput
#   3. LEVEL3 ISR priority (dwc2_esp32.h) - minimal ISR latency

set -e
TUSB_DIR="managed_components/espressif__tinyusb/src/portable/synopsys/dwc2"

if [ ! -d "$TUSB_DIR" ]; then
    echo "ERROR: $TUSB_DIR not found. Run 'idf.py build' first to download components."
    exit 1
fi

echo "Applying DWC2 performance patches..."

# 1. INCR16 burst: HBSTLEN_2 (INCR4) -> HBSTLEN_4 (INCR16)
sed -i 's/GAHBCFG_DMAEN | GAHBCFG_HBSTLEN_2/GAHBCFG_DMAEN | GAHBCFG_HBSTLEN_4/' \
    "$TUSB_DIR/dwc2_common.c"
echo "  [1/3] INCR16 AHB burst applied to dwc2_common.c"

# 2. DTHRCTL TX thresholding (add after gahbcfg setup in dcd_init)
if ! grep -q 'dthrctl' "$TUSB_DIR/dcd_dwc2.c"; then
    sed -i '/dwc2->gahbcfg = gahbcfg;/a\
\
  // DMA threshold control for HS DMA mode.\
  // NONISOTHREN + TXTHRLEN=8 yields ~15% SOURCE throughput improvement.\
  // Threshold must be <= smallest TX FIFO (EP0 = 16 words) for enumeration.\
  if (is_highspeed \&\& is_dma) {\
    dwc2->dthrctl = DTHRCTL_NONISOTHREN |\
                    (8u << DTHRCTL_TXTHRLEN_Pos);\
  }' "$TUSB_DIR/dcd_dwc2.c"
    echo "  [2/3] DTHRCTL thresholding applied to dcd_dwc2.c"
else
    echo "  [2/3] DTHRCTL already present in dcd_dwc2.c (skipped)"
fi

# 3. LEVEL3 ISR priority
sed -i 's/ESP_INTR_FLAG_LOWMED/ESP_INTR_FLAG_LEVEL3/' \
    "$TUSB_DIR/dwc2_esp32.h"
echo "  [3/3] LEVEL3 ISR priority applied to dwc2_esp32.h"

echo "Done. All DWC2 patches applied."

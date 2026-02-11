# ESP32-P4 UVC Webcam — CPU, RAM & DMA Usage Analysis

## Per-Frame Pipeline Breakdown (MJPEG @ 800x800, 50fps)

| Stage | Hardware | CPU Work | Time/Frame |
|---|---|---|---|
| **MIPI CSI capture** | DMA (CSI controller) | V4L2 DQBUF/QBUF ioctl ~20us | 0us CPU data movement |
| **ISP processing** | DMA (ISP pipeline) | None — runs inline with CSI | 0us |
| **JPEG encode** | DMA (HW JPEG M2M) | V4L2 QBUF/DQBUF + cache sync ~40us | 0us encoding work |
| **H.264 encode** | DMA (HW H.264 M2M) | V4L2 QBUF/DQBUF + cache sync ~40us | 0us encoding work |
| **USB bulk transfer** | DMA (DWC2 controller) | TinyUSB ISR + descriptor mgmt | ~50us per frame |
| **Center-crop** (if <800x800) | **CPU memcpy** | Row-by-row copy from PSRAM | ~200us (640x480), ~60us (320x240) |
| **Cache sync** | CPU | `esp_cache_msync()` after encoder | ~20us (64-byte aligned) |

## Estimated CPU Usage

### Core 0 (Video pipeline) — priority 23

- At 800x800 native (no crop): **~1-2%** — just V4L2 ioctls and cache sync
- At 640x480 (with crop): **~3-5%** — adds ~200us memcpy per frame at 50fps
- At 320x240 (with crop): **~2-3%** — crop is smaller

### Core 1 (USB/TinyUSB) — priority 24

- **~0.5-1%** — DWC2 handles bulk transfers via DMA; CPU just manages descriptors in ISR

## RAM Usage Estimate

| Category | Internal SRAM | PSRAM |
|---|---|---|
| FreeRTOS + IDF overhead | ~100KB | — |
| Task stacks (video, USB, perf_mon, idle x 2) | ~40KB | — |
| V4L2 camera buffers (4 x 800x800x2) | — | ~5.12MB |
| JPEG encoder M2M buffers | — | ~2.56MB |
| H.264 encoder M2M buffers | — | ~2.56MB |
| UVC frame buffer (800x800x2) | — | ~1.28MB |
| Crop staging buffer (when active) | — | ~0.6MB max |
| TinyUSB + DWC2 DMA buffers | ~16KB | — |
| **Total estimate** | **~160KB / 512KB** | **~12MB / 32MB** |

## DMA vs Active CPU

### 100% DMA (zero CPU cycles for data movement)

- MIPI CSI -> ISP -> frame buffer (sensor to PSRAM)
- JPEG/H.264 encoding (PSRAM -> encoder -> PSRAM)
- USB bulk transfer (PSRAM -> host)

### CPU-bound operations

- V4L2 ioctl overhead (~120us/frame total for all QBUF/DQBUF calls)
- `esp_cache_msync()` after encoder DQBUF (~20us, required because V4L2 M2M doesn't invalidate cache)
- Center-crop memcpy (only when streaming < 800x800)
- Perf monitor task (negligible — runs every 5s, priority 1)

### Summary

The ESP32-P4 hardware does >95% of the work via DMA. At native 800x800,
CPU usage should be under 3% total across both cores. The performance
monitor (`perf_monitor.c`) reports real numbers every 5 seconds over serial.

## Task Pinning

| Task | Core | Priority | Purpose |
|---|---|---|---|
| video_task | CPU0 | 23 | Camera capture + encode loop |
| tinyusb_task | CPU1 | 24 | USB device stack |
| perf_mon | any | 1 | Performance reporting |
| IDLE0 | CPU0 | 0 | Idle (used for CPU% calc) |
| IDLE1 | CPU1 | 0 | Idle (used for CPU% calc) |

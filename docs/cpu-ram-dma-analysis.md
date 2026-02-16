# ESP32-P4 UVC Webcam — CPU, RAM & DMA Usage Analysis

Analysis of the hardware resource utilization for the dual-output (USB UVC + RTSP/RTP) camera pipeline at 1920x1080 RAW10 30fps.

## Per-Frame Pipeline Breakdown (1920x1080, 30fps)

| Stage | Hardware | CPU Work | Time/Frame |
|---|---|---|---|
| **MIPI CSI capture** | DMA (CSI controller, 2-lane) | V4L2 DQBUF/QBUF ioctl ~20us | 0us CPU data movement |
| **ISP processing** (RAW10→UYVY) | DMA (ISP pipeline) | None — runs inline with CSI | 0us |
| **JPEG encode** | DMA (HW JPEG M2M) | V4L2 QBUF/DQBUF + cache sync ~40us | 0us encoding work |
| **H.264 encode** | DMA (HW H.264 M2M) | V4L2 QBUF/DQBUF + cache sync ~40us | 0us encoding work |
| **USB bulk transfer** | DMA (DWC2 controller) | TinyUSB ISR + descriptor mgmt | ~50us per frame |
| **RTP packetization** (RTSP) | CPU | NAL parsing + UDP sendto() | ~100-200us per frame |
| **Ethernet TX** | DMA (EMAC controller) | None — EMAC DMA sends packets | 0us data movement |
| **Center-crop** (if <1080p) | **CPU memcpy** | Row-by-row copy from PSRAM | ~500us (1280x720), ~150us (640x480) |
| **Cache sync** | CPU | `esp_cache_msync()` after encoder | ~20us (64-byte aligned) |

## Estimated CPU Usage

### USB UVC Streaming (Single Output)

#### Core 0 (Video pipeline) — priority 23

- At 1920x1080 native (no crop): **~1-2%** — just V4L2 ioctls and cache sync
- At 1280x720 (center-crop from 1080p): **~3-5%** — adds ~500us memcpy per frame at 30fps
- At 640x480 (center-crop from 1080p): **~2-3%** — crop is smaller

#### Core 1 (USB/TinyUSB) — priority 24

- **~0.5-1%** — DWC2 handles bulk transfers via DMA; CPU just manages descriptors in ISR

### RTSP Streaming (Single Output)

#### Core 0 (Self-capture loop) — priority 10

- H.264 1080p at 8Mbps, GOP=10: **~15-20%** — V4L2 capture + encode + RTP packetization
- Higher than USB because the RTSP task handles the entire pipeline (capture, encode, packetize, send) in one task rather than splitting across ISR-driven USB DMA

#### Ethernet overhead

- **<1%** — EMAC DMA handles packet transmission; CPU just calls `sendto()` which queues to LWIP

### Simultaneous USB + RTSP

Not concurrent in practice — the RTSP server yields the camera/encoder when USB starts streaming and resumes when USB stops. Only one pipeline drives the hardware at a time.

## RAM Usage Estimate

| Category | Internal SRAM | PSRAM |
|---|---|---|
| FreeRTOS + IDF overhead | ~100KB | — |
| Task stacks (video, USB, RTSP, RTP, perf_mon, idle x 2) | ~60KB | — |
| V4L2 camera buffers (4 x 1920x1080x2) | — | ~16.6MB |
| JPEG encoder M2M buffers | — | ~8.3MB |
| H.264 encoder M2M buffers | — | ~8.3MB |
| UVC frame buffer (1920x1080x2) | — | ~4.1MB |
| Crop staging buffer (when active) | — | ~1.8MB max (1280x720) |
| RTP packet buffer | — | ~64KB |
| LWIP + Ethernet DMA buffers | ~32KB | — |
| TinyUSB + DWC2 DMA buffers | ~16KB | — |
| **Total estimate** | **~210KB / 512KB** | **~27MB / 32MB** |

> **Note:** The V4L2 M2M encoder buffers are allocated by the esp_video framework and may share memory with the capture buffers internally. Actual PSRAM usage may be lower than the sum above. Use `perf_monitor.c` output (reported every 5s on serial) for real numbers.

## DMA vs Active CPU

### 100% DMA (zero CPU cycles for data movement)

- MIPI CSI → ISP → frame buffer (sensor to PSRAM, 2-lane MIPI at RAW10)
- ISP processing (RAW10 Bayer → UYVY conversion, color correction, white balance)
- JPEG/H.264 encoding (PSRAM → encoder → PSRAM, M2M V4L2 device)
- USB bulk transfer (PSRAM → host, DWC2 High-Speed controller)
- Ethernet TX (PSRAM/SRAM → wire, EMAC RMII with IP101GR PHY)

### CPU-bound operations

- V4L2 ioctl overhead (~120us/frame total for all QBUF/DQBUF calls)
- `esp_cache_msync()` after encoder DQBUF (~20us, required because V4L2 M2M doesn't invalidate cache)
- Center-crop memcpy (only when streaming below 1920x1080)
- RTP NAL unit parsing and FU-A fragmentation (~100-200us/frame for H.264 at 8Mbps)
- UDP `sendto()` calls (~50-100 calls/frame for a typical 1080p IDR, ~5-10 for P-frames)
- Perf monitor task (negligible — runs every 5s, priority 1)

### Summary

The ESP32-P4 hardware does >95% of the work via DMA. At native 1920x1080 over USB, CPU usage is under 3% total across both cores. RTSP self-capture is higher (~19%) because the capture/encode/packetize/send loop runs entirely in a FreeRTOS task rather than being ISR-driven. The performance monitor (`perf_monitor.c`) reports real numbers every 5 seconds over serial.

## Task Pinning

| Task | Core | Priority | Stack | Purpose |
|---|---|---|---|---|
| video_task | CPU0 | 23 | 4KB | Camera capture + encode loop (UVC) |
| tinyusb_task | CPU1 | 24 | 4KB | USB device stack |
| rtsp_server | any | 10 | 8KB | RTSP protocol handler (TCP) |
| rtp_sender | any | 10 | 8KB | Self-capture + RTP packetization (UDP) |
| perf_mon | any | 1 | 4KB | CPU/memory/streaming stats |
| IDLE0 | CPU0 | 0 | — | Idle (used for CPU% calc) |
| IDLE1 | CPU1 | 0 | — | Idle (used for CPU% calc) |

## Bandwidth Budget

| Interface | Max Bandwidth | Typical Usage | Headroom |
|---|---|---|---|
| USB HS bulk | ~40 MB/s (practical) | MJPEG 1080p: ~3-5 MB/s, H.264: ~0.15 MB/s | >85% |
| Ethernet 100Mbps | 12.5 MB/s | H.264 RTSP 1080p: ~1.5 MB/s (12Mbps) | 88% |
| PSRAM bus (200MHz hex) | ~800 MB/s | Capture+encode: ~300 MB/s peak | 62% |
| MIPI CSI 2-lane | ~1.5 Gbps | RAW10 1080p@30: ~750 Mbps | 50% |

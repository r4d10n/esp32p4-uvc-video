# ESP32-P4 UVC Webcam + RTSP IP Camera

USB Video Class 1.5 webcam and RTSP/RTP IP camera running on the ESP32-P4 with an OV5647 sensor. Supports simultaneous USB and Ethernet streaming.

## Hardware

- **Board:** Olimex ESP32-P4-DevKit
- **SoC:** ESP32-P4 (dual-core RISC-V, 400MHz)
- **Sensor:** OV5647 (5MP) via 2-lane MIPI CSI at 1920x1080 RAW10 30fps
- **Memory:** 32MB PSRAM (hex mode, 200MHz)
- **USB:** High-Speed bulk (dedicated DP/DN pins, not the USB-C connector)
- **Ethernet:** IP101GR PHY (onboard RMII, 100Mbps)

### Wiring

| Function | GPIO |
|----------|------|
| I2C SDA (SCCB) | 7 |
| I2C SCL (SCCB) | 8 |
| XCLK (24MHz) | 40 |
| MIPI CSI | 43-48 (dedicated) |
| Ethernet MDC | 31 |
| Ethernet MDIO | 52 |
| Ethernet PHY RST | 51 |

The OV5647 camera module connects via the board's MIPI CSI ribbon connector. USB HS uses dedicated pins (not the USB-C port, which is serial/JTAG only).

## Features

### USB Video Class (UVC 1.5)

Three output formats over USB High-Speed bulk:

| Format | Resolutions | Notes |
|--------|-------------|-------|
| UYVY (uncompressed) | 640x480, 320x240 | 2 bytes/pixel, bandwidth-limited |
| MJPEG | 1920x1080, 1280x720, 640x480 | Hardware JPEG encoder |
| H.264 | 1920x1080, 1280x720, 640x480 | Hardware H.264 encoder, frame-based |

All formats run at 30fps. Non-native resolutions are center-cropped from the 1080p capture.

**Processing Unit controls** (adjustable from host):
- Brightness, contrast, hue, saturation, sharpness, gain
- White balance temperature (switches ISP color profiles: Tungsten 2873K through Shade 7600K)
- JPEG quality, H.264 bitrate/GOP/QP via extension units

### RTSP/RTP Ethernet Streaming

H.264 video over standard RTSP (RFC 2326) and RTP (RFC 6184):

- **URL:** `rtsp://<device-ip>:554/stream`
- **Codec:** H.264 Constrained Baseline, 1920x1080@30fps
- **Default bitrate:** 8 Mbps (configurable, GOP=10)
- **Transport:** RTP/AVP over UDP unicast
- **Clients:** Single concurrent client

The RTSP server operates in **self-capture mode** -- it independently drives the camera and H.264 encoder when USB is idle. When a USB host starts UVC streaming, the RTSP server yields the camera and pauses until USB streaming stops.

### ISP Pipeline

The on-chip ISP processes RAW10 Bayer data with:
- Color Correction Matrix (CCM) per color temperature profile
- Auto White Balance gains
- Gamma correction (sRGB curve)
- Sharpening, Bayer-domain denoising, demosaic control

Six calibrated profiles from RPi libcamera OV5647 tuning data (2873K-7600K).

## Build

### Prerequisites

- [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32p4/get-started/)
- ESP32-P4 target support enabled

### Compile and Flash

```bash
# Set up ESP-IDF environment
source /path/to/esp-idf/export.sh

# Build
idf.py set-target esp32p4
idf.py build

# Flash (uses /dev/ttyACM0 by default)
idf.py flash

# Monitor serial output
idf.py monitor
```

After flashing, the device enumerates as a UVC webcam on USB and starts the RTSP server on Ethernet.

### Full Clean Build

If switching Kconfig options or encountering stale cache issues:

```bash
idf.py fullclean
rm -f sdkconfig
idf.py build
```

## Configuration

All settings are in `idf.py menuconfig` under **UVC Webcam Configuration**:

### ISP Color Profile

Default white balance profile applied at startup. Changeable at runtime via the UVC white_balance_temperature control.

| Profile | Color Temperature |
|---------|-------------------|
| Tungsten | 2873K |
| Indoor-Warm | 3725K |
| Fluorescent | 5095K |
| **Daylight** (default) | 6015K |
| Cloudy | 6865K |
| Shade | 7600K |

### MJPEG Settings

| Option | Default | Range |
|--------|---------|-------|
| JPEG quality | 80 | 1-100 |

### H.264 Settings (USB)

| Option | Default | Range |
|--------|---------|-------|
| Bitrate | 1,000,000 bps | 25K-2.5M |
| I-frame period | 30 | 1-120 |
| Min QP | 25 | 0-51 |
| Max QP | 50 | 0-51 |

USB H.264 defaults favor low bitrate since USB HS bulk bandwidth is shared with other formats.

### Ethernet / RTSP

| Option | Default | Range |
|--------|---------|-------|
| IP mode | Static | Static / DHCP |
| Static IP | 192.168.0.200 | -- |
| Netmask | 255.255.255.0 | -- |
| Gateway | 192.168.0.1 | -- |
| RTSP port | 554 | 1-65535 |
| RTSP H.264 bitrate | 8,000,000 bps | 500K-20M |
| RTSP H.264 I-period (GOP) | 10 | 1-120 |
| RTSP H.264 min QP | 20 | 0-51 |
| RTSP H.264 max QP | 38 | 0-51 |

RTSP uses separate H.264 parameters from USB since Ethernet has higher bandwidth (100Mbps) and benefits from higher bitrate and P-frame compression.

## Usage

### USB Webcam

Connect the USB HS port (not USB-C) to a host. The device appears as a standard UVC webcam.

```bash
# List available formats
v4l2-ctl -d /dev/video0 --list-formats-ext

# Play MJPEG 1080p
ffplay /dev/video0 -input_format mjpeg -video_size 1920x1080

# Play H.264 1080p
ffplay /dev/video0 -input_format h264 -video_size 1920x1080

# Play H.264 720p (center-cropped)
ffplay /dev/video0 -input_format h264 -video_size 1280x720

# Record to file
ffmpeg -f v4l2 -input_format mjpeg -video_size 1920x1080 -i /dev/video0 output.mkv
```

### RTSP Streaming

Connect Ethernet and wait for link-up (check serial output for IP address).

```bash
# Play with ffplay (low latency flags)
ffplay -fflags nobuffer -flags low_delay -framedrop rtsp://<device-ip>:554/stream

# Play with VLC
vlc rtsp://<device-ip>:554/stream

# Probe stream info
ffprobe rtsp://<device-ip>:554/stream

# Record to file
ffmpeg -i rtsp://<device-ip>:554/stream -c copy output.mp4
```

### Simultaneous USB + Ethernet

Both interfaces can be active, but only one drives the camera at a time:

1. **Ethernet only** -- RTSP self-captures from camera/encoder at its own quality settings
2. **USB connects** -- RTSP yields, USB takes over camera/encoder
3. **USB disconnects** -- RTSP resumes self-capture automatically

This ensures zero resource conflicts while allowing both interfaces to be available.

## Architecture

```
OV5647 ──MIPI CSI──> ISP (RAW10→UYVY) ──> V4L2 Capture (1920x1080)
                                                │
                                          ┌─────┴──────┐
                                          │            │
                                     H.264 HW     JPEG HW
                                      encoder      encoder
                                          │            │
                                    ┌─────┴──────┐     │
                                    │            │     │
                                RTSP/RTP      UVC USB (H.264 + MJPEG + UYVY)
                              (Ethernet)     (High-Speed bulk)
                              port 554
```

### Source Files

| File | Purpose |
|------|---------|
| `app_main.c` | Startup sequencing |
| `camera_pipeline.c` | V4L2 camera + ISP initialization |
| `encoder_manager.c` | H.264/JPEG hardware encoder lifecycle |
| `uvc_streaming.c` | UVC format negotiation, frame capture, encoding |
| `uvc_controls.c` | Processing Unit + Extension Unit control bridge |
| `eth_init.c` | Ethernet PHY init, static IP / DHCP |
| `rtsp_server.c` | RTSP protocol handler, self-capture loop |
| `rtp_sender.c` | RTP H.264 packetization (NAL/FU-A) |
| `perf_monitor.c` | CPU usage, memory, streaming stats |
| `board_olimex_p4.h` | Board pin definitions |

### Key Dependencies

| Component | Source |
|-----------|--------|
| `esp_video` | ESP-IDF camera V4L2 framework |
| `esp_cam_sensor` | OV5647 MIPI CSI driver |
| `esp_h264` | Hardware H.264 encoder |
| `tinyusb` | USB device stack (UVC 1.5) |
| `usb_device_uvc` | UVC class driver (local component) |
| `esp_eth` | Ethernet MAC + IP101 PHY driver |

## License

Apache-2.0

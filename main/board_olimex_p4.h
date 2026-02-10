/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

/*
 * Olimex ESP32-P4-DevKit Board Configuration
 *
 * MIPI CSI: 2-lane, dedicated pins (43-48)
 * I2C/SCCB: GPIO7 (SDA), GPIO8 (SCL), port 1
 * USB HS:   Dedicated USB_DP/USB_DN pins (not on USB-C connector)
 * USB-C:    Serial/JTAG only (GPIO24/25, FS)
 * PSRAM:    32MB hex mode @ 200MHz
 */

/* I2C / SCCB for camera sensor */
#define BOARD_I2C_PORT          0
#define BOARD_I2C_SDA_PIN       7
#define BOARD_I2C_SCL_PIN       8
#define BOARD_I2C_FREQ          100000

/* Camera sensor control pins (not connected on Olimex board) */
#define BOARD_CAM_RESET_PIN     (-1)
#define BOARD_CAM_PWDN_PIN      (-1)

/* XCLK - ESP32-P4 clock router output for OV5647 master clock */
#define BOARD_CAM_XCLK_PIN      40
#define BOARD_CAM_XCLK_FREQ     24000000

/* MIPI CSI LDO - ESP32-P4 internal LDO channel 3 for MIPI PHY (2.5V) */
#define BOARD_CSI_LDO_CHAN       3
#define BOARD_CSI_LDO_MV        2500
#define BOARD_CSI_DONT_INIT_LDO  true  /* We init LDO ourselves before esp_video_init */

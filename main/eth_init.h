/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Ethernet (IP101GR PHY on Olimex ESP32-P4-DevKit)
 *
 * Initializes the TCP/IP stack, event loop, EMAC MAC+PHY, attaches
 * a netif, and starts DHCP.  Non-blocking — DHCP runs in background.
 *
 * @return ESP_OK on success
 */
esp_err_t eth_init(void);

#ifdef __cplusplus
}
#endif

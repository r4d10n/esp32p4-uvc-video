/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "uvc_streaming.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start the performance monitor task
 *
 * Periodically logs CPU usage (per core), heap memory stats,
 * and USB streaming throughput to the serial console.
 *
 * @param stream_ctx  Pointer to the UVC stream context (for streaming counters)
 */
esp_err_t perf_monitor_start(uvc_stream_ctx_t *stream_ctx);

#ifdef __cplusplus
}
#endif

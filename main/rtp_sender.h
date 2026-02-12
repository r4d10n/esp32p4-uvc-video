/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "lwip/sockets.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int sock_fd;                  /* UDP socket */
    struct sockaddr_in dest;      /* Client RTP destination (from RTSP SETUP) */
    uint16_t seq;                 /* RTP sequence number */
    uint32_t ssrc;                /* Random SSRC identifier */
    uint32_t timestamp;           /* 90kHz RTP clock */
    bool active;                  /* True when PLAY is active */
} rtp_session_t;

/**
 * @brief Initialize an RTP session
 *
 * Creates a UDP socket and generates a random SSRC.
 * Does NOT start sending — call rtp_session_set_dest() then rtp_session_start().
 */
esp_err_t rtp_session_init(rtp_session_t *session);

/**
 * @brief Set the RTP destination (client IP + port from RTSP SETUP)
 */
void rtp_session_set_dest(rtp_session_t *session,
                           uint32_t client_ip, uint16_t client_port);

/**
 * @brief Mark session as active (PLAY received)
 */
void rtp_session_start(rtp_session_t *session);

/**
 * @brief Mark session as inactive (TEARDOWN/PAUSE received)
 */
void rtp_session_stop(rtp_session_t *session);

/**
 * @brief Send an H.264 Annex-B frame over RTP
 *
 * Parses the frame into NAL units and sends each as:
 *   - Single NAL Unit packet if NAL <= MTU
 *   - FU-A fragmented packets if NAL > MTU
 *
 * Per RFC 6184 (RTP Payload Format for H.264 Video).
 *
 * @param session   Active RTP session
 * @param frame     H.264 Annex-B frame (with 00 00 00 01 start codes)
 * @param len       Frame length in bytes
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not active
 */
esp_err_t rtp_send_h264_frame(rtp_session_t *session,
                               const uint8_t *frame, size_t len);

/**
 * @brief Close the RTP session and release the socket
 */
void rtp_session_close(rtp_session_t *session);

#ifdef __cplusplus
}
#endif

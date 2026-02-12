/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RTP H.264 packetization per RFC 6184.
 *
 * Supports:
 *   - Single NAL Unit packets (NAL size <= MTU)
 *   - FU-A fragmentation (NAL size > MTU)
 *
 * Timestamp clock: 90kHz (standard for H.264 RTP).
 */

#include "rtp_sender.h"
#include "esp_log.h"
#include "esp_random.h"
#include <string.h>

static const char *TAG = "rtp";

/* Ethernet MTU=1500, IP=20, UDP=8, RTP=12 → max payload ~1400 */
#define RTP_MTU             1400
#define RTP_HEADER_SIZE     12

/* 90kHz clock ticks per frame at 30fps */
#define TICKS_PER_FRAME_30FPS  3000

/*
 * Build an RTP header (12 bytes) into buf.
 * V=2, P=0, X=0, CC=0, M=marker, PT=96
 */
static void rtp_build_header(uint8_t *buf, rtp_session_t *s, bool marker)
{
    buf[0] = 0x80;                          /* V=2 */
    buf[1] = (marker ? 0x80 : 0x00) | 96;  /* M + PT=96 */
    buf[2] = (s->seq >> 8) & 0xFF;
    buf[3] = s->seq & 0xFF;
    buf[4] = (s->timestamp >> 24) & 0xFF;
    buf[5] = (s->timestamp >> 16) & 0xFF;
    buf[6] = (s->timestamp >>  8) & 0xFF;
    buf[7] = s->timestamp & 0xFF;
    buf[8]  = (s->ssrc >> 24) & 0xFF;
    buf[9]  = (s->ssrc >> 16) & 0xFF;
    buf[10] = (s->ssrc >>  8) & 0xFF;
    buf[11] = s->ssrc & 0xFF;
}

/*
 * Find the next NAL unit in an Annex-B stream.
 * Looks for 00 00 00 01 or 00 00 01 start codes.
 * Returns pointer to first byte after start code, sets *nal_len.
 */
static const uint8_t *find_next_nal(const uint8_t *data, size_t len,
                                     size_t *nal_len)
{
    const uint8_t *p = data;
    const uint8_t *end = data + len;

    /* Skip to start code */
    while (p + 3 < end) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
            p += 3;
            goto found;
        }
        if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p + 3 < end && p[3] == 1) {
            p += 4;
            goto found;
        }
        p++;
    }
    return NULL;

found:;
    /* Find end of this NAL (next start code or end of data) */
    const uint8_t *nal_start = p;
    const uint8_t *q = p;
    while (q + 2 < end) {
        if (q[0] == 0 && q[1] == 0 && (q[2] == 1 || (q[2] == 0 && q + 3 < end && q[3] == 1))) {
            break;
        }
        q++;
    }
    if (q + 2 >= end) {
        q = end;
    }
    /* Trim trailing zeros before next start code */
    while (q > nal_start && q[-1] == 0) {
        q--;
    }
    *nal_len = q - nal_start;
    return nal_start;
}

/*
 * Send a single NAL unit that fits in one RTP packet.
 * RTP payload = NAL header + NAL body (the NAL byte is part of the data).
 */
static esp_err_t send_single_nal(rtp_session_t *s, const uint8_t *nal,
                                  size_t nal_len, bool last_nal)
{
    uint8_t pkt[RTP_HEADER_SIZE + RTP_MTU];
    rtp_build_header(pkt, s, last_nal);
    s->seq++;

    memcpy(pkt + RTP_HEADER_SIZE, nal, nal_len);

    int ret = sendto(s->sock_fd, pkt, RTP_HEADER_SIZE + nal_len, 0,
                     (struct sockaddr *)&s->dest, sizeof(s->dest));
    if (ret < 0) {
        ESP_LOGD(TAG, "sendto failed: errno %d", errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/*
 * Send a large NAL unit using FU-A fragmentation (RFC 6184 Section 5.8).
 *
 * FU-A packet format:
 *   [RTP Header (12)] [FU Indicator (1)] [FU Header (1)] [FU Payload (N)]
 *
 * FU Indicator: (nal[0] & 0xE0) | 28  (type=28 for FU-A)
 * FU Header:    S|E|R|Type  (S=start, E=end, R=0, Type=nal[0]&0x1F)
 */
static esp_err_t send_fua_nal(rtp_session_t *s, const uint8_t *nal,
                               size_t nal_len, bool last_nal)
{
    uint8_t pkt[RTP_HEADER_SIZE + 2 + RTP_MTU];
    uint8_t fu_indicator = (nal[0] & 0xE0) | 28;  /* NRI + FU-A type */
    uint8_t nal_type = nal[0] & 0x1F;

    /* Skip the first NAL header byte — it's encoded in FU indicator/header */
    const uint8_t *payload = nal + 1;
    size_t remaining = nal_len - 1;
    size_t max_frag = RTP_MTU - 2;  /* 2 bytes for FU indicator + FU header */
    bool first = true;

    while (remaining > 0) {
        size_t frag_len = (remaining > max_frag) ? max_frag : remaining;
        bool last_frag = (frag_len == remaining);

        /* Marker bit set on last fragment of last NAL in frame */
        rtp_build_header(pkt, s, last_nal && last_frag);
        s->seq++;

        /* FU Indicator */
        pkt[RTP_HEADER_SIZE] = fu_indicator;

        /* FU Header: S=start, E=end, R=0, Type */
        uint8_t fu_header = nal_type;
        if (first) fu_header |= 0x80;      /* S bit */
        if (last_frag) fu_header |= 0x40;  /* E bit */
        pkt[RTP_HEADER_SIZE + 1] = fu_header;

        memcpy(pkt + RTP_HEADER_SIZE + 2, payload, frag_len);

        int ret = sendto(s->sock_fd, pkt, RTP_HEADER_SIZE + 2 + frag_len, 0,
                         (struct sockaddr *)&s->dest, sizeof(s->dest));
        if (ret < 0) {
            ESP_LOGD(TAG, "FU-A sendto failed: errno %d", errno);
            return ESP_FAIL;
        }

        payload += frag_len;
        remaining -= frag_len;
        first = false;
    }

    return ESP_OK;
}

esp_err_t rtp_session_init(rtp_session_t *session)
{
    memset(session, 0, sizeof(*session));
    session->ssrc = esp_random();
    session->seq = (uint16_t)(esp_random() & 0xFFFF);
    session->active = false;

    session->sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (session->sock_fd < 0) {
        ESP_LOGE(TAG, "UDP socket create failed: errno %d", errno);
        return ESP_FAIL;
    }

    /* Set send buffer and make non-blocking to avoid stalling the pipeline */
    int sndbuf = 65536;
    setsockopt(session->sock_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    ESP_LOGI(TAG, "RTP session initialized (SSRC=0x%08lx)", (unsigned long)session->ssrc);
    return ESP_OK;
}

void rtp_session_set_dest(rtp_session_t *session,
                           uint32_t client_ip, uint16_t client_port)
{
    memset(&session->dest, 0, sizeof(session->dest));
    session->dest.sin_family = AF_INET;
    session->dest.sin_addr.s_addr = client_ip;
    session->dest.sin_port = htons(client_port);

    ESP_LOGI(TAG, "RTP dest: %d.%d.%d.%d:%d",
             (client_ip) & 0xFF, (client_ip >> 8) & 0xFF,
             (client_ip >> 16) & 0xFF, (client_ip >> 24) & 0xFF,
             client_port);
}

void rtp_session_start(rtp_session_t *session)
{
    session->active = true;
    ESP_LOGI(TAG, "RTP streaming started");
}

void rtp_session_stop(rtp_session_t *session)
{
    session->active = false;
    ESP_LOGI(TAG, "RTP streaming stopped");
}

esp_err_t rtp_send_h264_frame(rtp_session_t *session,
                               const uint8_t *frame, size_t len)
{
    if (!session->active) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Advance timestamp by one frame period (90kHz / 30fps = 3000 ticks) */
    session->timestamp += TICKS_PER_FRAME_30FPS;

    /*
     * Parse Annex-B stream into individual NAL units.
     * A typical H.264 frame contains: SPS, PPS, then one or more slice NALs.
     */
    const uint8_t *p = frame;
    size_t remaining = len;
    const uint8_t *nal;
    size_t nal_len;

    /* Collect all NAL start positions first to know which is last */
    typedef struct { const uint8_t *ptr; size_t len; } nal_info_t;
    nal_info_t nals[16];  /* Enough for SPS+PPS+slices */
    int nal_count = 0;

    while (remaining > 0 && nal_count < 16) {
        nal = find_next_nal(p, remaining, &nal_len);
        if (!nal || nal_len == 0) break;
        nals[nal_count].ptr = nal;
        nals[nal_count].len = nal_len;
        nal_count++;
        size_t consumed = (nal + nal_len) - p;
        p += consumed;
        remaining -= consumed;
    }

    /* Send each NAL */
    for (int i = 0; i < nal_count; i++) {
        bool last = (i == nal_count - 1);
        if (nals[i].len <= RTP_MTU) {
            send_single_nal(session, nals[i].ptr, nals[i].len, last);
        } else {
            send_fua_nal(session, nals[i].ptr, nals[i].len, last);
        }
    }

    return ESP_OK;
}

void rtp_session_close(rtp_session_t *session)
{
    session->active = false;
    if (session->sock_fd >= 0) {
        close(session->sock_fd);
        session->sock_fd = -1;
    }
    ESP_LOGI(TAG, "RTP session closed");
}

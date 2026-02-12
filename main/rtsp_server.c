/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal RTSP 1.0 server (RFC 2326) for H.264 streaming over RTP.
 *
 * Supports one client at a time with UDP unicast RTP transport.
 * Methods: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN.
 */

#include "rtsp_server.h"
#include "rtp_sender.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "esp_netif.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "rtsp";

#define RTSP_PORT           554
#define RTSP_BUF_SIZE       2048
#define RTSP_STACK_SIZE     8192
#define RTSP_TASK_PRIO      10

/* Max H.264 frame size for RTSP copy buffer (256KB covers 1080p IDR frames) */
#define RTSP_FRAME_BUF_SIZE (256 * 1024)

/* RTSP session state */
typedef enum {
    RTSP_STATE_INIT,
    RTSP_STATE_READY,
    RTSP_STATE_PLAYING,
} rtsp_state_t;

static struct {
    rtp_session_t rtp;
    rtsp_state_t  state;
    uint32_t      session_id;
    int           client_fd;

    /* H.264 frame double-buffer for decoupling UVC and RTP paths */
    uint8_t      *frame_buf;
    size_t        frame_len;
    SemaphoreHandle_t frame_ready;
    SemaphoreHandle_t frame_mutex;
} s_rtsp;

/* ---- H.264 frame feeding ------------------------------------------------ */

void rtsp_server_feed_h264(const uint8_t *data, size_t len)
{
    if (s_rtsp.state != RTSP_STATE_PLAYING || !s_rtsp.frame_buf) {
        return;
    }

    /* Copy frame under mutex — drop if mutex busy (non-blocking) */
    if (xSemaphoreTake(s_rtsp.frame_mutex, 0) == pdTRUE) {
        size_t copy_len = (len > RTSP_FRAME_BUF_SIZE) ? RTSP_FRAME_BUF_SIZE : len;
        memcpy(s_rtsp.frame_buf, data, copy_len);
        s_rtsp.frame_len = copy_len;
        xSemaphoreGive(s_rtsp.frame_mutex);

        /* Signal RTP sender that a new frame is available */
        xSemaphoreGive(s_rtsp.frame_ready);
    }
    /* else: RTP sender is busy with previous frame, drop this one */
}

/* ---- RTSP protocol helpers ---------------------------------------------- */

static int rtsp_get_cseq(const char *request)
{
    const char *p = strstr(request, "CSeq:");
    if (!p) p = strstr(request, "cseq:");
    if (!p) return 0;
    return atoi(p + 5);
}

/*
 * Get the local IP address of the Ethernet interface.
 * Returns "0.0.0.0" if not available.
 */
static void get_local_ip(char *buf, size_t buflen)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("ETH_DEF");
    if (!netif) {
        snprintf(buf, buflen, "0.0.0.0");
        return;
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(buf, buflen, IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(buf, buflen, "0.0.0.0");
    }
}

static int send_response(int fd, const char *resp)
{
    return send(fd, resp, strlen(resp), 0);
}

static void handle_options(int fd, int cseq)
{
    char resp[256];
    snprintf(resp, sizeof(resp),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n"
             "\r\n",
             cseq);
    send_response(fd, resp);
}

static void handle_describe(int fd, int cseq)
{
    char local_ip[32];
    get_local_ip(local_ip, sizeof(local_ip));

    char sdp[512];
    int sdp_len = snprintf(sdp, sizeof(sdp),
        "v=0\r\n"
        "o=- 0 0 IN IP4 %s\r\n"
        "s=ESP32-P4 Camera\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1\r\n"
        "a=control:track1\r\n",
        local_ip);

    char resp[1024];
    snprintf(resp, sizeof(resp),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Content-Type: application/sdp\r\n"
             "Content-Length: %d\r\n"
             "\r\n"
             "%s",
             cseq, sdp_len, sdp);
    send_response(fd, resp);
}

/*
 * Parse "Transport:" header to extract client_port.
 * Example: Transport: RTP/AVP;unicast;client_port=5000-5001
 */
static uint16_t parse_client_port(const char *request)
{
    const char *p = strstr(request, "client_port=");
    if (!p) return 0;
    return (uint16_t)atoi(p + 12);
}

static void handle_setup(int fd, int cseq, const char *request,
                          struct sockaddr_in *client_addr)
{
    uint16_t client_port = parse_client_port(request);
    if (client_port == 0) {
        char resp[256];
        snprintf(resp, sizeof(resp),
                 "RTSP/1.0 461 Unsupported Transport\r\n"
                 "CSeq: %d\r\n\r\n", cseq);
        send_response(fd, resp);
        return;
    }

    /* Configure RTP destination */
    rtp_session_set_dest(&s_rtsp.rtp, client_addr->sin_addr.s_addr, client_port);

    s_rtsp.session_id = esp_random();
    s_rtsp.state = RTSP_STATE_READY;

    /* Get the local RTP port (ephemeral, assigned by OS) */
    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    getsockname(s_rtsp.rtp.sock_fd, (struct sockaddr *)&local, &local_len);
    uint16_t server_port = ntohs(local.sin_port);

    char resp[512];
    snprintf(resp, sizeof(resp),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
             "Session: %08lx\r\n"
             "\r\n",
             cseq, client_port, client_port + 1,
             server_port, server_port + 1,
             (unsigned long)s_rtsp.session_id);
    send_response(fd, resp);

    ESP_LOGI(TAG, "SETUP: client_port=%d, session=%08lx",
             client_port, (unsigned long)s_rtsp.session_id);
}

static void handle_play(int fd, int cseq)
{
    if (s_rtsp.state != RTSP_STATE_READY && s_rtsp.state != RTSP_STATE_PLAYING) {
        char resp[256];
        snprintf(resp, sizeof(resp),
                 "RTSP/1.0 455 Method Not Valid in This State\r\n"
                 "CSeq: %d\r\n\r\n", cseq);
        send_response(fd, resp);
        return;
    }

    rtp_session_start(&s_rtsp.rtp);
    s_rtsp.state = RTSP_STATE_PLAYING;

    char resp[256];
    snprintf(resp, sizeof(resp),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n"
             "Session: %08lx\r\n"
             "\r\n",
             cseq, (unsigned long)s_rtsp.session_id);
    send_response(fd, resp);

    ESP_LOGI(TAG, "PLAY: RTP streaming started");
}

static void handle_teardown(int fd, int cseq)
{
    rtp_session_stop(&s_rtsp.rtp);
    s_rtsp.state = RTSP_STATE_INIT;

    char resp[256];
    snprintf(resp, sizeof(resp),
             "RTSP/1.0 200 OK\r\n"
             "CSeq: %d\r\n\r\n",
             cseq);
    send_response(fd, resp);

    ESP_LOGI(TAG, "TEARDOWN: session ended");
}

/* ---- RTP sender task ---------------------------------------------------- */

static void rtp_sender_task(void *arg)
{
    ESP_LOGI(TAG, "RTP sender task started");

    /* Temporary buffer for sending (avoid holding mutex during sendto) */
    uint8_t *send_buf = heap_caps_malloc(RTSP_FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!send_buf) {
        ESP_LOGE(TAG, "Failed to allocate RTP send buffer");
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        /* Wait for a new frame (or timeout to check for state changes) */
        if (xSemaphoreTake(s_rtsp.frame_ready, pdMS_TO_TICKS(1000)) != pdTRUE) {
            continue;
        }

        if (s_rtsp.state != RTSP_STATE_PLAYING) {
            continue;
        }

        /* Copy frame out under mutex */
        size_t len = 0;
        if (xSemaphoreTake(s_rtsp.frame_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            len = s_rtsp.frame_len;
            if (len > 0) {
                memcpy(send_buf, s_rtsp.frame_buf, len);
            }
            s_rtsp.frame_len = 0;
            xSemaphoreGive(s_rtsp.frame_mutex);
        }

        if (len > 0) {
            rtp_send_h264_frame(&s_rtsp.rtp, send_buf, len);
        }
    }
}

/* ---- RTSP control task -------------------------------------------------- */

static void handle_client(int client_fd, struct sockaddr_in *client_addr)
{
    char buf[RTSP_BUF_SIZE];
    s_rtsp.client_fd = client_fd;
    s_rtsp.state = RTSP_STATE_INIT;

    ESP_LOGI(TAG, "Client connected from %d.%d.%d.%d:%d",
             ((uint8_t *)&client_addr->sin_addr.s_addr)[0],
             ((uint8_t *)&client_addr->sin_addr.s_addr)[1],
             ((uint8_t *)&client_addr->sin_addr.s_addr)[2],
             ((uint8_t *)&client_addr->sin_addr.s_addr)[3],
             ntohs(client_addr->sin_port));

    /* Set TCP receive timeout */
    struct timeval tv = { .tv_sec = 60, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (1) {
        int n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            if (n == 0) {
                ESP_LOGI(TAG, "Client disconnected");
            } else {
                ESP_LOGW(TAG, "recv error: %d", errno);
            }
            break;
        }
        buf[n] = '\0';

        int cseq = rtsp_get_cseq(buf);

        if (strncmp(buf, "OPTIONS", 7) == 0) {
            handle_options(client_fd, cseq);
        } else if (strncmp(buf, "DESCRIBE", 8) == 0) {
            handle_describe(client_fd, cseq);
        } else if (strncmp(buf, "SETUP", 5) == 0) {
            handle_setup(client_fd, cseq, buf, client_addr);
        } else if (strncmp(buf, "PLAY", 4) == 0) {
            handle_play(client_fd, cseq);
        } else if (strncmp(buf, "TEARDOWN", 8) == 0) {
            handle_teardown(client_fd, cseq);
            break;
        } else {
            char resp[256];
            snprintf(resp, sizeof(resp),
                     "RTSP/1.0 405 Method Not Allowed\r\n"
                     "CSeq: %d\r\n\r\n", cseq);
            send_response(client_fd, resp);
        }
    }

    /* Clean up on disconnect */
    rtp_session_stop(&s_rtsp.rtp);
    s_rtsp.state = RTSP_STATE_INIT;
    close(client_fd);
    s_rtsp.client_fd = -1;
}

static void rtsp_server_task(void *arg)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "Socket create failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(RTSP_PORT),
    };

    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Bind failed: errno %d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    if (listen(listen_fd, 1) < 0) {
        ESP_LOGE(TAG, "Listen failed: errno %d", errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "RTSP server listening on port %d", RTSP_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) {
            ESP_LOGW(TAG, "Accept failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        handle_client(client_fd, &client_addr);
    }
}

/* ---- Public API --------------------------------------------------------- */

esp_err_t rtsp_server_start(void)
{
    memset(&s_rtsp, 0, sizeof(s_rtsp));
    s_rtsp.client_fd = -1;
    s_rtsp.state = RTSP_STATE_INIT;

    /* Initialize RTP session */
    ESP_RETURN_ON_ERROR(rtp_session_init(&s_rtsp.rtp), TAG, "RTP init failed");

    /* Allocate frame buffer in PSRAM */
    s_rtsp.frame_buf = heap_caps_malloc(RTSP_FRAME_BUF_SIZE, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_rtsp.frame_buf, ESP_ERR_NO_MEM, TAG,
                        "Frame buffer alloc failed (%d bytes)", RTSP_FRAME_BUF_SIZE);

    /* Create synchronization primitives */
    s_rtsp.frame_ready = xSemaphoreCreateBinary();
    s_rtsp.frame_mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_rtsp.frame_ready && s_rtsp.frame_mutex,
                        ESP_ERR_NO_MEM, TAG, "Semaphore create failed");

    /* Start RTP sender task */
    BaseType_t ret = xTaskCreate(rtp_sender_task, "rtp_sender",
                                  4096, NULL, RTSP_TASK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "RTP sender task create failed");

    /* Start RTSP control task */
    ret = xTaskCreate(rtsp_server_task, "rtsp_server",
                      RTSP_STACK_SIZE, NULL, RTSP_TASK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "RTSP server task create failed");

    ESP_LOGI(TAG, "RTSP server started (port %d, frame buf %dKB)",
             RTSP_PORT, RTSP_FRAME_BUF_SIZE / 1024);

    return ESP_OK;
}

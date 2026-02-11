/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runtime performance monitor for the UVC webcam.
 *
 * Periodically (every 5 seconds) logs:
 *   - Per-core CPU usage (derived from IDLE task runtime deltas)
 *   - Heap memory: internal SRAM and PSRAM (free / total / min-ever-free)
 *   - USB streaming: fps, MB/s, total frames
 *
 * CPU usage method: FreeRTOS runtime stats track cumulative execution time
 * per task. IDLE0 and IDLE1 are pinned to core 0 and core 1 respectively.
 * CPU_usage = 1 - (idle_delta / elapsed_delta).
 */

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "perf_monitor.h"

static const char *TAG = "perf_mon";

#define PERF_INTERVAL_MS    5000
#define MAX_TASKS           40
#define PERF_TASK_STACK     4096

static uvc_stream_ctx_t *s_stream_ctx;

/* Previous snapshot for delta computation */
#if configGENERATE_RUN_TIME_STATS
static uint32_t s_prev_idle0_runtime;
static uint32_t s_prev_idle1_runtime;
static uint32_t s_prev_total_runtime;
#endif
static uint32_t s_prev_frame_count;
static uint64_t s_prev_byte_count;

static void log_cpu_usage(void)
{
#if configGENERATE_RUN_TIME_STATS
    TaskStatus_t *tasks = malloc(MAX_TASKS * sizeof(TaskStatus_t));
    if (!tasks) return;

    uint32_t total_runtime;
    UBaseType_t count = uxTaskGetSystemState(tasks, MAX_TASKS, &total_runtime);

    uint32_t idle0_rt = 0, idle1_rt = 0;
    for (UBaseType_t i = 0; i < count; i++) {
        if (strcmp(tasks[i].pcTaskName, "IDLE0") == 0) {
            idle0_rt = tasks[i].ulRunTimeCounter;
        } else if (strcmp(tasks[i].pcTaskName, "IDLE1") == 0) {
            idle1_rt = tasks[i].ulRunTimeCounter;
        }
    }

    uint32_t dt = total_runtime - s_prev_total_runtime;
    if (dt > 0) {
        float cpu0 = 100.0f * (1.0f - (float)(idle0_rt - s_prev_idle0_runtime) / dt);
        float cpu1 = 100.0f * (1.0f - (float)(idle1_rt - s_prev_idle1_runtime) / dt);
        /* Clamp to [0, 100] — small timing races can cause slight negatives */
        if (cpu0 < 0) cpu0 = 0;
        if (cpu1 < 0) cpu1 = 0;
        ESP_LOGI(TAG, "CPU: core0=%.1f%% (video) | core1=%.1f%% (USB)", cpu0, cpu1);
    }

    s_prev_idle0_runtime = idle0_rt;
    s_prev_idle1_runtime = idle1_rt;
    s_prev_total_runtime = total_runtime;

    free(tasks);
#else
    ESP_LOGI(TAG, "CPU: runtime stats not enabled");
#endif
}

static void log_memory_usage(void)
{
    multi_heap_info_t internal, spiram;
    heap_caps_get_info(&internal, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&spiram, MALLOC_CAP_SPIRAM);

    ESP_LOGI(TAG, "Heap internal: %lu free, %lu alloc, %lu min-free  (total %lu)",
             (unsigned long)internal.total_free_bytes,
             (unsigned long)internal.total_allocated_bytes,
             (unsigned long)internal.minimum_free_bytes,
             (unsigned long)(internal.total_free_bytes + internal.total_allocated_bytes));
    ESP_LOGI(TAG, "Heap PSRAM:    %lu free, %lu alloc, %lu min-free  (total %lu)",
             (unsigned long)spiram.total_free_bytes,
             (unsigned long)spiram.total_allocated_bytes,
             (unsigned long)spiram.minimum_free_bytes,
             (unsigned long)(spiram.total_free_bytes + spiram.total_allocated_bytes));
}

static void log_stream_stats(void)
{
    if (!s_stream_ctx) return;

    uint32_t frames = s_stream_ctx->perf_frame_count;
    uint64_t bytes = s_stream_ctx->perf_byte_count;

    uint32_t d_frames = frames - s_prev_frame_count;
    uint64_t d_bytes = bytes - s_prev_byte_count;

    float dt_sec = PERF_INTERVAL_MS / 1000.0f;
    float fps = d_frames / dt_sec;
    float mb_s = d_bytes / (dt_sec * 1024.0f * 1024.0f);
    float mbps = (d_bytes * 8.0f) / (dt_sec * 1e6f);

    s_prev_frame_count = frames;
    s_prev_byte_count = bytes;

    if (s_stream_ctx->streaming) {
        ESP_LOGI(TAG, "Stream: %.1f fps, %.2f MB/s (%.1f Mbps), %dx%d fmt=%d, %lu total frames",
                 fps, mb_s, mbps,
                 s_stream_ctx->negotiated_width, s_stream_ctx->negotiated_height,
                 s_stream_ctx->active_format,
                 (unsigned long)frames);
    } else {
        ESP_LOGI(TAG, "Stream: idle (no active stream)");
    }
}

static void perf_monitor_task(void *arg)
{
    /* Let the system settle before first report */
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* Prime the snapshot so first delta is meaningful */
#if configGENERATE_RUN_TIME_STATS
    {
        TaskStatus_t *tasks = malloc(MAX_TASKS * sizeof(TaskStatus_t));
        if (tasks) {
            uint32_t total;
            UBaseType_t count = uxTaskGetSystemState(tasks, MAX_TASKS, &total);
            for (UBaseType_t i = 0; i < count; i++) {
                if (strcmp(tasks[i].pcTaskName, "IDLE0") == 0)
                    s_prev_idle0_runtime = tasks[i].ulRunTimeCounter;
                else if (strcmp(tasks[i].pcTaskName, "IDLE1") == 0)
                    s_prev_idle1_runtime = tasks[i].ulRunTimeCounter;
            }
            s_prev_total_runtime = total;
            free(tasks);
        }
    }
#endif
    s_prev_frame_count = s_stream_ctx ? s_stream_ctx->perf_frame_count : 0;
    s_prev_byte_count = s_stream_ctx ? s_stream_ctx->perf_byte_count : 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(PERF_INTERVAL_MS));

        ESP_LOGI(TAG, "========== Performance Report ==========");
        log_cpu_usage();
        log_memory_usage();
        log_stream_stats();
    }
}

esp_err_t perf_monitor_start(uvc_stream_ctx_t *stream_ctx)
{
    s_stream_ctx = stream_ctx;

    BaseType_t ret = xTaskCreatePinnedToCore(
        perf_monitor_task,
        "perf_mon",
        PERF_TASK_STACK,
        NULL,
        1,       /* Low priority — must not interfere with streaming */
        NULL,
        tskNO_AFFINITY  /* Can run on either core */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create perf monitor task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Performance monitor started (interval=%ds)", PERF_INTERVAL_MS / 1000);
    return ESP_OK;
}

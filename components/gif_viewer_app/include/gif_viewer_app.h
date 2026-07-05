#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GIF_VIEWER_APP_NAME_MAX_LEN 96
#define GIF_VIEWER_APP_PATH_MAX_LEN 192

typedef enum {
    GIF_VIEWER_APP_STATE_STOPPED = 0,
    GIF_VIEWER_APP_STATE_STARTING,
    GIF_VIEWER_APP_STATE_RUNNING,
    GIF_VIEWER_APP_STATE_FAILED,
} gif_viewer_app_state_t;

typedef enum {
    GIF_VIEWER_APP_MEDIA_READY = 0,
    GIF_VIEWER_APP_MEDIA_SD_MOUNT_FAILED,
    GIF_VIEWER_APP_MEDIA_GIF_DIR_MISSING,
    GIF_VIEWER_APP_MEDIA_NO_GIFS,
    GIF_VIEWER_APP_MEDIA_SCAN_FAILED,
} gif_viewer_app_media_state_t;

typedef struct {
    bool enable_boot_button;
    bool enable_console;
    size_t event_queue_length;
    uint32_t task_stack_size;
    uint32_t task_priority;
    int task_core_id;
    size_t initial_index;
} gif_viewer_app_config_t;

#define GIF_VIEWER_APP_DEFAULT_CONFIG()       \
    {                                         \
        .enable_boot_button = true,           \
        .enable_console = true,               \
        .event_queue_length = 8,              \
        .task_stack_size = 6144,              \
        .task_priority = 4,                   \
        .task_core_id = -1,                   \
        .initial_index = 0,                   \
    }

typedef struct {
    gif_viewer_app_state_t state;
    gif_viewer_app_media_state_t media_state;
    esp_err_t last_error;
    size_t gif_count;
    int current_index;
    char current_name[GIF_VIEWER_APP_NAME_MAX_LEN];
    char source_path[GIF_VIEWER_APP_PATH_MAX_LEN];
    int width;
    int height;
    int frame_index;
    size_t file_size_bytes;
    size_t frame_data_size_bytes;
    size_t internal_free_bytes;
    size_t internal_largest_bytes;
    size_t psram_free_bytes;
    size_t psram_largest_bytes;
} gif_viewer_app_status_t;

esp_err_t gif_viewer_app_start(const gif_viewer_app_config_t *config);
esp_err_t gif_viewer_app_next(void);
esp_err_t gif_viewer_app_prev(void);
esp_err_t gif_viewer_app_show_index(size_t zero_based_index);
esp_err_t gif_viewer_app_get_status(gif_viewer_app_status_t *status);

#ifdef __cplusplus
}
#endif

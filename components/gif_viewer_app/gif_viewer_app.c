/*
 * GIF viewer app component.
 *
 * Provides a non-blocking app-level entry point for the SD-card GIF viewer.
 */

#include "gif_viewer_app.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_console.h"
#include "app_events.h"
#include "app_ui.h"
#include "bsp/esp-bsp.h"
#include "gif_player.h"
#include "gif_playlist.h"
#include "sd_media.h"

#define GIF_VIEWER_APP_TASK_NAME "gif_viewer_app"

static const char *TAG = "gif_viewer_app";

static QueueHandle_t s_evt_queue;
static SemaphoreHandle_t s_state_lock;
static TaskHandle_t s_task_handle;
static gif_viewer_app_config_t s_config;
static gif_viewer_app_state_t s_app_state = GIF_VIEWER_APP_STATE_STOPPED;
static esp_err_t s_last_error = ESP_OK;
static gif_playlist_t s_playlist;
static sd_media_status_t s_media;
static app_ui_t s_ui;
static gif_player_t *s_player;
static int s_cur_index = -1;

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0U) {
        return;
    }
    snprintf(dst, dst_size, "%s", (src != NULL) ? src : "");
}

static void state_lock(void)
{
    if (s_state_lock != NULL) {
        (void)xSemaphoreTake(s_state_lock, portMAX_DELAY);
    }
}

static void state_unlock(void)
{
    if (s_state_lock != NULL) {
        xSemaphoreGive(s_state_lock);
    }
}

static void set_app_state(gif_viewer_app_state_t state, esp_err_t err)
{
    state_lock();
    s_app_state = state;
    s_last_error = err;
    state_unlock();
}

static gif_viewer_app_state_t get_app_state(void)
{
    state_lock();
    gif_viewer_app_state_t state = s_app_state;
    state_unlock();
    return state;
}

static esp_err_t get_last_error(void)
{
    state_lock();
    esp_err_t err = s_last_error;
    state_unlock();
    return err;
}

static void set_current_index(int index)
{
    state_lock();
    s_cur_index = index;
    state_unlock();
}

static int get_current_index(void *ctx)
{
    (void)ctx;

    state_lock();
    int index = s_cur_index;
    state_unlock();
    return index;
}

static const char *event_name(app_evt_type_t type)
{
    switch (type) {
    case APP_EVT_NEXT:
        return "next";
    case APP_EVT_PREV:
        return "prev";
    case APP_EVT_GOTO:
        return "goto";
    default:
        return "unknown";
    }
}

static esp_err_t queue_event(app_evt_type_t type, int index)
{
    if (s_evt_queue == NULL) {
        ESP_LOGW(TAG, "Event queue is not ready, drop %s event", event_name(type));
        return ESP_ERR_INVALID_STATE;
    }

    const gif_viewer_app_state_t state = get_app_state();
    if (state != GIF_VIEWER_APP_STATE_STARTING && state != GIF_VIEWER_APP_STATE_RUNNING) {
        ESP_LOGW(TAG, "App is not running, drop %s event", event_name(type));
        return ESP_ERR_INVALID_STATE;
    }

    const app_evt_t evt = {
        .type = type,
        .index = index,
    };
    if (xQueueSend(s_evt_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, drop %s event", event_name(type));
        return ESP_ERR_TIMEOUT;
    }

    ESP_LOGI(TAG, "Event queued: %s index=%d", event_name(type), index);
    return ESP_OK;
}

static bool post_event_for_console(app_evt_type_t type, int index, void *ctx)
{
    (void)ctx;
    return queue_event(type, index) == ESP_OK;
}

static void on_button_click(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    ESP_LOGI(TAG, "BOOT single click");
    (void)queue_event(APP_EVT_NEXT, 0);
}

static int normalize_index(int index, size_t total)
{
    if (total == 0U) {
        return 0;
    }

    index %= (int)total;
    if (index < 0) {
        index += (int)total;
    }
    return index;
}

static void show_gif(int index)
{
    const size_t total = gif_playlist_count(&s_playlist);
    if (total == 0U) {
        ESP_LOGW(TAG, "No GIF files loaded");
        app_ui_set_status(&s_ui, "%s", sd_media_state_text(s_media.state));
        return;
    }

    index = normalize_index(index, total);
    const gif_playlist_item_t *item = gif_playlist_get(&s_playlist, (size_t)index);
    if (item == NULL) {
        ESP_LOGE(TAG, "Invalid GIF index: %d", index);
        return;
    }

    esp_err_t err = bsp_display_lock(APP_LVGL_LOCK_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock display, skip switching: %s", esp_err_to_name(err));
        set_app_state(get_app_state(), err);
        return;
    }

    err = gif_player_show_locked(s_player, item);
    set_current_index(index);
    set_app_state(get_app_state(), err);
    if (err == ESP_OK) {
        app_ui_set_gif_name_locked(&s_ui, index, (int)total, item->name);
        ESP_LOGI(TAG, "Showing GIF %d/%u: %s (%u bytes, %s)",
                 index + 1,
                 (unsigned)total,
                 item->name,
                 (unsigned)item->size_bytes,
                 item->lvgl_path);
    } else {
        app_ui_set_load_failed_locked(&s_ui, item->name);
        ESP_LOGE(TAG, "Failed to start GIF %d/%u: %s (%s): %s",
                 index + 1,
                 (unsigned)total,
                 item->name,
                 item->sd_path,
                 esp_err_to_name(err));
    }
    bsp_display_unlock();
}

static esp_err_t mount_sd_and_scan_gifs(void)
{
    esp_err_t err = sd_media_mount(&s_media);
    if (err != ESP_OK) {
        return err;
    }

    gif_playlist_scan_status_t scan_status;
    err = gif_playlist_scan(&s_playlist, APP_SD_GIF_DIR, APP_LVGL_GIF_PREFIX, &scan_status);
    sd_media_apply_scan_status(&s_media, &scan_status);
    return (s_media.state == SD_MEDIA_OK) ? ESP_OK : err;
}

static void init_button(void)
{
    static button_handle_t buttons[BSP_BUTTON_NUM];
    int button_count = 0;
    esp_err_t err = bsp_iot_button_create(buttons, &button_count, BSP_BUTTON_NUM);
    if (err != ESP_OK || button_count <= BSP_BUTTON_BOOT || buttons[BSP_BUTTON_BOOT] == NULL) {
        ESP_LOGW(TAG, "Button init failed: %s", esp_err_to_name(err));
        return;
    }

    err = iot_button_register_cb(buttons[BSP_BUTTON_BOOT],
                                 BUTTON_SINGLE_CLICK, NULL,
                                 on_button_click, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BOOT button callback registration failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "BOOT button ready: single click -> next GIF");
}

static esp_err_t init_display(void)
{
    /*
     * GIF 单帧解码 + 整屏 flush 的耗时会超过帧定时器周期，lv_timer_handler()
     * 因此总是返回 0。适配器默认 task_min_delay_ms=1，在 CONFIG_FREERTOS_HZ=100
     * 下 pdMS_TO_TICKS(1)==0，vTaskDelay(0) 不会让出 CPU，高优先级(6)的 lvgl
     * 任务将饿死 CPU0 上的 app_main(优先级1) 与 IDLE0。将最小休眠提高到 10ms，
     * 保证每轮 LVGL 循环真实阻塞一次，事件循环与看门狗恢复正常。
     */
    bsp_display_cfg_t disp_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    disp_cfg.lv_adapter_cfg.task_min_delay_ms = 10;

    lv_display_t *display = bsp_display_start_with_config(&disp_cfg);
    if (display == NULL) {
        ESP_LOGE(TAG, "Failed to initialize display");
        return ESP_FAIL;
    }
    return bsp_display_brightness_set(APP_LCD_BRIGHTNESS_PERCENT);
}

static void start_console(void)
{
    const app_console_config_t console_cfg = {
        .playlist = &s_playlist,
        .media_status = &s_media,
        .player = s_player,
        .post_event = post_event_for_console,
        .get_current_index = get_current_index,
        .ctx = NULL,
    };
    (void)app_console_start(&console_cfg);
}

static void fail_app_task(esp_err_t err, const char *stage)
{
    ESP_LOGE(TAG, "GIF viewer app init failed at %s: %s", stage, esp_err_to_name(err));
    set_app_state(GIF_VIEWER_APP_STATE_FAILED, err);
    state_lock();
    s_task_handle = NULL;
    state_unlock();
    vTaskDelete(NULL);
}

#define CHECK_APP_INIT(expr, stage) do {              \
        esp_err_t check_err_ = (expr);                \
        if (check_err_ != ESP_OK) {                   \
            fail_app_task(check_err_, (stage));       \
            return;                                   \
        }                                             \
    } while (0)

static void gif_viewer_app_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Starting SD GIF player on ESP32-S3-Touch-LCD-1.85B");

    CHECK_APP_INIT(bsp_i2c_init(), "i2c");
    CHECK_APP_INIT(init_display(), "display");
    CHECK_APP_INIT(app_ui_create(&s_ui), "ui");

    const gif_player_config_t player_cfg = {
        .parent = app_ui_gif_layer(&s_ui),
    };
    CHECK_APP_INIT(gif_player_create(&player_cfg, &s_player), "gif player");

    if (s_config.enable_boot_button) {
        init_button();
    }

    app_ui_set_status(&s_ui, "mounting SD...");
    esp_err_t err = mount_sd_and_scan_gifs();
    esp_err_t startup_err = err;
    if (err == ESP_OK) {
        show_gif((int)s_config.initial_index);
        startup_err = get_last_error();
    } else {
        ESP_LOGW(TAG, "GIF source unavailable: %s", sd_media_state_text(s_media.state));
        app_ui_set_status(&s_ui, "%s", sd_media_state_text(s_media.state));
    }

    if (s_config.enable_console) {
        start_console();
    }

    set_app_state(GIF_VIEWER_APP_STATE_RUNNING, startup_err);

    app_evt_t evt;
    while (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) == pdTRUE) {
        const int current = get_current_index(NULL);
        const size_t total = gif_playlist_count(&s_playlist);
        ESP_LOGI(TAG, "Event handling: %s index=%d current=%d total=%u",
                 event_name(evt.type), evt.index, current, (unsigned)total);
        switch (evt.type) {
        case APP_EVT_NEXT:
            show_gif(current + 1);
            break;
        case APP_EVT_PREV:
            show_gif(current - 1);
            break;
        case APP_EVT_GOTO:
            show_gif(evt.index);
            break;
        default:
            break;
        }
    }
}

static esp_err_t validate_config(const gif_viewer_app_config_t *config)
{
    if (config->event_queue_length == 0U ||
        config->task_stack_size == 0U ||
        config->task_priority >= configMAX_PRIORITIES ||
        config->initial_index > (size_t)INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->task_core_id < -1 || config->task_core_id >= portNUM_PROCESSORS) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t gif_viewer_app_start(const gif_viewer_app_config_t *config)
{
    if (s_evt_queue != NULL || get_app_state() != GIF_VIEWER_APP_STATE_STOPPED) {
        return ESP_ERR_INVALID_STATE;
    }

    s_config = (config != NULL) ? *config : (gif_viewer_app_config_t)GIF_VIEWER_APP_DEFAULT_CONFIG();
    esp_err_t err = validate_config(&s_config);
    if (err != ESP_OK) {
        return err;
    }

    gif_playlist_init(&s_playlist);
    sd_media_status_init(&s_media);
    s_player = NULL;
    s_cur_index = -1;

    s_evt_queue = xQueueCreate((UBaseType_t)s_config.event_queue_length, sizeof(app_evt_t));
    if (s_evt_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_state_lock = xSemaphoreCreateMutex();
    if (s_state_lock == NULL) {
        vQueueDelete(s_evt_queue);
        s_evt_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    set_app_state(GIF_VIEWER_APP_STATE_STARTING, ESP_OK);

    const BaseType_t task_ret = xTaskCreatePinnedToCore(gif_viewer_app_task,
                                                        GIF_VIEWER_APP_TASK_NAME,
                                                        s_config.task_stack_size,
                                                        NULL,
                                                        (UBaseType_t)s_config.task_priority,
                                                        &s_task_handle,
                                                        (s_config.task_core_id < 0) ?
                                                        tskNO_AFFINITY : s_config.task_core_id);
    if (task_ret != pdPASS) {
        set_app_state(GIF_VIEWER_APP_STATE_STOPPED, ESP_ERR_NO_MEM);
        vSemaphoreDelete(s_state_lock);
        s_state_lock = NULL;
        vQueueDelete(s_evt_queue);
        s_evt_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t gif_viewer_app_next(void)
{
    return queue_event(APP_EVT_NEXT, 0);
}

esp_err_t gif_viewer_app_prev(void)
{
    return queue_event(APP_EVT_PREV, 0);
}

esp_err_t gif_viewer_app_show_index(size_t zero_based_index)
{
    if (zero_based_index > (size_t)INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return queue_event(APP_EVT_GOTO, (int)zero_based_index);
}

static gif_viewer_app_media_state_t map_media_state(sd_media_state_t state)
{
    switch (state) {
    case SD_MEDIA_OK:
        return GIF_VIEWER_APP_MEDIA_READY;
    case SD_MEDIA_SD_MOUNT_FAILED:
        return GIF_VIEWER_APP_MEDIA_SD_MOUNT_FAILED;
    case SD_MEDIA_GIF_DIR_MISSING:
        return GIF_VIEWER_APP_MEDIA_GIF_DIR_MISSING;
    case SD_MEDIA_NO_GIFS:
        return GIF_VIEWER_APP_MEDIA_NO_GIFS;
    case SD_MEDIA_SCAN_FAILED:
    default:
        return GIF_VIEWER_APP_MEDIA_SCAN_FAILED;
    }
}

esp_err_t gif_viewer_app_get_status(gif_viewer_app_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));
    status->state = get_app_state();
    status->last_error = get_last_error();
    status->media_state = map_media_state(s_media.state);
    status->gif_count = gif_playlist_count(&s_playlist);
    status->current_index = get_current_index(NULL);

    if (status->current_index >= 0 &&
        (size_t)status->current_index < status->gif_count) {
        const gif_playlist_item_t *item =
            gif_playlist_get(&s_playlist, (size_t)status->current_index);
        if (item != NULL) {
            copy_text(status->current_name, sizeof(status->current_name), item->name);
        }
    }

    gif_player_status_t player_status;
    gif_player_get_status(s_player, &player_status);
    copy_text(status->source_path, sizeof(status->source_path), player_status.lvgl_path);
    status->width = player_status.width;
    status->height = player_status.height;
    status->frame_index = player_status.frame_index;
    status->file_size_bytes = player_status.file_size_bytes;
    status->frame_data_size_bytes = player_status.frame_data_size_bytes;
    status->internal_free_bytes = player_status.internal_free_bytes;
    status->internal_largest_bytes = player_status.internal_largest_bytes;
    status->psram_free_bytes = player_status.psram_free_bytes;
    status->psram_largest_bytes = player_status.psram_largest_bytes;
    return ESP_OK;
}

/*
 * GIF 相册播放器
 *
 * 功能:
 *  - 开机从 SD 卡 /sdcard/assets/gif 目录扫描 GIF 文件并播放第一张
 *  - BOOT 按键单击切换下一张，播放到最后一张后循环回第一张
 *  - 通过 dbg_console 串口调试助手提供 next/prev/goto/list/status 指令
 */

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
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

static const char *TAG = "gif_app";

static QueueHandle_t s_evt_queue;
static SemaphoreHandle_t s_state_lock;
static gif_playlist_t s_playlist;
static sd_media_status_t s_media;
static app_ui_t s_ui;
static gif_player_t *s_player;
static int s_cur_index;

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

static void set_current_index(int index)
{
    if (s_state_lock != NULL) {
        (void)xSemaphoreTake(s_state_lock, portMAX_DELAY);
    }
    s_cur_index = index;
    if (s_state_lock != NULL) {
        xSemaphoreGive(s_state_lock);
    }
}

static int get_current_index(void *ctx)
{
    (void)ctx;

    int index;
    if (s_state_lock != NULL) {
        (void)xSemaphoreTake(s_state_lock, portMAX_DELAY);
    }
    index = s_cur_index;
    if (s_state_lock != NULL) {
        xSemaphoreGive(s_state_lock);
    }
    return index;
}

static bool post_event(app_evt_type_t type, int index, void *ctx)
{
    (void)ctx;

    if (s_evt_queue == NULL) {
        ESP_LOGW(TAG, "Event queue is not ready, drop %s event", event_name(type));
        return false;
    }

    const app_evt_t evt = {
        .type = type,
        .index = index,
    };
    if (xQueueSend(s_evt_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, drop %s event", event_name(type));
        return false;
    }

    ESP_LOGI(TAG, "Event queued: %s index=%d", event_name(type), index);
    return true;
}

static void on_button_click(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    ESP_LOGI(TAG, "BOOT single click");
    (void)post_event(APP_EVT_NEXT, 0, NULL);
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
        return;
    }

    err = gif_player_show_locked(s_player, item);
    set_current_index(index);
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
    if (err == ESP_OK && button_count > BSP_BUTTON_BOOT && buttons[BSP_BUTTON_BOOT] != NULL) {
        ESP_ERROR_CHECK(iot_button_register_cb(buttons[BSP_BUTTON_BOOT],
                                               BUTTON_SINGLE_CLICK, NULL,
                                               on_button_click, NULL));
        ESP_LOGI(TAG, "BOOT button ready: single click -> next GIF");
    } else {
        ESP_LOGW(TAG, "Button init failed: %s", esp_err_to_name(err));
    }
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
        .post_event = post_event,
        .get_current_index = get_current_index,
        .ctx = NULL,
    };
    (void)app_console_start(&console_cfg);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Starting SD GIF player on ESP32-S3-Touch-LCD-1.85B");

    gif_playlist_init(&s_playlist);
    sd_media_status_init(&s_media);

    s_evt_queue = xQueueCreate(8, sizeof(app_evt_t));
    assert(s_evt_queue != NULL);
    s_state_lock = xSemaphoreCreateMutex();
    assert(s_state_lock != NULL);

    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(init_display());
    ESP_ERROR_CHECK(app_ui_create(&s_ui));

    const gif_player_config_t player_cfg = {
        .parent = app_ui_gif_layer(&s_ui),
    };
    ESP_ERROR_CHECK(gif_player_create(&player_cfg, &s_player));

    init_button();

    app_ui_set_status(&s_ui, "mounting SD...");
    esp_err_t err = mount_sd_and_scan_gifs();
    if (err == ESP_OK) {
        show_gif(0);
    } else {
        ESP_LOGW(TAG, "GIF source unavailable: %s", sd_media_state_text(s_media.state));
        app_ui_set_status(&s_ui, "%s", sd_media_state_text(s_media.state));
    }

    start_console();

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

/*
 * GIF 相册播放器
 *
 * 功能:
 *  - 开机自动从 assets 分区 (由 assets/gif 目录打包) 播放第一张 GIF
 *  - BOOT 按键单击切换下一张，播放到最后一张后循环回第一张
 *  - 通过 dbg_console 串口调试助手提供 next/prev/goto/list 调试指令
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"

#include "bsp/esp-bsp.h"
#include "esp_mmap_assets.h"
#include "esp_lv_fs.h"
#include "mmap_generate_assets.h"

#include "dbg_console.h"

static const char *TAG = "gif_player";

#define APP_LCD_BRIGHTNESS_PERCENT 80
#define APP_ASSETS_PARTITION_LABEL "assets"
#define APP_LVGL_FS_LETTER         'A'
#define APP_LVGL_LOCK_TIMEOUT_MS   -1  /* 阻塞等待: GIF 解码期间 LVGL 任务可能长时间持锁 */

/* 图片切换事件 */
typedef enum {
    APP_EVT_NEXT,
    APP_EVT_PREV,
    APP_EVT_GOTO,
} app_evt_type_t;

typedef struct {
    app_evt_type_t type;
    int index;
} app_evt_t;

static QueueHandle_t s_evt_queue;
static mmap_assets_handle_t s_assets;
static esp_lv_fs_handle_t s_fs;
static lv_obj_t *s_gif;
static lv_obj_t *s_name_label;
static int s_cur_index;
static int s_total;

/* ---------------------------------------------------------------------------
 * GIF 显示
 * ------------------------------------------------------------------------- */

static void show_gif(int index)
{
    /* 归一化到 [0, s_total) 实现循环 */
    index %= s_total;
    if (index < 0) {
        index += s_total;
    }
    s_cur_index = index;

    const char *name = mmap_assets_get_name(s_assets, index);
    int size = mmap_assets_get_size(s_assets, index);

    /* lv_gif 持有路径期间需要保证缓冲区有效，使用静态缓冲区 */
    static char path[64];
    snprintf(path, sizeof(path), "%c:%s", APP_LVGL_FS_LETTER, name);

    if (bsp_display_lock(APP_LVGL_LOCK_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock display, skip switching");
        return;
    }
    lv_gif_set_src(s_gif, path);
    lv_label_set_text_fmt(s_name_label, "%d/%d %s", index + 1, s_total, name);
    bsp_display_unlock();

    ESP_LOGI(TAG, "Showing GIF %d/%d: %s (%d bytes)", index + 1, s_total, name, size);
}

static void create_ui(void)
{
    /* timeout < 0 表示一直等待 */
    ESP_ERROR_CHECK(bsp_display_lock(-1));

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    s_gif = lv_gif_create(screen);
    lv_obj_center(s_gif);

    s_name_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_name_label, lv_color_white(), 0);
    lv_obj_align(s_name_label, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_label_set_text(s_name_label, "");

    bsp_display_unlock();
}

/* ---------------------------------------------------------------------------
 * 资产分区挂载: assets 分区 -> LVGL 文件系统盘符 'A'
 * ------------------------------------------------------------------------- */

static esp_err_t mount_assets(void)
{
    const mmap_assets_config_t asset_cfg = {
        .partition_label = APP_ASSETS_PARTITION_LABEL,
        .max_files = MMAP_ASSETS_FILES,
        .checksum = MMAP_ASSETS_CHECKSUM,
        .flags = {
            .mmap_enable = true,
        },
    };
    esp_err_t err = mmap_assets_new(&asset_cfg, &s_assets);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount assets partition: %s", esp_err_to_name(err));
        return err;
    }

    const fs_cfg_t fs_cfg = {
        .fs_letter = APP_LVGL_FS_LETTER,
        .fs_assets = s_assets,
        .fs_nums = MMAP_ASSETS_FILES,
    };
    err = esp_lv_fs_desc_init(&fs_cfg, &s_fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init LVGL FS: %s", esp_err_to_name(err));
        return err;
    }

    s_total = MMAP_ASSETS_FILES;
    ESP_LOGI(TAG, "Assets mounted: %d file(s) on drive %c:", s_total, APP_LVGL_FS_LETTER);
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * 输入: 按键 + 串口调试命令，统一投递到事件队列
 * ------------------------------------------------------------------------- */

static void post_event(app_evt_type_t type, int index)
{
    app_evt_t evt = { .type = type, .index = index };
    if (xQueueSend(s_evt_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, drop event");
    }
}

static void on_button_click(void *button_handle, void *usr_data)
{
    post_event(APP_EVT_NEXT, 0);
}

static int cmd_next(int argc, char **argv)
{
    post_event(APP_EVT_NEXT, 0);
    return 0;
}

static int cmd_prev(int argc, char **argv)
{
    post_event(APP_EVT_PREV, 0);
    return 0;
}

static int cmd_goto(int argc, char **argv)
{
    if (argc != 2) {
        dbg_console_printf("usage: goto <1..%d>\n", s_total);
        return 1;
    }
    int index = atoi(argv[1]);
    if (index < 1 || index > s_total) {
        dbg_console_printf("index out of range: %s (valid: 1..%d)\n", argv[1], s_total);
        return 1;
    }
    post_event(APP_EVT_GOTO, index - 1);
    return 0;
}

static int cmd_list(int argc, char **argv)
{
    for (int i = 0; i < s_total; i++) {
        dbg_console_printf("%c %2d: %-16s %7d bytes\n",
                           (i == s_cur_index) ? '*' : ' ',
                           i + 1,
                           mmap_assets_get_name(s_assets, i),
                           mmap_assets_get_size(s_assets, i));
    }
    return 0;
}

static void register_app_cmds(void)
{
    dbg_console_register_cmd("next", "Show next GIF", NULL, cmd_next);
    dbg_console_register_cmd("prev", "Show previous GIF", NULL, cmd_prev);
    dbg_console_register_cmd("goto", "Jump to GIF by index", "<index>", cmd_goto);
    dbg_console_register_cmd("list", "List all GIF assets", NULL, cmd_list);
}

/* ---------------------------------------------------------------------------
 * app_main
 * ------------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "Starting GIF player on ESP32-S3-Touch-LCD-1.85B");

    s_evt_queue = xQueueCreate(8, sizeof(app_evt_t));
    assert(s_evt_queue != NULL);

    /* 1. 板级初始化: I2C + 显示 (含 LVGL) */
    ESP_ERROR_CHECK(bsp_i2c_init());

    lv_display_t *display = bsp_display_start();
    if (display == NULL) {
        ESP_LOGE(TAG, "Failed to initialize display");
        return;
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(APP_LCD_BRIGHTNESS_PERCENT));

    /* 2. 挂载 GIF 资产分区并注册 LVGL 文件系统 */
    if (mount_assets() != ESP_OK) {
        ESP_LOGE(TAG, "Asset mount failed, stop");
        return;
    }

    /* 3. 创建 UI 并显示第一张 GIF */
    create_ui();
    show_gif(0);

    /* 4. BOOT 按键: 单击切换下一张 */
    static button_handle_t buttons[BSP_BUTTON_NUM];
    int button_count = 0;
    esp_err_t err = bsp_iot_button_create(buttons, &button_count, BSP_BUTTON_NUM);
    if (err == ESP_OK && button_count > 0) {
        ESP_ERROR_CHECK(iot_button_register_cb(buttons[BSP_BUTTON_BOOT],
                                               BUTTON_SINGLE_CLICK, NULL,
                                               on_button_click, NULL));
        ESP_LOGI(TAG, "BOOT button ready: single click -> next GIF");
    } else {
        ESP_LOGW(TAG, "Button init failed: %s", esp_err_to_name(err));
    }

    /* 5. 启动串口调试助手并注册业务命令 */
    ESP_ERROR_CHECK(dbg_console_start(NULL));
    register_app_cmds();

    /* 6. 事件循环: 统一处理按键/串口触发的切图请求 */
    app_evt_t evt;
    while (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) == pdTRUE) {
        switch (evt.type) {
        case APP_EVT_NEXT:
            show_gif(s_cur_index + 1);
            break;
        case APP_EVT_PREV:
            show_gif(s_cur_index - 1);
            break;
        case APP_EVT_GOTO:
            show_gif(evt.index);
            break;
        default:
            break;
        }
    }
}

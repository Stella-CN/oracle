/*
 * GIF 相册播放器
 *
 * 功能:
 *  - 开机从 SD 卡 /sdcard/assets/gif 目录扫描 GIF 文件并播放第一张
 *  - BOOT 按键单击切换下一张，播放到最后一张后循环回第一张
 *  - 通过 dbg_console 串口调试助手提供 next/prev/goto/list 调试指令
 */

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"

#include "bsp/esp-bsp.h"

#include "dbg_console.h"

static const char *TAG = "gif_player";

#define APP_LCD_BRIGHTNESS_PERCENT 80
#define APP_SD_GIF_SUBDIR          "assets/gif"
#define APP_SD_GIF_DIR             BSP_SD_MOUNT_POINT "/" APP_SD_GIF_SUBDIR
#define APP_LVGL_FS_LETTER         'S'
#define APP_LVGL_LOCK_TIMEOUT_MS   -1
#define APP_LVGL_STDIO_PATH_MAX    256
#define APP_SCAN_PATH_MAX          320

typedef enum {
    APP_EVT_NEXT,
    APP_EVT_PREV,
    APP_EVT_GOTO,
} app_evt_type_t;

typedef struct {
    app_evt_type_t type;
    int index;
} app_evt_t;

typedef struct {
    char *name;
    char *lv_src;
    size_t size;
} app_gif_t;

typedef enum {
    APP_MEDIA_OK = 0,
    APP_MEDIA_SD_MOUNT_FAILED,
    APP_MEDIA_GIF_DIR_MISSING,
    APP_MEDIA_NO_GIFS,
    APP_MEDIA_SCAN_FAILED,
} app_media_state_t;

static QueueHandle_t s_evt_queue;
static app_gif_t *s_gifs;
static lv_obj_t *s_gif;
static lv_obj_t *s_name_label;
static int s_cur_index;
static int s_total;
static app_media_state_t s_media_state = APP_MEDIA_SD_MOUNT_FAILED;
static esp_err_t s_last_sd_err = ESP_OK;
static int s_last_scan_errno;

/* ---------------------------------------------------------------------------
 * GIF 列表扫描
 * ------------------------------------------------------------------------- */

static char *app_strdup(const char *src)
{
    size_t len = strlen(src) + 1;
    char *copy = malloc(len);
    if (copy != NULL) {
        memcpy(copy, src, len);
    }
    return copy;
}

static void clear_gif_list(void)
{
    for (int i = 0; i < s_total; i++) {
        free(s_gifs[i].name);
        free(s_gifs[i].lv_src);
    }
    free(s_gifs);
    s_gifs = NULL;
    s_cur_index = 0;
    s_total = 0;
}

static bool is_gif_name(const char *name)
{
    const char *ext = strrchr(name, '.');
    return ext != NULL && strcasecmp(ext, ".gif") == 0;
}

static bool build_full_sd_path(const char *name, char *path, size_t path_len)
{
    int written = snprintf(path, path_len, "%s/%s", APP_SD_GIF_DIR, name);
    if (written < 0 || (size_t)written >= path_len) {
        ESP_LOGW(TAG, "Skip '%s': full path is too long", name);
        return false;
    }

    if ((size_t)written >= APP_LVGL_STDIO_PATH_MAX) {
        ESP_LOGW(TAG, "Skip '%s': path exceeds LVGL stdio limit", name);
        return false;
    }

    return true;
}

static esp_err_t add_gif_file(const char *name, size_t size)
{
    char full_path[APP_SCAN_PATH_MAX];
    if (!build_full_sd_path(name, full_path, sizeof(full_path))) {
        return ESP_OK;
    }

    size_t name_len = strlen(name);
    char *name_copy = app_strdup(name);
    if (name_copy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char *lv_src = malloc(name_len + 3);
    if (lv_src == NULL) {
        free(name_copy);
        return ESP_ERR_NO_MEM;
    }
    snprintf(lv_src, name_len + 3, "%c:%s", APP_LVGL_FS_LETTER, name);

    app_gif_t *new_list = realloc(s_gifs, (size_t)(s_total + 1) * sizeof(*s_gifs));
    if (new_list == NULL) {
        free(name_copy);
        free(lv_src);
        return ESP_ERR_NO_MEM;
    }

    s_gifs = new_list;
    s_gifs[s_total].name = name_copy;
    s_gifs[s_total].lv_src = lv_src;
    s_gifs[s_total].size = size;
    s_total++;
    return ESP_OK;
}

static int compare_gif_by_name(const void *lhs, const void *rhs)
{
    const app_gif_t *a = lhs;
    const app_gif_t *b = rhs;
    int ret = strcasecmp(a->name, b->name);
    return (ret != 0) ? ret : strcmp(a->name, b->name);
}

static esp_err_t scan_sd_gifs(void)
{
    clear_gif_list();

    errno = 0;
    DIR *dir = opendir(APP_SD_GIF_DIR);
    if (dir == NULL) {
        s_last_scan_errno = errno;
        if (s_last_scan_errno == ENOENT) {
            s_media_state = APP_MEDIA_GIF_DIR_MISSING;
            ESP_LOGE(TAG, "GIF directory missing: %s (create %s at SD card root)",
                     APP_SD_GIF_DIR, APP_SD_GIF_SUBDIR);
            return ESP_ERR_NOT_FOUND;
        }

        s_media_state = APP_MEDIA_SCAN_FAILED;
        ESP_LOGE(TAG, "Failed to open GIF directory '%s': errno=%d (%s)",
                 APP_SD_GIF_DIR, s_last_scan_errno, strerror(s_last_scan_errno));
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_gif_name(entry->d_name)) {
            continue;
        }

        char full_path[APP_SCAN_PATH_MAX];
        if (!build_full_sd_path(entry->d_name, full_path, sizeof(full_path))) {
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) != 0) {
            ESP_LOGW(TAG, "Skip '%s': stat failed, errno=%d", entry->d_name, errno);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            continue;
        }

        ret = add_gif_file(entry->d_name, (st.st_size > 0) ? (size_t)st.st_size : 0);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add GIF '%s': %s", entry->d_name, esp_err_to_name(ret));
            break;
        }
    }

    closedir(dir);

    if (ret != ESP_OK) {
        s_media_state = APP_MEDIA_SCAN_FAILED;
        clear_gif_list();
        return ret;
    }

    if (s_total == 0) {
        s_media_state = APP_MEDIA_NO_GIFS;
        ESP_LOGW(TAG, "No GIF files found in %s", APP_SD_GIF_DIR);
        return ESP_ERR_NOT_FOUND;
    }

    qsort(s_gifs, (size_t)s_total, sizeof(*s_gifs), compare_gif_by_name);
    s_media_state = APP_MEDIA_OK;
    ESP_LOGI(TAG, "Found %d GIF file(s) in %s", s_total, APP_SD_GIF_DIR);
    return ESP_OK;
}

static esp_err_t mount_sdmmc_with_width(uint8_t bus_width)
{
    sdmmc_host_t host = {0};
    sdmmc_slot_config_t slot = {0};
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024,
    };

    bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &host);
    bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &slot);
    slot.width = bus_width;

    bsp_sdcard_cfg_t cfg = {
        .mount = &mount_config,
        .host = &host,
        .slot.sdmmc = &slot,
    };

    return bsp_sdcard_sdmmc_mount(&cfg);
}

static esp_err_t mount_sd_card(void)
{
    esp_err_t err = mount_sdmmc_with_width(4);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted at %s (SDMMC 4-bit)", BSP_SD_MOUNT_POINT);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "SDMMC 4-bit mount failed: %s, retrying 1-bit", esp_err_to_name(err));

    err = mount_sdmmc_with_width(1);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted at %s (SDMMC 1-bit fallback)", BSP_SD_MOUNT_POINT);
        return ESP_OK;
    }

    s_last_sd_err = err;
    s_media_state = APP_MEDIA_SD_MOUNT_FAILED;
    ESP_LOGE(TAG,
             "Failed to mount SD card: %s. Use a FAT32-formatted card; "
             "exFAT is not enabled in this firmware. Expected directory: /%s",
             esp_err_to_name(err), APP_SD_GIF_SUBDIR);
    return err;
}

static esp_err_t mount_sd_and_scan_gifs(void)
{
    s_media_state = APP_MEDIA_SD_MOUNT_FAILED;
    s_last_sd_err = ESP_OK;
    s_last_scan_errno = 0;

    esp_err_t err = mount_sd_card();
    if (err != ESP_OK) {
        return err;
    }

    s_media_state = APP_MEDIA_SCAN_FAILED;
    return scan_sd_gifs();
}

/* ---------------------------------------------------------------------------
 * UI / GIF 显示
 * ------------------------------------------------------------------------- */

static const char *media_status_text(void)
{
    switch (s_media_state) {
    case APP_MEDIA_OK:
        return "ready";
    case APP_MEDIA_SD_MOUNT_FAILED:
        return "SD mount failed: use FAT32";
    case APP_MEDIA_GIF_DIR_MISSING:
        return "missing /assets/gif";
    case APP_MEDIA_NO_GIFS:
        return "no GIF in /assets/gif";
    case APP_MEDIA_SCAN_FAILED:
    default:
        return "SD scan failed";
    }
}

static void print_media_status(void)
{
    dbg_console_printf("%s\n", media_status_text());

    switch (s_media_state) {
    case APP_MEDIA_SD_MOUNT_FAILED:
        dbg_console_printf("last SD error: %s\n", esp_err_to_name(s_last_sd_err));
        dbg_console_printf("format the SD card as FAT32; exFAT is not enabled in this firmware\n");
        break;
    case APP_MEDIA_GIF_DIR_MISSING:
        dbg_console_printf("create %s at the SD card root and copy .gif files into it\n",
                           APP_SD_GIF_SUBDIR);
        break;
    case APP_MEDIA_NO_GIFS:
        dbg_console_printf("copy .gif files into %s\n", APP_SD_GIF_DIR);
        break;
    case APP_MEDIA_SCAN_FAILED:
        dbg_console_printf("scan errno: %d (%s)\n",
                           s_last_scan_errno, strerror(s_last_scan_errno));
        break;
    case APP_MEDIA_OK:
    default:
        break;
    }
}

static void set_status_text_locked(const char *text)
{
    if (s_name_label == NULL) {
        return;
    }

    lv_label_set_text(s_name_label, text);
    lv_obj_align(s_name_label, LV_ALIGN_BOTTOM_MID, 0, -18);
}

static void set_status_text(const char *fmt, ...)
{
    char text[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);

    if (bsp_display_lock(APP_LVGL_LOCK_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock display, skip status update");
        return;
    }
    set_status_text_locked(text);
    bsp_display_unlock();
}

static void show_gif(int index)
{
    if (s_total <= 0) {
        ESP_LOGW(TAG, "No GIF files loaded");
        set_status_text("%s", media_status_text());
        return;
    }

    index %= s_total;
    if (index < 0) {
        index += s_total;
    }
    s_cur_index = index;

    const app_gif_t *gif = &s_gifs[index];
    if (bsp_display_lock(APP_LVGL_LOCK_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock display, skip switching");
        return;
    }

    lv_gif_set_src(s_gif, gif->lv_src);
    lv_obj_align(s_gif, LV_ALIGN_CENTER, 0, -12);

    if (lv_gif_is_loaded(s_gif)) {
        lv_label_set_text_fmt(s_name_label, "%d/%d %s", index + 1, s_total, gif->name);
        ESP_LOGI(TAG, "Showing GIF %d/%d: %s (%u bytes)",
                 index + 1, s_total, gif->name, (unsigned)gif->size);
    } else {
        lv_label_set_text_fmt(s_name_label, "load failed: %s", gif->name);
        ESP_LOGE(TAG, "Failed to load GIF %d/%d: %s (%s)",
                 index + 1, s_total, gif->name, gif->lv_src);
    }
    lv_obj_align(s_name_label, LV_ALIGN_BOTTOM_MID, 0, -18);
    bsp_display_unlock();
}

static void create_ui(void)
{
    ESP_ERROR_CHECK(bsp_display_lock(APP_LVGL_LOCK_TIMEOUT_MS));

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);

    s_gif = lv_gif_create(screen);
    lv_obj_align(s_gif, LV_ALIGN_CENTER, 0, -12);

    s_name_label = lv_label_create(screen);
    lv_obj_set_style_text_color(s_name_label, lv_color_white(), 0);
    lv_obj_set_style_text_align(s_name_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_name_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_name_label, LV_PCT(95));
    set_status_text_locked("starting...");

    bsp_display_unlock();
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
    (void)button_handle;
    (void)usr_data;
    post_event(APP_EVT_NEXT, 0);
}

static int cmd_next(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_total <= 0) {
        print_media_status();
        return 1;
    }
    post_event(APP_EVT_NEXT, 0);
    return 0;
}

static int cmd_prev(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_total <= 0) {
        print_media_status();
        return 1;
    }
    post_event(APP_EVT_PREV, 0);
    return 0;
}

static int cmd_goto(int argc, char **argv)
{
    if (s_total <= 0) {
        print_media_status();
        return 1;
    }

    if (argc != 2) {
        dbg_console_printf("usage: goto <1..%d>\n", s_total);
        return 1;
    }

    errno = 0;
    char *end = NULL;
    long index = strtol(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0') {
        dbg_console_printf("invalid index: %s\n", argv[1]);
        return 1;
    }

    if (index < 1 || index > s_total) {
        dbg_console_printf("index out of range: %s (valid: 1..%d)\n", argv[1], s_total);
        return 1;
    }
    post_event(APP_EVT_GOTO, (int)index - 1);
    return 0;
}

static int cmd_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (s_total <= 0) {
        print_media_status();
        return 1;
    }

    for (int i = 0; i < s_total; i++) {
        dbg_console_printf("%c %2d: %-24s %7u bytes\n",
                           (i == s_cur_index) ? '*' : ' ',
                           i + 1,
                           s_gifs[i].name,
                           (unsigned)s_gifs[i].size);
    }
    return 0;
}

static esp_err_t register_app_cmds(void *user_ctx)
{
    (void)user_ctx;

    esp_err_t ret = ESP_OK;
    esp_err_t err = dbg_console_register_cmd("next", "Show next GIF", NULL, cmd_next);
    if (err != ESP_OK && ret == ESP_OK) {
        ret = err;
    }
    err = dbg_console_register_cmd("prev", "Show previous GIF", NULL, cmd_prev);
    if (err != ESP_OK && ret == ESP_OK) {
        ret = err;
    }
    err = dbg_console_register_cmd("goto", "Jump to GIF by index", "<index>", cmd_goto);
    if (err != ESP_OK && ret == ESP_OK) {
        ret = err;
    }
    err = dbg_console_register_cmd("list", "List all GIF assets", NULL, cmd_list);
    if (err != ESP_OK && ret == ESP_OK) {
        ret = err;
    }
    return ret;
}

/* ---------------------------------------------------------------------------
 * app_main
 * ------------------------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "Starting SD GIF player on ESP32-S3-Touch-LCD-1.85B");

    s_evt_queue = xQueueCreate(8, sizeof(app_evt_t));
    assert(s_evt_queue != NULL);

    ESP_ERROR_CHECK(bsp_i2c_init());

    lv_display_t *display = bsp_display_start();
    if (display == NULL) {
        ESP_LOGE(TAG, "Failed to initialize display");
        return;
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(APP_LCD_BRIGHTNESS_PERCENT));

    create_ui();

    esp_err_t err = mount_sd_and_scan_gifs();
    if (err == ESP_OK) {
        show_gif(0);
    } else {
        ESP_LOGW(TAG, "GIF source unavailable: %s", media_status_text());
        set_status_text("%s", media_status_text());
    }

    static button_handle_t buttons[BSP_BUTTON_NUM];
    int button_count = 0;
    err = bsp_iot_button_create(buttons, &button_count, BSP_BUTTON_NUM);
    if (err == ESP_OK && button_count > BSP_BUTTON_BOOT && buttons[BSP_BUTTON_BOOT] != NULL) {
        ESP_ERROR_CHECK(iot_button_register_cb(buttons[BSP_BUTTON_BOOT],
                                               BUTTON_SINGLE_CLICK, NULL,
                                               on_button_click, NULL));
        ESP_LOGI(TAG, "BOOT button ready: single click -> next GIF");
    } else {
        ESP_LOGW(TAG, "Button init failed: %s", esp_err_to_name(err));
    }

    const dbg_console_config_t console_cfg = {
        .prompt = "dbg> ",
        .max_cmdline_length = 256,
        .register_system_cmds = true,
        .register_user_cmds = register_app_cmds,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(dbg_console_start(&console_cfg));

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

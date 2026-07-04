/*
 * GIF 相册播放器
 *
 * 功能:
 *  - 开机从 SD 卡 /sdcard/assets/gif 目录扫描 GIF 文件并播放第一张
 *  - BOOT 按键单击切换下一张，播放到最后一张后循环回第一张
 *  - 通过 dbg_console 串口调试助手提供 next/prev/goto/list 调试指令
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_log.h"

#include "bsp/esp-bsp.h"
#include "libs/gif/AnimatedGIF.h"
#include "misc/cache/instance/lv_image_cache.h"

#include "dbg_console.h"

static const char *TAG = "gif_player";

#define APP_LCD_BRIGHTNESS_PERCENT 80
#define APP_SD_GIF_SUBDIR          "assets/gif"
#define APP_SD_GIF_DIR             BSP_SD_MOUNT_POINT "/" APP_SD_GIF_SUBDIR
#define APP_LVGL_LOCK_TIMEOUT_MS   -1
#define APP_SCAN_PATH_MAX          320
#define APP_SD_HINT_MAX            160U
#define APP_GIF_LOAD_CHUNK_SIZE    (16U * 1024U)
#define APP_GIF_MIN_FRAME_DELAY_MS 20U

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
    char *path;
    size_t size;
} app_gif_t;

typedef enum {
    APP_MEDIA_OK = 0,
    APP_MEDIA_SD_MOUNT_FAILED,
    APP_MEDIA_GIF_DIR_MISSING,
    APP_MEDIA_NO_GIFS,
    APP_MEDIA_SCAN_FAILED,
} app_media_state_t;

typedef struct {
    int x;
    int y;
    int w;
    int h;
    uint8_t disposal_method;
    uint16_t bg_color;
} app_gif_frame_state_t;

typedef struct {
    GIFIMAGE gif;
    lv_obj_t *image;
    lv_timer_t *timer;
    lv_image_dsc_t frame_dsc;
    uint8_t *source_data;
    size_t source_size;
    uint16_t *frame_data;
    size_t frame_data_size;
    int width;
    int height;
    int frame_index;
    bool open;
    bool have_frame_state;
    bool frame_drawn;
    app_gif_frame_state_t frame_state;
} app_gif_player_t;

static QueueHandle_t s_evt_queue;
static app_gif_t *s_gifs;
static lv_obj_t *s_gif_layer;
static lv_obj_t *s_name_label;
static app_gif_player_t s_gif_player;
static int s_cur_index;
static int s_total;
static app_media_state_t s_media_state = APP_MEDIA_SD_MOUNT_FAILED;
static esp_err_t s_last_sd_err = ESP_OK;
static int s_last_scan_errno;
static char s_last_sd_hint[APP_SD_HINT_MAX];
static uint8_t s_sd_bus_width;

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
        free(s_gifs[i].path);
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

    return true;
}

static esp_err_t add_gif_file(const char *name, size_t size)
{
    char full_path[APP_SCAN_PATH_MAX];
    if (!build_full_sd_path(name, full_path, sizeof(full_path))) {
        return ESP_OK;
    }

    char *name_copy = app_strdup(name);
    if (name_copy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char *path_copy = app_strdup(full_path);
    if (path_copy == NULL) {
        free(name_copy);
        return ESP_ERR_NO_MEM;
    }

    app_gif_t *new_list = realloc(s_gifs, (size_t)(s_total + 1) * sizeof(*s_gifs));
    if (new_list == NULL) {
        free(name_copy);
        free(path_copy);
        return ESP_ERR_NO_MEM;
    }

    s_gifs = new_list;
    s_gifs[s_total].name = name_copy;
    s_gifs[s_total].path = path_copy;
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

static void set_last_sd_hint(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(s_last_sd_hint, sizeof(s_last_sd_hint), fmt, args);
    va_end(args);
}

static esp_err_t mount_sdmmc_with_width(uint8_t bus_width)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    esp_err_t err = bsp_sdcard_mount_with_width(bus_width, &mount_config);
    if (err != ESP_OK) {
        set_last_sd_hint("SDMMC %u-bit mount failed through BSP helper: %s",
                         bus_width, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t mount_sd_card(void)
{
    s_last_sd_hint[0] = '\0';
    s_sd_bus_width = 0;

    esp_err_t err = mount_sdmmc_with_width(4);
    if (err == ESP_OK) {
        s_sd_bus_width = 4;
        ESP_LOGI(TAG, "SD card mounted at %s (SDMMC 4-bit)", BSP_SD_MOUNT_POINT);
        return ESP_OK;
    }
    const esp_err_t four_bit_err = err;
    char four_bit_hint[APP_SD_HINT_MAX];
    snprintf(four_bit_hint, sizeof(four_bit_hint), "%s",
             s_last_sd_hint[0] ? s_last_sd_hint : "no detail");

    ESP_LOGW(TAG, "SDMMC 4-bit mount failed: %s (%s), retrying 1-bit",
             esp_err_to_name(err),
             four_bit_hint);

    err = mount_sdmmc_with_width(1);
    if (err == ESP_OK) {
        s_sd_bus_width = 1;
        ESP_LOGI(TAG, "SD card mounted at %s (SDMMC 1-bit fallback)", BSP_SD_MOUNT_POINT);
        return ESP_OK;
    }
    char one_bit_hint[APP_SD_HINT_MAX];
    snprintf(one_bit_hint, sizeof(one_bit_hint), "%s",
             s_last_sd_hint[0] ? s_last_sd_hint : "no detail");

    s_last_sd_err = err;
    s_media_state = APP_MEDIA_SD_MOUNT_FAILED;
    set_last_sd_hint("4-bit: %s (%s); 1-bit: %s (%s). Use a FAT/FAT32 card with /%s",
                     esp_err_to_name(four_bit_err), four_bit_hint,
                     esp_err_to_name(err), one_bit_hint,
                     APP_SD_GIF_SUBDIR);
    ESP_LOGE(TAG,
             "Failed to mount SD card. %s",
             s_last_sd_hint);
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
        return "SD mount failed";
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
        dbg_console_printf("%s\n",
                           s_last_sd_hint[0] ? s_last_sd_hint :
                           "use a FAT/FAT32-formatted card; exFAT is not enabled");
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

static void *alloc_gif_buffer(size_t size)
{
    void *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) {
        data = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return data;
}

static esp_err_t load_gif_into_memory(const app_gif_t *gif,
                                      uint8_t **out_data,
                                      size_t *out_size)
{
    if (gif == NULL || gif->path == NULL || out_data == NULL || out_size == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_data = NULL;
    *out_size = 0;

    size_t size = gif->size;
    if (size == 0) {
        struct stat st;
        if (stat(gif->path, &st) != 0 || st.st_size <= 0) {
            ESP_LOGE(TAG, "GIF '%s' has invalid size", gif->name);
            return ESP_ERR_INVALID_SIZE;
        }
        size = (size_t)st.st_size;
    }
    if (size > UINT32_MAX) {
        ESP_LOGE(TAG, "GIF '%s' is too large for LVGL source descriptor: %u bytes",
                 gif->name, (unsigned)size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (size > INT_MAX) {
        ESP_LOGE(TAG, "GIF '%s' is too large for AnimatedGIF RAM source: %u bytes",
                 gif->name, (unsigned)size);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *fp = fopen(gif->path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "Failed to open GIF '%s': errno=%d (%s)",
                 gif->path, errno, strerror(errno));
        return ESP_FAIL;
    }

    uint8_t *data = alloc_gif_buffer(size);
    if (data == NULL) {
        fclose(fp);
        ESP_LOGE(TAG, "No memory for GIF '%s' (%u bytes)", gif->name, (unsigned)size);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (total < size) {
        const size_t want = ((size - total) > APP_GIF_LOAD_CHUNK_SIZE) ?
                            APP_GIF_LOAD_CHUNK_SIZE : (size - total);
        const size_t got = fread(data + total, 1, want, fp);
        if (got == 0) {
            const int read_errno = ferror(fp) ? errno : 0;
            ESP_LOGE(TAG, "Failed to read GIF '%s': %s",
                     gif->path,
                     (read_errno != 0) ? strerror(read_errno) : "unexpected EOF");
            free(data);
            fclose(fp);
            return ESP_FAIL;
        }
        total += got;
    }

    if (fclose(fp) != 0) {
        ESP_LOGW(TAG, "fclose failed for GIF '%s': errno=%d (%s)",
                 gif->path, errno, strerror(errno));
    }

    *out_data = data;
    *out_size = size;
    return ESP_OK;
}

static void fill_frame_rect(app_gif_player_t *player,
                            int x,
                            int y,
                            int w,
                            int h,
                            uint16_t color)
{
    if (player == NULL || player->frame_data == NULL || w <= 0 || h <= 0) {
        return;
    }

    int x1 = x;
    int y1 = y;
    int x2 = x + w;
    int y2 = y + h;

    if (x1 < 0) {
        x1 = 0;
    }
    if (y1 < 0) {
        y1 = 0;
    }
    if (x2 > player->width) {
        x2 = player->width;
    }
    if (y2 > player->height) {
        y2 = player->height;
    }
    if (x1 >= x2 || y1 >= y2) {
        return;
    }

    for (int row = y1; row < y2; row++) {
        uint16_t *dst = player->frame_data + ((size_t)row * player->width) + x1;
        for (int col = x1; col < x2; col++) {
            *dst++ = color;
        }
    }
}

static uint16_t gif_draw_bg_color(const GIFDRAW *draw)
{
    if (draw == NULL || draw->pPalette == NULL) {
        return 0;
    }
    return draw->pPalette[draw->ucBackground];
}

static void gif_draw_cb(GIFDRAW *draw)
{
    app_gif_player_t *player = (app_gif_player_t *)draw->pUser;
    if (player == NULL || player->frame_data == NULL || draw->pPixels == NULL ||
        draw->pPalette == NULL) {
        return;
    }

    if (!player->frame_drawn) {
        player->frame_state = (app_gif_frame_state_t) {
            .x = draw->iX,
            .y = draw->iY,
            .w = draw->iWidth,
            .h = draw->iHeight,
            .disposal_method = draw->ucDisposalMethod,
            .bg_color = gif_draw_bg_color(draw),
        };
        player->have_frame_state = true;
        player->frame_drawn = true;
    }

    const int dst_y = draw->iY + draw->y;
    if (dst_y < 0 || dst_y >= player->height) {
        return;
    }

    int src_x = 0;
    int dst_x = draw->iX;
    int width = draw->iWidth;

    if (dst_x < 0) {
        src_x = -dst_x;
        width -= src_x;
        dst_x = 0;
    }
    if (dst_x + width > player->width) {
        width = player->width - dst_x;
    }
    if (width <= 0) {
        return;
    }

    const uint8_t *src = draw->pPixels + src_x;
    uint16_t *dst = player->frame_data + ((size_t)dst_y * player->width) + dst_x;
    const uint16_t *palette = draw->pPalette;

    if (draw->ucHasTransparency) {
        for (int i = 0; i < width; i++) {
            const uint8_t pixel = src[i];
            if (pixel != draw->ucTransparent) {
                dst[i] = palette[pixel];
            }
        }
    } else {
        for (int i = 0; i < width; i++) {
            dst[i] = palette[src[i]];
        }
    }
}

static void apply_previous_frame_disposal(app_gif_player_t *player)
{
    if (player == NULL || !player->have_frame_state) {
        return;
    }

    const app_gif_frame_state_t *frame = &player->frame_state;
    if (frame->disposal_method == 2) {
        fill_frame_rect(player, frame->x, frame->y, frame->w, frame->h, frame->bg_color);
    } else if (frame->disposal_method == 3) {
        /*
         * Restore-to-previous needs an additional full-frame backup buffer. Keep memory
         * usage bounded and degrade to background restore, matching the previous behavior.
         */
        fill_frame_rect(player, frame->x, frame->y, frame->w, frame->h, frame->bg_color);
    }
}

static void update_gif_image_locked(app_gif_player_t *player)
{
    if (player == NULL || player->image == NULL) {
        return;
    }

    lv_image_cache_drop(&player->frame_dsc);
    lv_obj_invalidate(player->image);
}

static bool decode_next_frame_locked(app_gif_player_t *player)
{
    if (player == NULL || !player->open) {
        return false;
    }

    apply_previous_frame_disposal(player);
    player->frame_drawn = false;

    int delay_ms = 0;
    (void)GIF_playFrame(&player->gif, &delay_ms, player);
    if (!player->frame_drawn) {
        ESP_LOGE(TAG, "GIF decode produced no frame, last_error=%d",
                 GIF_getLastError(&player->gif));
        return false;
    }

    player->frame_index++;
    if (delay_ms < (int)APP_GIF_MIN_FRAME_DELAY_MS) {
        delay_ms = APP_GIF_MIN_FRAME_DELAY_MS;
    }
    if (player->timer != NULL) {
        lv_timer_set_period(player->timer, (uint32_t)delay_ms);
    }

    update_gif_image_locked(player);
    return true;
}

static void gif_timer_cb(lv_timer_t *timer)
{
    if (!decode_next_frame_locked((app_gif_player_t *)lv_timer_get_user_data(timer))) {
        lv_timer_pause(timer);
    }
}

static void stop_gif_player_locked(uint8_t **old_source,
                                   uint16_t **old_frame)
{
    app_gif_player_t *player = &s_gif_player;

    if (old_source != NULL) {
        *old_source = player->source_data;
    }
    if (old_frame != NULL) {
        *old_frame = player->frame_data;
    }

    if (player->timer != NULL) {
        lv_timer_delete(player->timer);
    }
    if (player->image != NULL) {
        lv_obj_delete(player->image);
    }
    if (player->open) {
        GIF_close(&player->gif);
    }

    memset(player, 0, sizeof(*player));

    lv_obj_t *parent = (s_gif_layer != NULL) ? s_gif_layer : lv_screen_active();
    lv_obj_invalidate(parent);
}

static uint16_t initial_gif_bg_color(const GIFIMAGE *gif)
{
    if (gif == NULL) {
        return 0;
    }
    return gif->pPalette[gif->ucBackground];
}

static esp_err_t start_gif_player_locked(const app_gif_t *gif,
                                         uint8_t *source_data,
                                         size_t source_size)
{
    if (gif == NULL || source_data == NULL || source_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    app_gif_player_t *player = &s_gif_player;
    memset(player, 0, sizeof(*player));
    player->source_data = source_data;
    player->source_size = source_size;

    if (source_size > INT_MAX) {
        ESP_LOGE(TAG, "GIF source is too large for %s: %u bytes",
                 gif->name, (unsigned)source_size);
        return ESP_ERR_INVALID_SIZE;
    }

    GIF_begin(&player->gif, GIF_PALETTE_RGB565_LE);
    if (!GIF_openRAM(&player->gif, source_data, (int)source_size, gif_draw_cb)) {
        ESP_LOGE(TAG, "GIF_openRAM failed for %s, last_error=%d",
                 gif->name, GIF_getLastError(&player->gif));
        return ESP_FAIL;
    }
    player->open = true;
    player->width = GIF_getCanvasWidth(&player->gif);
    player->height = GIF_getCanvasHeight(&player->gif);

    if (player->width <= 0 || player->height <= 0 ||
        (size_t)player->width > SIZE_MAX / (size_t)player->height ||
        ((size_t)player->width * (size_t)player->height) > SIZE_MAX / sizeof(uint16_t)) {
        ESP_LOGE(TAG, "Invalid GIF canvas for %s: %dx%d",
                 gif->name, player->width, player->height);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t pixel_count = (size_t)player->width * (size_t)player->height;
    player->frame_data_size = pixel_count * sizeof(uint16_t);
    if (player->frame_data_size > UINT32_MAX) {
        ESP_LOGE(TAG, "GIF frame buffer is too large for %s: %u bytes",
                 gif->name, (unsigned)player->frame_data_size);
        return ESP_ERR_INVALID_SIZE;
    }
    player->frame_data = alloc_gif_buffer(player->frame_data_size);
    if (player->frame_data == NULL) {
        ESP_LOGE(TAG, "No memory for GIF frame buffer %s (%u bytes)",
                 gif->name, (unsigned)player->frame_data_size);
        return ESP_ERR_NO_MEM;
    }

    const uint16_t bg_color = initial_gif_bg_color(&player->gif);
    for (size_t i = 0; i < pixel_count; i++) {
        player->frame_data[i] = bg_color;
    }

    player->frame_dsc = (lv_image_dsc_t) {
        .header = {
            .magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_RGB565,
            .w = (uint32_t)player->width,
            .h = (uint32_t)player->height,
            .stride = (uint32_t)player->width * sizeof(uint16_t),
        },
        .data_size = (uint32_t)player->frame_data_size,
        .data = (const uint8_t *)player->frame_data,
    };

    lv_obj_t *parent = (s_gif_layer != NULL) ? s_gif_layer : lv_screen_active();
    player->image = lv_image_create(parent);
    if (player->image == NULL) {
        ESP_LOGE(TAG, "Failed to create LVGL image object for %s", gif->name);
        return ESP_ERR_NO_MEM;
    }
    lv_image_set_src(player->image, &player->frame_dsc);
    lv_obj_center(player->image);

    player->timer = lv_timer_create(gif_timer_cb, APP_GIF_MIN_FRAME_DELAY_MS, player);
    if (player->timer == NULL) {
        ESP_LOGE(TAG, "Failed to create GIF timer for %s", gif->name);
        return ESP_ERR_NO_MEM;
    }
    lv_timer_pause(player->timer);

    if (!decode_next_frame_locked(player)) {
        return ESP_FAIL;
    }

    lv_timer_resume(player->timer);
    lv_obj_invalidate(parent);

    ESP_LOGI(TAG, "GIF player ready: %s canvas=%dx%d file=%u frame_buf=%u",
             gif->name,
             player->width,
             player->height,
             (unsigned)source_size,
             (unsigned)player->frame_data_size);
    return ESP_OK;
}

static void free_gif_buffers(uint8_t *source, uint16_t *frame)
{
    free(source);
    free(frame);
}

static void show_load_failed(int index, const app_gif_t *gif)
{
    uint8_t *old_source = NULL;
    uint16_t *old_frame = NULL;

    if (bsp_display_lock(APP_LVGL_LOCK_TIMEOUT_MS) == ESP_OK) {
        stop_gif_player_locked(&old_source, &old_frame);
        s_cur_index = index;
        lv_label_set_text_fmt(s_name_label, "load failed: %s", gif->name);
        lv_obj_align(s_name_label, LV_ALIGN_BOTTOM_MID, 0, -18);
        lv_obj_move_to_index(s_name_label, -1);
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to lock display, skip load-failed update");
    }

    free_gif_buffers(old_source, old_frame);
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

    const app_gif_t *gif = &s_gifs[index];
    uint8_t *new_data = NULL;
    size_t new_size = 0;
    esp_err_t err = load_gif_into_memory(gif, &new_data, &new_size);
    bool previous_cleared_for_retry = false;
    if (err == ESP_ERR_NO_MEM && s_gif_player.source_data != NULL) {
        uint8_t *old_source = NULL;
        uint16_t *old_frame = NULL;

        if (bsp_display_lock(APP_LVGL_LOCK_TIMEOUT_MS) == ESP_OK) {
            stop_gif_player_locked(&old_source, &old_frame);
            set_status_text_locked("loading...");
            lv_obj_move_to_index(s_name_label, -1);
            bsp_display_unlock();
            previous_cleared_for_retry = true;
        }

        free_gif_buffers(old_source, old_frame);
        if (previous_cleared_for_retry) {
            err = load_gif_into_memory(gif, &new_data, &new_size);
        }
    }
    if (err != ESP_OK) {
        show_load_failed(index, gif);
        return;
    }

    uint8_t *old_source = NULL;
    uint16_t *old_frame = NULL;
    uint8_t *failed_source = NULL;
    uint16_t *failed_frame = NULL;

    if (bsp_display_lock(APP_LVGL_LOCK_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to lock display, skip switching");
        free(new_data);
        return;
    }

    stop_gif_player_locked(&old_source, &old_frame);
    err = start_gif_player_locked(gif, new_data, new_size);
    new_data = NULL;
    if (err == ESP_OK) {
        s_cur_index = index;
        lv_label_set_text_fmt(s_name_label, "%d/%d %s", index + 1, s_total, gif->name);
        ESP_LOGI(TAG, "Showing GIF %d/%d: %s (%u bytes)",
                 index + 1, s_total, gif->name, (unsigned)gif->size);
    } else {
        stop_gif_player_locked(&failed_source, &failed_frame);
        s_cur_index = index;
        lv_label_set_text_fmt(s_name_label, "load failed: %s", gif->name);
        ESP_LOGE(TAG, "Failed to start GIF %d/%d: %s (%s): %s",
                 index + 1, s_total, gif->name, gif->path, esp_err_to_name(err));
    }
    lv_obj_align(s_name_label, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_move_to_index(s_name_label, -1);
    bsp_display_unlock();

    free(new_data);
    free_gif_buffers(old_source, old_frame);
    free_gif_buffers(failed_source, failed_frame);
}

static void create_ui(void)
{
    ESP_ERROR_CHECK(bsp_display_lock(APP_LVGL_LOCK_TIMEOUT_MS));

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    s_gif_layer = lv_obj_create(screen);
    lv_obj_remove_style_all(s_gif_layer);
    lv_obj_set_size(s_gif_layer, LV_PCT(100), LV_PCT(100));
    lv_obj_align(s_gif_layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_gif_layer, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_gif_layer, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_gif_layer, LV_OBJ_FLAG_SCROLLABLE);

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

static void post_event(app_evt_type_t type, int index)
{
    app_evt_t evt = { .type = type, .index = index };
    if (xQueueSend(s_evt_queue, &evt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, drop %s event", event_name(type));
    } else {
        ESP_LOGI(TAG, "Event queued: %s index=%d", event_name(type), index);
    }
}

static void on_button_click(void *button_handle, void *usr_data)
{
    (void)button_handle;
    (void)usr_data;
    ESP_LOGI(TAG, "BOOT single click");
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

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    print_media_status();
    dbg_console_printf("gifs    : %d\n", s_total);
    if (s_sd_bus_width > 0) {
        dbg_console_printf("sd bus  : %u-bit\n", (unsigned)s_sd_bus_width);
    } else {
        dbg_console_printf("sd bus  : not mounted\n");
    }
    if (s_total > 0 && s_cur_index >= 0 && s_cur_index < s_total) {
        dbg_console_printf("current : %d/%d %s\n",
                           s_cur_index + 1, s_total, s_gifs[s_cur_index].name);
    }
    dbg_console_printf("canvas  : %dx%d frame=%d\n",
                       s_gif_player.width,
                       s_gif_player.height,
                       s_gif_player.frame_index);
    dbg_console_printf("source  : %u bytes\n", (unsigned)s_gif_player.source_size);
    dbg_console_printf("framebuf: %u bytes\n", (unsigned)s_gif_player.frame_data_size);
    dbg_console_printf("internal: free %u KB, largest %u KB\n",
                       (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                       (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    dbg_console_printf("psram   : free %u KB, largest %u KB\n",
                       (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                       (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
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
    err = dbg_console_register_cmd("status", "Show GIF player status", NULL, cmd_status);
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

    /*
     * GIF 单帧解码 + 整屏 flush 的耗时会超过帧定时器周期，lv_timer_handler()
     * 因此总是返回 0。适配器默认 task_min_delay_ms=1，在 CONFIG_FREERTOS_HZ=100
     * 下 pdMS_TO_TICKS(1)==0，vTaskDelay(0) 不会让出 CPU，高优先级(6)的 lvgl
     * 任务将饿死 CPU0 上的 app_main(优先级1) 与 IDLE0：按键事件入队后永远无人
     * 处理，并触发 task_wdt 风暴。将最小休眠提高到 10ms（100Hz 下恰好 1 tick），
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
        return;
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(APP_LCD_BRIGHTNESS_PERCENT));

    create_ui();

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

    const dbg_console_config_t console_cfg = {
        .prompt = "dbg> ",
        .max_cmdline_length = 256,
        .register_system_cmds = true,
        .register_user_cmds = register_app_cmds,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(dbg_console_start(&console_cfg));

    err = mount_sd_and_scan_gifs();
    if (err == ESP_OK) {
        show_gif(0);
    } else {
        ESP_LOGW(TAG, "GIF source unavailable: %s", media_status_text());
        set_status_text("%s", media_status_text());
    }

    app_evt_t evt;
    while (xQueueReceive(s_evt_queue, &evt, portMAX_DELAY) == pdTRUE) {
        ESP_LOGI(TAG, "Event handling: %s index=%d current=%d total=%d",
                 event_name(evt.type), evt.index, s_cur_index, s_total);
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

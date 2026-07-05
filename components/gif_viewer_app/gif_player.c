#include "gif_player.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "libs/gif/AnimatedGIF.h"
#include "misc/cache/instance/lv_image_cache.h"

#define GIF_PLAYER_MIN_FRAME_DELAY_MS 20U

typedef struct {
    int x;
    int y;
    int w;
    int h;
    uint8_t disposal_method;
    uint16_t bg_color;
} gif_frame_state_t;

struct gif_player {
    GIFIMAGE gif;
    lv_obj_t *parent;
    lv_obj_t *image;
    lv_timer_t *timer;
    lv_image_dsc_t frame_dsc;
    uint16_t *frame_data;
    size_t frame_data_size;
    size_t file_size;
    int width;
    int height;
    int frame_index;
    esp_err_t last_error;
    int gif_error;
    bool open;
    bool have_frame_state;
    bool frame_drawn;
    gif_frame_state_t frame_state;
    char name[96];
    char lvgl_path[192];
    SemaphoreHandle_t status_lock;
    gif_player_status_t status;
};

static const char *TAG = "gif_player";

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (dst == NULL || dst_size == 0U) {
        return;
    }

    snprintf(dst, dst_size, "%s", (src != NULL) ? src : "");
}

static void *alloc_frame_buffer(size_t size)
{
    void *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) {
        data = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return data;
}

static void status_lock(gif_player_t *player)
{
    if (player != NULL && player->status_lock != NULL) {
        (void)xSemaphoreTake(player->status_lock, portMAX_DELAY);
    }
}

static void status_unlock(gif_player_t *player)
{
    if (player != NULL && player->status_lock != NULL) {
        xSemaphoreGive(player->status_lock);
    }
}

static void reset_runtime_state(gif_player_t *player)
{
    lv_obj_t *parent = player->parent;
    SemaphoreHandle_t lock = player->status_lock;

    memset(player, 0, sizeof(*player));
    player->parent = parent;
    player->status_lock = lock;
    player->last_error = ESP_OK;
    player->status.last_error = ESP_OK;
}

static void sync_status_locked(gif_player_t *player)
{
    if (player == NULL) {
        return;
    }

    player->status.open = player->open;
    copy_text(player->status.name, sizeof(player->status.name), player->name);
    copy_text(player->status.lvgl_path, sizeof(player->status.lvgl_path), player->lvgl_path);
    player->status.width = player->width;
    player->status.height = player->height;
    player->status.frame_index = player->frame_index;
    player->status.file_size_bytes = player->file_size;
    player->status.frame_data_size_bytes = player->frame_data_size;
    player->status.last_error = player->last_error;
    player->status.gif_error = player->gif_error;
}

static void fill_frame_rect(gif_player_t *player,
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
        uint16_t *dst = player->frame_data + ((size_t)row * (size_t)player->width) + x1;
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
    gif_player_t *player = (gif_player_t *)draw->pUser;
    if (player == NULL || player->frame_data == NULL || draw->pPixels == NULL ||
        draw->pPalette == NULL) {
        return;
    }

    if (!player->frame_drawn) {
        player->frame_state = (gif_frame_state_t) {
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
    uint16_t *dst = player->frame_data + ((size_t)dst_y * (size_t)player->width) + dst_x;
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

static void apply_previous_frame_disposal(gif_player_t *player)
{
    if (player == NULL || !player->have_frame_state) {
        return;
    }

    const gif_frame_state_t *frame = &player->frame_state;
    if (frame->disposal_method == 2 || frame->disposal_method == 3) {
        fill_frame_rect(player, frame->x, frame->y, frame->w, frame->h, frame->bg_color);
    }
}

static void update_gif_image_locked(gif_player_t *player)
{
    if (player == NULL || player->image == NULL) {
        return;
    }

    lv_image_cache_drop(&player->frame_dsc);
    lv_obj_invalidate(player->image);
}

static bool decode_next_frame_locked(gif_player_t *player)
{
    if (player == NULL || !player->open) {
        return false;
    }

    apply_previous_frame_disposal(player);
    player->frame_drawn = false;

    int delay_ms = 0;
    (void)GIF_playFrame(&player->gif, &delay_ms, player);
    if (!player->frame_drawn) {
        const int gif_error = GIF_getLastError(&player->gif);
        ESP_LOGE(TAG, "GIF decode produced no frame, last_error=%d", gif_error);
        status_lock(player);
        player->gif_error = gif_error;
        player->last_error = ESP_FAIL;
        sync_status_locked(player);
        status_unlock(player);
        return false;
    }

    status_lock(player);
    player->frame_index++;
    player->gif_error = GIF_getLastError(&player->gif);
    player->last_error = ESP_OK;
    sync_status_locked(player);
    status_unlock(player);

    if (delay_ms < (int)GIF_PLAYER_MIN_FRAME_DELAY_MS) {
        delay_ms = GIF_PLAYER_MIN_FRAME_DELAY_MS;
    }
    if (player->timer != NULL) {
        lv_timer_set_period(player->timer, (uint32_t)delay_ms);
    }

    update_gif_image_locked(player);
    return true;
}

static void gif_timer_cb(lv_timer_t *timer)
{
    if (!decode_next_frame_locked((gif_player_t *)lv_timer_get_user_data(timer))) {
        lv_timer_pause(timer);
    }
}

static uint16_t initial_gif_bg_color(const GIFIMAGE *gif)
{
    if (gif == NULL) {
        return 0;
    }
    return gif->pPalette[gif->ucBackground];
}

static esp_err_t validate_canvas(const gif_playlist_item_t *item, int width, int height)
{
    if (width <= 0 || height <= 0 ||
        (size_t)width > SIZE_MAX / (size_t)height ||
        ((size_t)width * (size_t)height) > SIZE_MAX / sizeof(uint16_t)) {
        ESP_LOGE(TAG, "Invalid GIF canvas for %s: %dx%d",
                 item->name, width, height);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t fail_show_locked(gif_player_t *player,
                                  const gif_playlist_item_t *item,
                                  esp_err_t err)
{
    const int gif_error = GIF_getLastError(&player->gif);
    const size_t file_size = (item != NULL) ? item->size_bytes : 0U;
    char name[sizeof(player->name)];
    char lvgl_path[sizeof(player->lvgl_path)];

    copy_text(name, sizeof(name), (item != NULL) ? item->name : "");
    copy_text(lvgl_path, sizeof(lvgl_path), (item != NULL) ? item->lvgl_path : "");

    gif_player_stop_locked(player);

    status_lock(player);
    copy_text(player->name, sizeof(player->name), name);
    copy_text(player->lvgl_path, sizeof(player->lvgl_path), lvgl_path);
    player->file_size = file_size;
    player->last_error = err;
    player->gif_error = gif_error;
    sync_status_locked(player);
    status_unlock(player);
    return err;
}

esp_err_t gif_player_create(const gif_player_config_t *config, gif_player_t **out_player)
{
    if (config == NULL || out_player == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gif_player_t *player = calloc(1, sizeof(*player));
    if (player == NULL) {
        return ESP_ERR_NO_MEM;
    }

    player->parent = (config->parent != NULL) ? config->parent : lv_screen_active();
    player->status_lock = xSemaphoreCreateMutex();
    if (player->status_lock == NULL) {
        free(player);
        return ESP_ERR_NO_MEM;
    }
    player->last_error = ESP_OK;
    player->status.last_error = ESP_OK;

    *out_player = player;
    return ESP_OK;
}

void gif_player_stop_locked(gif_player_t *player)
{
    if (player == NULL) {
        return;
    }

    if (player->timer != NULL) {
        lv_timer_delete(player->timer);
        player->timer = NULL;
    }
    if (player->image != NULL) {
        lv_image_cache_drop(&player->frame_dsc);
        lv_obj_delete(player->image);
        player->image = NULL;
    }
    if (player->open) {
        GIF_close(&player->gif);
        player->open = false;
    }
    free(player->frame_data);
    player->frame_data = NULL;

    lv_obj_t *parent = (player->parent != NULL) ? player->parent : lv_screen_active();
    lv_obj_invalidate(parent);

    status_lock(player);
    reset_runtime_state(player);
    status_unlock(player);
}

void gif_player_destroy(gif_player_t *player)
{
    if (player == NULL) {
        return;
    }

    gif_player_stop_locked(player);
    if (player->status_lock != NULL) {
        vSemaphoreDelete(player->status_lock);
    }
    free(player);
}

esp_err_t gif_player_show_locked(gif_player_t *player, const gif_playlist_item_t *item)
{
    if (player == NULL || item == NULL || item->lvgl_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gif_player_stop_locked(player);

    status_lock(player);
    copy_text(player->name, sizeof(player->name), item->name);
    copy_text(player->lvgl_path, sizeof(player->lvgl_path), item->lvgl_path);
    player->file_size = item->size_bytes;
    sync_status_locked(player);
    status_unlock(player);

    GIF_begin(&player->gif, GIF_PALETTE_RGB565_LE);
    if (!GIF_openFile(&player->gif, item->lvgl_path, gif_draw_cb)) {
        const int gif_error = GIF_getLastError(&player->gif);
        ESP_LOGE(TAG, "GIF_openFile failed for %s (%s), last_error=%d",
                 item->name, item->lvgl_path, gif_error);
        return fail_show_locked(player, item, ESP_FAIL);
    }
    player->open = true;
    player->gif.ucDrawType = GIF_DRAW_RAW;
    player->width = GIF_getCanvasWidth(&player->gif);
    player->height = GIF_getCanvasHeight(&player->gif);
    status_lock(player);
    sync_status_locked(player);
    status_unlock(player);

    esp_err_t err = validate_canvas(item, player->width, player->height);
    if (err != ESP_OK) {
        return fail_show_locked(player, item, err);
    }

    const size_t pixel_count = (size_t)player->width * (size_t)player->height;
    player->frame_data_size = pixel_count * sizeof(uint16_t);
    status_lock(player);
    sync_status_locked(player);
    status_unlock(player);
    if (player->frame_data_size > UINT32_MAX) {
        ESP_LOGE(TAG, "GIF frame buffer is too large for %s: %u bytes",
                 item->name, (unsigned)player->frame_data_size);
        return fail_show_locked(player, item, ESP_ERR_INVALID_SIZE);
    }

    player->frame_data = alloc_frame_buffer(player->frame_data_size);
    if (player->frame_data == NULL) {
        ESP_LOGE(TAG, "No memory for GIF frame buffer %s (%u bytes)",
                 item->name, (unsigned)player->frame_data_size);
        return fail_show_locked(player, item, ESP_ERR_NO_MEM);
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

    lv_obj_t *parent = (player->parent != NULL) ? player->parent : lv_screen_active();
    player->image = lv_image_create(parent);
    if (player->image == NULL) {
        return fail_show_locked(player, item, ESP_ERR_NO_MEM);
    }
    lv_image_set_src(player->image, &player->frame_dsc);
    lv_obj_center(player->image);

    player->timer = lv_timer_create(gif_timer_cb, GIF_PLAYER_MIN_FRAME_DELAY_MS, player);
    if (player->timer == NULL) {
        return fail_show_locked(player, item, ESP_ERR_NO_MEM);
    }
    lv_timer_pause(player->timer);

    if (!decode_next_frame_locked(player)) {
        return fail_show_locked(player, item, ESP_FAIL);
    }

    lv_timer_resume(player->timer);
    lv_obj_invalidate(parent);

    ESP_LOGI(TAG, "GIF player ready: %s canvas=%dx%d file=%u frame_buf=%u source=%s",
             item->name,
             player->width,
             player->height,
             (unsigned)item->size_bytes,
             (unsigned)player->frame_data_size,
             item->lvgl_path);
    return ESP_OK;
}

void gif_player_get_status(gif_player_t *player, gif_player_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    if (player != NULL) {
        status_lock(player);
        *status = player->status;
        status_unlock(player);
    }

    status->internal_free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    status->internal_largest_bytes = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    status->psram_free_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    status->psram_largest_bytes = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}

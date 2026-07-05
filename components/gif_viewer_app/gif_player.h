#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#include "gif_playlist.h"

typedef struct gif_player gif_player_t;

typedef struct {
    lv_obj_t *parent;
} gif_player_config_t;

typedef struct {
    bool open;
    char name[96];
    char lvgl_path[192];
    int width;
    int height;
    int frame_index;
    size_t file_size_bytes;
    size_t frame_data_size_bytes;
    esp_err_t last_error;
    int gif_error;
    size_t internal_free_bytes;
    size_t internal_largest_bytes;
    size_t psram_free_bytes;
    size_t psram_largest_bytes;
} gif_player_status_t;

esp_err_t gif_player_create(const gif_player_config_t *config, gif_player_t **out_player);
void gif_player_destroy(gif_player_t *player);
esp_err_t gif_player_show_locked(gif_player_t *player, const gif_playlist_item_t *item);
void gif_player_stop_locked(gif_player_t *player);
void gif_player_get_status(gif_player_t *player, gif_player_status_t *status);

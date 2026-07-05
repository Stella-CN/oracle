#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef struct {
    char *name;
    char *sd_path;
    char *lvgl_path;
    size_t size_bytes;
} gif_playlist_item_t;

typedef struct {
    gif_playlist_item_t *items;
    size_t count;
    size_t capacity;
} gif_playlist_t;

typedef enum {
    GIF_PLAYLIST_SCAN_OK = 0,
    GIF_PLAYLIST_SCAN_DIR_MISSING,
    GIF_PLAYLIST_SCAN_EMPTY,
    GIF_PLAYLIST_SCAN_FAILED,
} gif_playlist_scan_result_t;

typedef struct {
    gif_playlist_scan_result_t result;
    int errno_code;
} gif_playlist_scan_status_t;

void gif_playlist_init(gif_playlist_t *playlist);
void gif_playlist_clear(gif_playlist_t *playlist);
esp_err_t gif_playlist_scan(gif_playlist_t *playlist,
                            const char *sd_dir,
                            const char *lvgl_prefix,
                            gif_playlist_scan_status_t *status);
size_t gif_playlist_count(const gif_playlist_t *playlist);
const gif_playlist_item_t *gif_playlist_get(const gif_playlist_t *playlist, size_t index);

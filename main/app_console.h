#pragma once

#include "esp_err.h"

#include "app_events.h"
#include "gif_player.h"
#include "gif_playlist.h"
#include "sd_media.h"

typedef int (*app_console_get_current_index_cb_t)(void *ctx);

typedef struct {
    const gif_playlist_t *playlist;
    const sd_media_status_t *media_status;
    gif_player_t *player;
    app_post_event_cb_t post_event;
    app_console_get_current_index_cb_t get_current_index;
    void *ctx;
} app_console_config_t;

esp_err_t app_console_start(const app_console_config_t *config);

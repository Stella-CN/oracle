#pragma once

#include <stdint.h>

#include "esp_err.h"

#include "gif_playlist.h"

typedef enum {
    SD_MEDIA_OK = 0,
    SD_MEDIA_SD_MOUNT_FAILED,
    SD_MEDIA_GIF_DIR_MISSING,
    SD_MEDIA_NO_GIFS,
    SD_MEDIA_SCAN_FAILED,
} sd_media_state_t;

typedef struct {
    sd_media_state_t state;
    esp_err_t last_sd_err;
    int last_scan_errno;
    char hint[256];
    uint8_t bus_width;
} sd_media_status_t;

void sd_media_status_init(sd_media_status_t *status);
esp_err_t sd_media_mount(sd_media_status_t *status);
void sd_media_apply_scan_status(sd_media_status_t *status,
                                const gif_playlist_scan_status_t *scan_status);
const char *sd_media_state_text(sd_media_state_t state);

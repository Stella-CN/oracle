#include "sd_media.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "app_config.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "sd_media";

static void set_hint(sd_media_status_t *status, const char *fmt, ...)
{
    if (status == NULL) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(status->hint, sizeof(status->hint), fmt, args);
    va_end(args);
}

static esp_err_t mount_with_width(uint8_t bus_width, sd_media_status_t *status)
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
        set_hint(status,
                 "SDMMC %u-bit mount failed through BSP helper: %s",
                 (unsigned)bus_width,
                 esp_err_to_name(err));
    }
    return err;
}

void sd_media_status_init(sd_media_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->state = SD_MEDIA_SD_MOUNT_FAILED;
    status->last_sd_err = ESP_OK;
}

esp_err_t sd_media_mount(sd_media_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    status->state = SD_MEDIA_SD_MOUNT_FAILED;
    status->last_sd_err = ESP_OK;
    status->last_scan_errno = 0;
    status->hint[0] = '\0';
    status->bus_width = 0;

    esp_err_t err = mount_with_width(4, status);
    if (err == ESP_OK) {
        status->state = SD_MEDIA_SCAN_FAILED;
        status->bus_width = 4;
        ESP_LOGI(TAG, "SD card mounted at %s (SDMMC 4-bit)", BSP_SD_MOUNT_POINT);
        return ESP_OK;
    }

    const esp_err_t four_bit_err = err;
    char four_bit_hint[sizeof(status->hint)];
    snprintf(four_bit_hint, sizeof(four_bit_hint), "%s",
             status->hint[0] ? status->hint : "no detail");

    ESP_LOGW(TAG, "SDMMC 4-bit mount failed: %s (%s), retrying 1-bit",
             esp_err_to_name(err), four_bit_hint);

    err = mount_with_width(1, status);
    if (err == ESP_OK) {
        status->state = SD_MEDIA_SCAN_FAILED;
        status->bus_width = 1;
        ESP_LOGI(TAG, "SD card mounted at %s (SDMMC 1-bit fallback)", BSP_SD_MOUNT_POINT);
        return ESP_OK;
    }

    char one_bit_hint[sizeof(status->hint)];
    snprintf(one_bit_hint, sizeof(one_bit_hint), "%s",
             status->hint[0] ? status->hint : "no detail");

    status->state = SD_MEDIA_SD_MOUNT_FAILED;
    status->last_sd_err = err;
    set_hint(status,
             "4-bit: %s (%s); 1-bit: %s (%s). Supported: FAT/FAT32 on SFD/MBR/GPT; exFAT is not enabled. Expected /%s",
             esp_err_to_name(four_bit_err), four_bit_hint,
             esp_err_to_name(err), one_bit_hint,
             APP_SD_GIF_SUBDIR);
    ESP_LOGE(TAG, "Failed to mount SD card. %s", status->hint);
    return err;
}

void sd_media_apply_scan_status(sd_media_status_t *status,
                                const gif_playlist_scan_status_t *scan_status)
{
    if (status == NULL || scan_status == NULL) {
        return;
    }

    status->last_scan_errno = scan_status->errno_code;
    switch (scan_status->result) {
    case GIF_PLAYLIST_SCAN_OK:
        status->state = SD_MEDIA_OK;
        break;
    case GIF_PLAYLIST_SCAN_DIR_MISSING:
        status->state = SD_MEDIA_GIF_DIR_MISSING;
        ESP_LOGE(TAG, "GIF directory missing: %s (create %s at SD card root)",
                 APP_SD_GIF_DIR, APP_SD_GIF_SUBDIR);
        break;
    case GIF_PLAYLIST_SCAN_EMPTY:
        status->state = SD_MEDIA_NO_GIFS;
        break;
    case GIF_PLAYLIST_SCAN_FAILED:
    default:
        status->state = SD_MEDIA_SCAN_FAILED;
        break;
    }
}

const char *sd_media_state_text(sd_media_state_t state)
{
    switch (state) {
    case SD_MEDIA_OK:
        return "ready";
    case SD_MEDIA_SD_MOUNT_FAILED:
        return "SD mount failed";
    case SD_MEDIA_GIF_DIR_MISSING:
        return "missing /assets/gif";
    case SD_MEDIA_NO_GIFS:
        return "no GIF in /assets/gif";
    case SD_MEDIA_SCAN_FAILED:
    default:
        return "SD scan failed";
    }
}

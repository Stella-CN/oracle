#include "app_console.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "app_config.h"
#include "dbg_console.h"

static const char *TAG = "app_console";

static app_console_config_t s_console;

static int current_index(void)
{
    if (s_console.get_current_index == NULL) {
        return 0;
    }
    return s_console.get_current_index(s_console.ctx);
}

static bool post_event(app_evt_type_t type, int index)
{
    if (s_console.post_event == NULL) {
        return false;
    }
    return s_console.post_event(type, index, s_console.ctx);
}

static void print_media_status(void)
{
    const sd_media_status_t *status = s_console.media_status;
    if (status == NULL) {
        dbg_console_printf("media status unavailable\n");
        return;
    }

    dbg_console_printf("%s\n", sd_media_state_text(status->state));
    switch (status->state) {
    case SD_MEDIA_SD_MOUNT_FAILED:
        dbg_console_printf("last SD error: %s\n", esp_err_to_name(status->last_sd_err));
        dbg_console_printf("%s\n",
                           status->hint[0] ? status->hint :
                           "use a FAT/FAT32-formatted card; exFAT is not enabled");
        break;
    case SD_MEDIA_GIF_DIR_MISSING:
        dbg_console_printf("create %s at the SD card root and copy .gif files into it\n",
                           APP_SD_GIF_SUBDIR);
        break;
    case SD_MEDIA_NO_GIFS:
        dbg_console_printf("copy .gif files into %s\n", APP_SD_GIF_DIR);
        break;
    case SD_MEDIA_SCAN_FAILED:
        dbg_console_printf("scan errno: %d (%s)\n",
                           status->last_scan_errno,
                           strerror(status->last_scan_errno));
        break;
    case SD_MEDIA_OK:
    default:
        break;
    }
}

static size_t playlist_count(void)
{
    return gif_playlist_count(s_console.playlist);
}

static int cmd_next(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        dbg_console_printf("usage: next\n");
        return 1;
    }
    if (playlist_count() == 0U) {
        print_media_status();
        return 1;
    }
    return post_event(APP_EVT_NEXT, 0) ? 0 : 1;
}

static int cmd_prev(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        dbg_console_printf("usage: prev\n");
        return 1;
    }
    if (playlist_count() == 0U) {
        print_media_status();
        return 1;
    }
    return post_event(APP_EVT_PREV, 0) ? 0 : 1;
}

static int cmd_goto(int argc, char **argv)
{
    const size_t total = playlist_count();
    if (total == 0U) {
        print_media_status();
        return 1;
    }

    if (argc != 2) {
        dbg_console_printf("usage: goto <1..%u>\n", (unsigned)total);
        return 1;
    }

    errno = 0;
    char *end = NULL;
    long index = strtol(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0') {
        dbg_console_printf("invalid index: %s\n", argv[1]);
        return 1;
    }

    if (index < 1 || index > (long)total) {
        dbg_console_printf("index out of range: %s (valid: 1..%u)\n",
                           argv[1], (unsigned)total);
        return 1;
    }

    return post_event(APP_EVT_GOTO, (int)index - 1) ? 0 : 1;
}

static int cmd_list(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        dbg_console_printf("usage: list\n");
        return 1;
    }

    const size_t total = playlist_count();
    if (total == 0U) {
        print_media_status();
        return 1;
    }

    const int current = current_index();
    for (size_t i = 0; i < total; i++) {
        const gif_playlist_item_t *item = gif_playlist_get(s_console.playlist, i);
        if (item == NULL) {
            continue;
        }
        dbg_console_printf("%c %2u: %-24s %7u bytes\n",
                           ((int)i == current) ? '*' : ' ',
                           (unsigned)i + 1U,
                           item->name,
                           (unsigned)item->size_bytes);
    }
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        dbg_console_printf("usage: status\n");
        return 1;
    }

    print_media_status();

    const sd_media_status_t *media = s_console.media_status;
    const size_t total = playlist_count();
    const int current = current_index();
    gif_player_status_t player_status;
    gif_player_get_status(s_console.player, &player_status);

    dbg_console_printf("gifs    : %u\n", (unsigned)total);
    if (media != NULL && media->bus_width > 0) {
        dbg_console_printf("sd bus  : %u-bit\n", (unsigned)media->bus_width);
    } else {
        dbg_console_printf("sd bus  : not mounted\n");
    }

    if (total > 0U && current >= 0 && current < (int)total) {
        const gif_playlist_item_t *item = gif_playlist_get(s_console.playlist, (size_t)current);
        dbg_console_printf("current : %d/%u %s\n",
                           current + 1, (unsigned)total,
                           (item != NULL) ? item->name : "unknown");
    }
    dbg_console_printf("source  : %s\n",
                       player_status.lvgl_path[0] ? player_status.lvgl_path : "none");
    dbg_console_printf("canvas  : %dx%d frame=%d open=%s\n",
                       player_status.width,
                       player_status.height,
                       player_status.frame_index,
                       player_status.open ? "yes" : "no");
    dbg_console_printf("file    : %u bytes\n", (unsigned)player_status.file_size_bytes);
    dbg_console_printf("framebuf: %u bytes\n", (unsigned)player_status.frame_data_size_bytes);
    dbg_console_printf("last err: %s gif=%d\n",
                       esp_err_to_name(player_status.last_error),
                       player_status.gif_error);
    dbg_console_printf("internal: free %u KB, largest %u KB\n",
                       (unsigned)(player_status.internal_free_bytes / 1024U),
                       (unsigned)(player_status.internal_largest_bytes / 1024U));
    dbg_console_printf("psram   : free %u KB, largest %u KB\n",
                       (unsigned)(player_status.psram_free_bytes / 1024U),
                       (unsigned)(player_status.psram_largest_bytes / 1024U));
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

esp_err_t app_console_start(const app_console_config_t *config)
{
    if (config == NULL || config->playlist == NULL || config->media_status == NULL ||
        config->player == NULL || config->post_event == NULL ||
        config->get_current_index == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_console = *config;

    const dbg_console_config_t console_cfg = {
        .prompt = "dbg> ",
        .max_cmdline_length = 256,
        .register_system_cmds = true,
        .register_user_cmds = register_app_cmds,
        .user_ctx = NULL,
    };
    esp_err_t err = dbg_console_start(&console_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Debug console unavailable: %s", esp_err_to_name(err));
    }
    return err;
}

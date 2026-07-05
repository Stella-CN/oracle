#include "gif_playlist.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_log.h"

static const char *TAG = "gif_playlist";

static char *playlist_strdup(const char *src)
{
    if (src == NULL) {
        return NULL;
    }

    size_t len = strlen(src) + 1;
    char *copy = malloc(len);
    if (copy != NULL) {
        memcpy(copy, src, len);
    }
    return copy;
}

static char *join_path(const char *dir, const char *name)
{
    if (dir == NULL || name == NULL) {
        return NULL;
    }

    const size_t dir_len = strlen(dir);
    const size_t name_len = strlen(name);
    const bool need_sep = (dir_len > 0 && dir[dir_len - 1] != '/');
    const size_t total = dir_len + (need_sep ? 1U : 0U) + name_len + 1U;
    if (total < dir_len || total < name_len) {
        return NULL;
    }

    char *path = malloc(total);
    if (path == NULL) {
        return NULL;
    }

    snprintf(path, total, "%s%s%s", dir, need_sep ? "/" : "", name);
    return path;
}

static char *build_lvgl_path(const char *prefix, const char *name)
{
    if (prefix == NULL || name == NULL) {
        return NULL;
    }

    const size_t prefix_len = strlen(prefix);
    const size_t name_len = strlen(name);
    const size_t total = prefix_len + name_len + 1U;
    if (total < prefix_len || total < name_len) {
        return NULL;
    }

    char *path = malloc(total);
    if (path == NULL) {
        return NULL;
    }

    snprintf(path, total, "%s%s", prefix, name);
    return path;
}

static bool is_gif_name(const char *name)
{
    const char *ext = (name != NULL) ? strrchr(name, '.') : NULL;
    return ext != NULL && strcasecmp(ext, ".gif") == 0;
}

static void free_item(gif_playlist_item_t *item)
{
    if (item == NULL) {
        return;
    }

    free(item->name);
    free(item->sd_path);
    free(item->lvgl_path);
    memset(item, 0, sizeof(*item));
}

static esp_err_t reserve_items(gif_playlist_t *playlist, size_t wanted)
{
    if (wanted <= playlist->capacity) {
        return ESP_OK;
    }

    size_t new_capacity = (playlist->capacity == 0U) ? 8U : playlist->capacity;
    while (new_capacity < wanted) {
        if (new_capacity > SIZE_MAX / 2U) {
            return ESP_ERR_NO_MEM;
        }
        new_capacity *= 2U;
    }

    if (new_capacity > SIZE_MAX / sizeof(*playlist->items)) {
        return ESP_ERR_NO_MEM;
    }

    gif_playlist_item_t *items = realloc(playlist->items, new_capacity * sizeof(*items));
    if (items == NULL) {
        return ESP_ERR_NO_MEM;
    }

    memset(items + playlist->capacity, 0,
           (new_capacity - playlist->capacity) * sizeof(*items));
    playlist->items = items;
    playlist->capacity = new_capacity;
    return ESP_OK;
}

static esp_err_t add_item(gif_playlist_t *playlist,
                          const char *sd_dir,
                          const char *lvgl_prefix,
                          const char *name,
                          size_t size_bytes)
{
    esp_err_t ret = reserve_items(playlist, playlist->count + 1U);
    if (ret != ESP_OK) {
        return ret;
    }

    gif_playlist_item_t item = {
        .name = playlist_strdup(name),
        .sd_path = join_path(sd_dir, name),
        .lvgl_path = build_lvgl_path(lvgl_prefix, name),
        .size_bytes = size_bytes,
    };
    if (item.name == NULL || item.sd_path == NULL || item.lvgl_path == NULL) {
        free_item(&item);
        return ESP_ERR_NO_MEM;
    }

    playlist->items[playlist->count++] = item;
    return ESP_OK;
}

static int compare_item_by_name(const void *lhs, const void *rhs)
{
    const gif_playlist_item_t *a = lhs;
    const gif_playlist_item_t *b = rhs;
    int ret = strcasecmp(a->name, b->name);
    return (ret != 0) ? ret : strcmp(a->name, b->name);
}

void gif_playlist_init(gif_playlist_t *playlist)
{
    if (playlist != NULL) {
        memset(playlist, 0, sizeof(*playlist));
    }
}

void gif_playlist_clear(gif_playlist_t *playlist)
{
    if (playlist == NULL) {
        return;
    }

    for (size_t i = 0; i < playlist->count; i++) {
        free_item(&playlist->items[i]);
    }
    free(playlist->items);
    memset(playlist, 0, sizeof(*playlist));
}

esp_err_t gif_playlist_scan(gif_playlist_t *playlist,
                            const char *sd_dir,
                            const char *lvgl_prefix,
                            gif_playlist_scan_status_t *status)
{
    if (playlist == NULL || sd_dir == NULL || lvgl_prefix == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (status != NULL) {
        *status = (gif_playlist_scan_status_t) {
            .result = GIF_PLAYLIST_SCAN_FAILED,
            .errno_code = 0,
        };
    }

    gif_playlist_clear(playlist);

    errno = 0;
    DIR *dir = opendir(sd_dir);
    if (dir == NULL) {
        const int open_errno = errno;
        if (status != NULL) {
            status->result = (open_errno == ENOENT) ?
                             GIF_PLAYLIST_SCAN_DIR_MISSING :
                             GIF_PLAYLIST_SCAN_FAILED;
            status->errno_code = open_errno;
        }
        ESP_LOGE(TAG, "Failed to open GIF directory '%s': errno=%d (%s)",
                 sd_dir, open_errno, strerror(open_errno));
        return (open_errno == ENOENT) ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    esp_err_t ret = ESP_OK;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!is_gif_name(entry->d_name)) {
            continue;
        }

        char *sd_path = join_path(sd_dir, entry->d_name);
        if (sd_path == NULL) {
            ret = ESP_ERR_NO_MEM;
            break;
        }

        struct stat st;
        if (stat(sd_path, &st) != 0) {
            ESP_LOGW(TAG, "Skip '%s': stat failed, errno=%d (%s)",
                     sd_path, errno, strerror(errno));
            free(sd_path);
            continue;
        }
        free(sd_path);

        if (S_ISDIR(st.st_mode)) {
            continue;
        }

        ret = add_item(playlist,
                       sd_dir,
                       lvgl_prefix,
                       entry->d_name,
                       (st.st_size > 0) ? (size_t)st.st_size : 0U);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add GIF '%s': %s",
                     entry->d_name, esp_err_to_name(ret));
            break;
        }
    }

    closedir(dir);

    if (ret != ESP_OK) {
        gif_playlist_clear(playlist);
        if (status != NULL) {
            status->result = GIF_PLAYLIST_SCAN_FAILED;
            status->errno_code = errno;
        }
        return ret;
    }

    if (playlist->count == 0U) {
        if (status != NULL) {
            status->result = GIF_PLAYLIST_SCAN_EMPTY;
            status->errno_code = 0;
        }
        ESP_LOGW(TAG, "No GIF files found in %s", sd_dir);
        return ESP_ERR_NOT_FOUND;
    }

    qsort(playlist->items, playlist->count, sizeof(playlist->items[0]), compare_item_by_name);
    if (status != NULL) {
        status->result = GIF_PLAYLIST_SCAN_OK;
        status->errno_code = 0;
    }
    ESP_LOGI(TAG, "Found %u GIF file(s) in %s", (unsigned)playlist->count, sd_dir);
    return ESP_OK;
}

size_t gif_playlist_count(const gif_playlist_t *playlist)
{
    return (playlist != NULL) ? playlist->count : 0U;
}

const gif_playlist_item_t *gif_playlist_get(const gif_playlist_t *playlist, size_t index)
{
    if (playlist == NULL || index >= playlist->count) {
        return NULL;
    }
    return &playlist->items[index];
}

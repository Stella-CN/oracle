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
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/sdmmc_host.h"
#include "diskio_impl.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

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
#define APP_SD_SECTOR_SIZE         512U
#define APP_FATFS_DRIVE_STR_MAX    3U
#define APP_SD_HINT_MAX            160U
#define APP_MBR_PART_TABLE_OFFSET  446U
#define APP_MBR_PART_ENTRY_SIZE    16U
#define APP_MBR_PART_TYPE_OFFSET   4U
#define APP_MBR_PART_LBA_OFFSET    8U
#define APP_MBR_PART_SIZE_OFFSET   12U
#define APP_MBR_SIGNATURE_OFFSET   510U
#define APP_GPT_HEADER_LBA         1U
#define APP_GPT_MAX_SCAN_ENTRIES   128U
#define APP_GPT_ENTRY_SIZE         128U

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

typedef enum {
    APP_SD_LAYOUT_UNKNOWN = 0,
    APP_SD_LAYOUT_SUPER_FLOPPY,
    APP_SD_LAYOUT_MBR,
    APP_SD_LAYOUT_GPT,
} app_sd_layout_t;

typedef enum {
    APP_SD_FS_UNKNOWN = 0,
    APP_SD_FS_FAT,
    APP_SD_FS_FAT32,
    APP_SD_FS_EXFAT,
} app_sd_fs_t;

typedef struct {
    app_sd_layout_t layout;
    app_sd_fs_t fs_type;
    uint32_t start_lba;
    uint32_t sector_count;
    uint8_t partition_index;
    uint8_t partition_type;
} app_sd_volume_t;

typedef struct {
    sdmmc_card_t *card;
    BYTE pdrv;
    uint32_t start_lba;
    uint32_t sector_count;
    uint16_t sector_size;
    bool disk_status_check;
} app_sd_disk_t;

static QueueHandle_t s_evt_queue;
static app_gif_t *s_gifs;
static lv_obj_t *s_gif;
static lv_obj_t *s_name_label;
static int s_cur_index;
static int s_total;
static app_media_state_t s_media_state = APP_MEDIA_SD_MOUNT_FAILED;
static esp_err_t s_last_sd_err = ESP_OK;
static int s_last_scan_errno;
static FRESULT s_last_fresult = FR_OK;
static char s_last_sd_hint[APP_SD_HINT_MAX];
static app_sd_volume_t s_sd_volume;
static app_sd_disk_t s_sd_disk = {
    .pdrv = FF_DRV_NOT_USED,
};
static FATFS *s_sd_fs;
static sdmmc_card_t *s_sd_card;
static char s_sd_drive[APP_FATFS_DRIVE_STR_MAX];

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

static void set_last_sd_hint(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(s_last_sd_hint, sizeof(s_last_sd_hint), fmt, args);
    va_end(args);
}

static const char *sd_layout_name(app_sd_layout_t layout)
{
    switch (layout) {
    case APP_SD_LAYOUT_SUPER_FLOPPY:
        return "SFD";
    case APP_SD_LAYOUT_MBR:
        return "MBR";
    case APP_SD_LAYOUT_GPT:
        return "GPT";
    case APP_SD_LAYOUT_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *sd_fs_name(app_sd_fs_t fs_type)
{
    switch (fs_type) {
    case APP_SD_FS_FAT32:
        return "FAT32";
    case APP_SD_FS_FAT:
        return "FAT";
    case APP_SD_FS_EXFAT:
        return "exFAT";
    case APP_SD_FS_UNKNOWN:
    default:
        return "unknown";
    }
}

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint64_t read_le64(const uint8_t *data)
{
    return (uint64_t)read_le32(data) | ((uint64_t)read_le32(data + 4) << 32);
}

static bool is_power_of_two_u32(uint32_t value)
{
    return value != 0 && (value & (value - 1U)) == 0;
}

static bool has_boot_sector_signature(const uint8_t *sector)
{
    return read_le16(sector + APP_MBR_SIGNATURE_OFFSET) == 0xAA55;
}

static app_sd_fs_t detect_fat_vbr(const uint8_t *sector)
{
    if (!has_boot_sector_signature(sector)) {
        return APP_SD_FS_UNKNOWN;
    }

    if (memcmp(sector + 3, "EXFAT   ", 8) == 0) {
        return APP_SD_FS_EXFAT;
    }

    const uint8_t jump = sector[0];
    if (jump != 0xEB && jump != 0xE9 && jump != 0xE8) {
        return APP_SD_FS_UNKNOWN;
    }

    const uint16_t bytes_per_sector = read_le16(sector + 11);
    const uint8_t sectors_per_cluster = sector[13];
    const uint16_t reserved_sectors = read_le16(sector + 14);
    const uint8_t fat_count = sector[16];
    const uint16_t root_entry_count = read_le16(sector + 17);
    const uint16_t fat16_sectors = read_le16(sector + 22);
    const uint32_t fat32_sectors = read_le32(sector + 36);

    if (!is_power_of_two_u32(bytes_per_sector) ||
        bytes_per_sector < APP_SD_SECTOR_SIZE ||
        bytes_per_sector > 4096 ||
        !is_power_of_two_u32(sectors_per_cluster) ||
        reserved_sectors == 0 ||
        (fat_count != 1 && fat_count != 2)) {
        return APP_SD_FS_UNKNOWN;
    }

    if (memcmp(sector + 82, "FAT32   ", 8) == 0 ||
        (root_entry_count == 0 && fat32_sectors != 0)) {
        return APP_SD_FS_FAT32;
    }

    if (memcmp(sector + 54, "FAT", 3) == 0 ||
        (root_entry_count != 0 && fat16_sectors != 0)) {
        return APP_SD_FS_FAT;
    }

    return APP_SD_FS_UNKNOWN;
}

static bool partition_type_is_empty(const uint8_t *entry)
{
    for (int i = 0; i < 16; i++) {
        if (entry[i] != 0) {
            return false;
        }
    }
    return true;
}

static esp_err_t read_sd_sector(sdmmc_card_t *card, uint32_t sector, uint8_t *buffer)
{
    if (card->csd.sector_size != APP_SD_SECTOR_SIZE) {
        set_last_sd_hint("unsupported SD sector size: %u bytes",
                         (unsigned)card->csd.sector_size);
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t err = sdmmc_read_sectors(card, buffer, sector, 1);
    if (err != ESP_OK) {
        set_last_sd_hint("failed to read SD sector %" PRIu32 ": %s",
                         sector, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t probe_volume_at(sdmmc_card_t *card,
                                 uint64_t start_lba,
                                 uint64_t sector_count,
                                 app_sd_layout_t layout,
                                 uint8_t partition_index,
                                 uint8_t partition_type,
                                 app_sd_volume_t *volume,
                                 bool *saw_exfat)
{
    const uint64_t card_sectors = card->csd.capacity;
    if (start_lba >= card_sectors || sector_count == 0 ||
        sector_count > UINT32_MAX || start_lba > UINT32_MAX ||
        start_lba + sector_count > card_sectors) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t sector[APP_SD_SECTOR_SIZE] = {0};
    esp_err_t err = read_sd_sector(card, (uint32_t)start_lba, sector);
    if (err != ESP_OK) {
        return err;
    }

    app_sd_fs_t fs_type = detect_fat_vbr(sector);
    if (fs_type == APP_SD_FS_EXFAT) {
        if (saw_exfat) {
            *saw_exfat = true;
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (fs_type == APP_SD_FS_UNKNOWN) {
        return ESP_ERR_NOT_FOUND;
    }

    *volume = (app_sd_volume_t) {
        .layout = layout,
        .fs_type = fs_type,
        .start_lba = (uint32_t)start_lba,
        .sector_count = (uint32_t)sector_count,
        .partition_index = partition_index,
        .partition_type = partition_type,
    };
    return ESP_OK;
}

static bool is_gpt_protective_mbr(const uint8_t *sector)
{
    if (!has_boot_sector_signature(sector)) {
        return false;
    }

    for (uint8_t i = 0; i < 4; i++) {
        const uint8_t *entry = sector + APP_MBR_PART_TABLE_OFFSET +
                               (i * APP_MBR_PART_ENTRY_SIZE);
        if (entry[APP_MBR_PART_TYPE_OFFSET] == 0xEE) {
            return true;
        }
    }
    return false;
}

static esp_err_t probe_gpt_volume(sdmmc_card_t *card,
                                  app_sd_volume_t *volume,
                                  bool *saw_exfat)
{
    uint8_t gpt[APP_SD_SECTOR_SIZE] = {0};
    esp_err_t err = read_sd_sector(card, APP_GPT_HEADER_LBA, gpt);
    if (err != ESP_OK) {
        return err;
    }

    if (memcmp(gpt, "EFI PART", 8) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    const uint32_t header_size = read_le32(gpt + 12);
    const uint64_t entries_lba = read_le64(gpt + 72);
    const uint32_t entry_count = read_le32(gpt + 80);
    const uint32_t entry_size = read_le32(gpt + 84);

    if (header_size < 92 || header_size > APP_SD_SECTOR_SIZE ||
        entries_lba == 0 || entries_lba > UINT32_MAX ||
        entry_count == 0 || entry_size < APP_GPT_ENTRY_SIZE ||
        entry_size > APP_SD_SECTOR_SIZE) {
        set_last_sd_hint("invalid GPT header");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t entry_sector[APP_SD_SECTOR_SIZE] = {0};
    uint64_t loaded_sector = UINT64_MAX;
    const uint32_t scan_count = (entry_count < APP_GPT_MAX_SCAN_ENTRIES) ?
                                entry_count : APP_GPT_MAX_SCAN_ENTRIES;

    for (uint32_t i = 0; i < scan_count; i++) {
        const uint64_t entry_byte_offset = (uint64_t)i * entry_size;
        const uint64_t sector_lba = entries_lba + entry_byte_offset / APP_SD_SECTOR_SIZE;
        const uint32_t sector_offset = entry_byte_offset % APP_SD_SECTOR_SIZE;

        if (sector_lba > UINT32_MAX ||
            sector_offset + entry_size > APP_SD_SECTOR_SIZE) {
            continue;
        }

        if (loaded_sector != sector_lba) {
            err = read_sd_sector(card, (uint32_t)sector_lba, entry_sector);
            if (err != ESP_OK) {
                return err;
            }
            loaded_sector = sector_lba;
        }

        const uint8_t *entry = entry_sector + sector_offset;
        if (partition_type_is_empty(entry)) {
            continue;
        }

        const uint64_t first_lba = read_le64(entry + 32);
        const uint64_t last_lba = read_le64(entry + 40);
        if (first_lba == 0 || last_lba < first_lba) {
            continue;
        }

        err = probe_volume_at(card, first_lba, last_lba - first_lba + 1,
                              APP_SD_LAYOUT_GPT, (uint8_t)(i + 1), 0,
                              volume, saw_exfat);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_NOT_SUPPORTED) {
            return err;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t probe_mbr_volume(sdmmc_card_t *card,
                                  const uint8_t *mbr,
                                  app_sd_volume_t *volume,
                                  bool *saw_exfat)
{
    if (!has_boot_sector_signature(mbr)) {
        return ESP_ERR_NOT_FOUND;
    }

    for (uint8_t i = 0; i < 4; i++) {
        const uint8_t *entry = mbr + APP_MBR_PART_TABLE_OFFSET +
                               (i * APP_MBR_PART_ENTRY_SIZE);
        const uint8_t partition_type = entry[APP_MBR_PART_TYPE_OFFSET];
        const uint32_t first_lba = read_le32(entry + APP_MBR_PART_LBA_OFFSET);
        const uint32_t sector_count = read_le32(entry + APP_MBR_PART_SIZE_OFFSET);

        if (partition_type == 0 || first_lba == 0 || sector_count == 0) {
            continue;
        }

        esp_err_t err = probe_volume_at(card, first_lba, sector_count,
                                        APP_SD_LAYOUT_MBR, i + 1, partition_type,
                                        volume, saw_exfat);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_NOT_SUPPORTED) {
            return err;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t probe_sd_volume(sdmmc_card_t *card, app_sd_volume_t *volume)
{
    memset(volume, 0, sizeof(*volume));

    uint8_t sector0[APP_SD_SECTOR_SIZE] = {0};
    esp_err_t err = read_sd_sector(card, 0, sector0);
    if (err != ESP_OK) {
        return err;
    }

    bool saw_exfat = false;
    const app_sd_fs_t sfd_fs = detect_fat_vbr(sector0);
    if (sfd_fs == APP_SD_FS_FAT || sfd_fs == APP_SD_FS_FAT32) {
        if (card->csd.capacity > UINT32_MAX) {
            set_last_sd_hint("raw FAT/FAT32 volume is larger than 32-bit LBA range");
            return ESP_ERR_NOT_SUPPORTED;
        }

        *volume = (app_sd_volume_t) {
            .layout = APP_SD_LAYOUT_SUPER_FLOPPY,
            .fs_type = sfd_fs,
            .start_lba = 0,
            .sector_count = (uint32_t)card->csd.capacity,
        };
        return ESP_OK;
    }
    if (sfd_fs == APP_SD_FS_EXFAT) {
        set_last_sd_hint("SD card is exFAT; this firmware supports FAT/FAT32 only");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const bool has_gpt = is_gpt_protective_mbr(sector0);
    if (has_gpt) {
        err = probe_gpt_volume(card, volume, &saw_exfat);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_NOT_SUPPORTED) {
            return err;
        }
    }

    err = probe_mbr_volume(card, sector0, volume, &saw_exfat);
    if (err == ESP_OK) {
        return ESP_OK;
    }
    if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_NOT_SUPPORTED) {
        return err;
    }

    if (saw_exfat) {
        set_last_sd_hint("found exFAT partition; this firmware supports FAT/FAT32 only");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (has_gpt) {
        set_last_sd_hint("GPT was detected, but no FAT/FAT32 partition VBR was found");
    } else {
        set_last_sd_hint("no FAT/FAT32 volume found in sector 0 or MBR partitions");
    }
    return ESP_ERR_NOT_FOUND;
}

static bool app_sd_disk_is_current(BYTE pdrv)
{
    return s_sd_disk.card != NULL && s_sd_disk.pdrv == pdrv;
}

static DSTATUS app_sd_disk_initialize(BYTE pdrv)
{
    return app_sd_disk_is_current(pdrv) ? 0 : STA_NOINIT;
}

static DSTATUS app_sd_disk_status(BYTE pdrv)
{
    if (!app_sd_disk_is_current(pdrv)) {
        return STA_NOINIT;
    }

    if (!s_sd_disk.disk_status_check) {
        return 0;
    }

    return (sdmmc_get_status(s_sd_disk.card) == ESP_OK) ? 0 : STA_NOINIT;
}

static DRESULT app_sd_disk_read(BYTE pdrv, BYTE *buffer, DWORD sector, UINT count)
{
    if (!app_sd_disk_is_current(pdrv)) {
        return RES_NOTRDY;
    }
    if (count == 0) {
        return RES_OK;
    }
    if ((uint64_t)sector + count > s_sd_disk.sector_count) {
        return RES_PARERR;
    }

    esp_err_t err = sdmmc_read_sectors(s_sd_disk.card, buffer,
                                       s_sd_disk.start_lba + sector, count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD read failed at partition sector %" PRIu32 ": %s",
                 (uint32_t)sector, esp_err_to_name(err));
        return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT app_sd_disk_write(BYTE pdrv, const BYTE *buffer, DWORD sector, UINT count)
{
    if (!app_sd_disk_is_current(pdrv)) {
        return RES_NOTRDY;
    }
    if (count == 0) {
        return RES_OK;
    }
    if ((uint64_t)sector + count > s_sd_disk.sector_count) {
        return RES_PARERR;
    }

    esp_err_t err = sdmmc_write_sectors(s_sd_disk.card, buffer,
                                        s_sd_disk.start_lba + sector, count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD write failed at partition sector %" PRIu32 ": %s",
                 (uint32_t)sector, esp_err_to_name(err));
        return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT app_sd_disk_ioctl(BYTE pdrv, BYTE cmd, void *buffer)
{
    if (!app_sd_disk_is_current(pdrv)) {
        return RES_NOTRDY;
    }

    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *((DWORD *)buffer) = s_sd_disk.sector_count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *((WORD *)buffer) = s_sd_disk.sector_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *((DWORD *)buffer) = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

static void call_sd_host_deinit(const sdmmc_host_t *host)
{
    if (host == NULL) {
        return;
    }

    if ((host->flags & SDMMC_HOST_FLAG_DEINIT_ARG) && host->deinit_p != NULL) {
        host->deinit_p(host->slot);
    } else if (host->deinit != NULL) {
        host->deinit();
    }
}

static esp_err_t init_sdmmc_card(uint8_t bus_width, sdmmc_card_t **out_card)
{
    sdmmc_host_t host = {0};
    sdmmc_slot_config_t slot = {0};
    sdmmc_card_t *card = calloc(1, sizeof(*card));
    bool host_inited = false;

    if (card == NULL) {
        return ESP_ERR_NO_MEM;
    }

    bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &host);
    bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &slot);
    slot.width = bus_width;

    esp_err_t err = host.init();
    if (err != ESP_OK) {
        set_last_sd_hint("SDMMC host init failed in %u-bit mode: %s",
                         bus_width, esp_err_to_name(err));
        goto fail;
    }
    host_inited = true;

    err = sdmmc_host_init_slot(host.slot, &slot);
    if (err != ESP_OK) {
        set_last_sd_hint("SDMMC slot init failed in %u-bit mode: %s",
                         bus_width, esp_err_to_name(err));
        goto fail;
    }

    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) {
        set_last_sd_hint("SD card init failed in %u-bit mode: %s",
                         bus_width, esp_err_to_name(err));
        goto fail;
    }

    *out_card = card;
    return ESP_OK;

fail:
    if (host_inited) {
        call_sd_host_deinit(&host);
    }
    free(card);
    return err;
}

static void cleanup_failed_sd_mount(sdmmc_card_t *card)
{
    if (s_sd_fs != NULL && s_sd_drive[0] != '\0') {
        f_mount(NULL, s_sd_drive, 0);
        esp_vfs_fat_unregister_path(BSP_SD_MOUNT_POINT);
    }

    if (s_sd_disk.pdrv != FF_DRV_NOT_USED) {
        ff_diskio_unregister(s_sd_disk.pdrv);
    }

    memset(&s_sd_disk, 0, sizeof(s_sd_disk));
    s_sd_disk.pdrv = FF_DRV_NOT_USED;
    s_sd_fs = NULL;
    s_sd_drive[0] = '\0';

    if (card != NULL) {
        call_sd_host_deinit(&card->host);
        free(card);
    }
}

static esp_err_t mount_initialized_sd_card(sdmmc_card_t *card,
                                           const app_sd_volume_t *volume,
                                           const esp_vfs_fat_mount_config_t *mount_config)
{
    static const ff_diskio_impl_t diskio = {
        .init = app_sd_disk_initialize,
        .status = app_sd_disk_status,
        .read = app_sd_disk_read,
        .write = app_sd_disk_write,
        .ioctl = app_sd_disk_ioctl,
    };

    BYTE pdrv = FF_DRV_NOT_USED;
    esp_err_t err = ff_diskio_get_drive(&pdrv);
    if (err != ESP_OK || pdrv == FF_DRV_NOT_USED) {
        set_last_sd_hint("no free FatFs physical drive");
        return ESP_ERR_NO_MEM;
    }
    if (pdrv > 9) {
        set_last_sd_hint("FatFs drive number %u is unsupported by this mount path",
                         (unsigned)pdrv);
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_sd_disk = (app_sd_disk_t) {
        .card = card,
        .pdrv = pdrv,
        .start_lba = volume->start_lba,
        .sector_count = volume->sector_count,
        .sector_size = APP_SD_SECTOR_SIZE,
        .disk_status_check = mount_config->disk_status_check_enable,
    };
    ff_diskio_register(pdrv, &diskio);

    s_sd_drive[0] = (char)('0' + pdrv);
    s_sd_drive[1] = ':';
    s_sd_drive[2] = '\0';
    const esp_vfs_fat_conf_t conf = {
        .base_path = BSP_SD_MOUNT_POINT,
        .fat_drive = s_sd_drive,
        .max_files = mount_config->max_files,
    };

    err = esp_vfs_fat_register(&conf, &s_sd_fs);
    if (err != ESP_OK) {
        set_last_sd_hint("esp_vfs_fat_register failed: %s", esp_err_to_name(err));
        cleanup_failed_sd_mount(NULL);
        return err;
    }

    s_last_fresult = f_mount(s_sd_fs, s_sd_drive, 1);
    if (s_last_fresult != FR_OK) {
        set_last_sd_hint("FatFs mount failed with FRESULT=%d", (int)s_last_fresult);
        cleanup_failed_sd_mount(NULL);
        return ESP_FAIL;
    }

    s_sd_card = card;
    s_sd_volume = *volume;
    ESP_LOGI(TAG,
             "SD card mounted at %s (%s %s, start_lba=%" PRIu32 ", sectors=%" PRIu32 ")",
             BSP_SD_MOUNT_POINT,
             sd_layout_name(volume->layout),
             sd_fs_name(volume->fs_type),
             volume->start_lba,
             volume->sector_count);
    return ESP_OK;
}

static esp_err_t mount_sdmmc_with_width(uint8_t bus_width)
{
    const esp_vfs_fat_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = false,
        .use_one_fat = false,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t err = init_sdmmc_card(bus_width, &card);
    if (err != ESP_OK) {
        return err;
    }

    app_sd_volume_t volume = {0};
    err = probe_sd_volume(card, &volume);
    if (err == ESP_OK) {
        err = mount_initialized_sd_card(card, &volume, &mount_config);
    }

    if (err != ESP_OK) {
        cleanup_failed_sd_mount(card);
    }
    return err;
}

static esp_err_t mount_sd_card(void)
{
    s_last_sd_hint[0] = '\0';
    s_last_fresult = FR_OK;

    esp_err_t err = mount_sdmmc_with_width(4);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted at %s (SDMMC 4-bit)", BSP_SD_MOUNT_POINT);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "SDMMC 4-bit mount failed: %s (%s), retrying 1-bit",
             esp_err_to_name(err),
             s_last_sd_hint[0] ? s_last_sd_hint : "no detail");

    err = mount_sdmmc_with_width(1);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SD card mounted at %s (SDMMC 1-bit fallback)", BSP_SD_MOUNT_POINT);
        return ESP_OK;
    }

    s_last_sd_err = err;
    s_media_state = APP_MEDIA_SD_MOUNT_FAILED;
    ESP_LOGE(TAG,
             "Failed to mount SD card: %s. %s. Expected directory: /%s",
             esp_err_to_name(err),
             s_last_sd_hint[0] ? s_last_sd_hint : "Use a FAT/FAT32-formatted card",
             APP_SD_GIF_SUBDIR);
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
        if (s_last_fresult != FR_OK) {
            dbg_console_printf("last FatFs result: %d\n", (int)s_last_fresult);
        }
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

#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "dirent.h"
#include "diskio_impl.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "sdmmc_cmd.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp/touch.h"
#include "bsp_err_check.h"

#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_codec_dev_defaults.h"
#include "iot_button.h"

static const char *TAG = "esp32_s3_touch_lcd_1_85B";

/**
 * @brief I2C handle for BSP usage
 *
 * In IDF v5.4 you can call i2c_master_get_bus_handle(BSP_I2C_NUM, i2c_master_bus_handle_t *ret_handle)
 * from #include "esp_private/i2c_platform.h" to get this handle
 *
 * For IDF 5.2 and 5.3 you must call bsp_i2c_get_handle()
 */
static i2c_master_bus_handle_t i2c_handle = NULL;
static i2c_bus_handle_t i2c_bus = NULL;
static bool i2c_initialized = false;
static sdmmc_card_t *bsp_sdcard = NULL;    // Global uSD card handler
static esp_lcd_touch_handle_t tp;   // LCD touch handle
static esp_lcd_panel_handle_t panel_handle = NULL; // LCD panel handle
static esp_lcd_panel_io_handle_t io_handle = NULL;
#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static lv_indev_t *disp_indev = NULL;
static lv_display_t *disp_drv = NULL;
#endif // (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static uint8_t brightness;
static i2s_chan_handle_t i2s_tx_chan = NULL;
static i2s_chan_handle_t i2s_rx_chan = NULL;
static const audio_codec_data_if_t *i2s_data_if = NULL;  /* Codec data interface */

#define LCD_OPCODE_READ_CMD         (0x0BULL)
#define LCD_LEDC_CH            CONFIG_BSP_DISPLAY_BRIGHTNESS_LEDC_CH

#define BSP_SD_SECTOR_SIZE         512U
#define BSP_SD_FATFS_DRIVE_STR_MAX 3U
#define BSP_MBR_PART_TABLE_OFFSET  446U
#define BSP_MBR_PART_ENTRY_SIZE    16U
#define BSP_MBR_PART_TYPE_OFFSET   4U
#define BSP_MBR_PART_LBA_OFFSET    8U
#define BSP_MBR_PART_SIZE_OFFSET   12U
#define BSP_MBR_SIGNATURE_OFFSET   510U
#define BSP_GPT_HEADER_LBA         1U
#define BSP_GPT_MAX_SCAN_ENTRIES   128U
#define BSP_GPT_ENTRY_SIZE         128U

typedef enum {
    BSP_SD_LAYOUT_UNKNOWN = 0,
    BSP_SD_LAYOUT_SUPER_FLOPPY,
    BSP_SD_LAYOUT_MBR,
    BSP_SD_LAYOUT_GPT,
} bsp_sd_layout_t;

typedef enum {
    BSP_SD_FS_UNKNOWN = 0,
    BSP_SD_FS_FAT,
    BSP_SD_FS_FAT32,
    BSP_SD_FS_EXFAT,
} bsp_sd_fs_t;

typedef struct {
    bsp_sd_layout_t layout;
    bsp_sd_fs_t fs_type;
    uint32_t start_lba;
    uint32_t sector_count;
    uint8_t partition_index;
    uint8_t partition_type;
} bsp_sd_volume_t;

typedef struct {
    sdmmc_card_t *card;
    BYTE pdrv;
    uint32_t start_lba;
    uint32_t sector_count;
    uint16_t sector_size;
    bool disk_status_check;
} bsp_sd_disk_t;

static bsp_sd_disk_t bsp_sd_disk = {
    .pdrv = FF_DRV_NOT_USED,
};
static FATFS *bsp_sd_fs = NULL;
static char bsp_sd_drive[BSP_SD_FATFS_DRIVE_STR_MAX];
static bool bsp_sd_uses_partition_diskio = false;

/* Can be used for i2s_std_gpio_config_t and/or i2s_std_config_t initialization */
#define BSP_ES7210_CODEC_ADDR ES7210_CODEC_DEFAULT_ADDR
#define BSP_I2S_GPIO_CFG       \
    {                          \
        .mclk = BSP_I2S_MCLK,  \
        .bclk = BSP_I2S_SCLK,  \
        .ws = BSP_I2S_LCLK,    \
        .dout = BSP_I2S_DOUT,  \
        .din = BSP_I2S_DSIN,   \
        .invert_flags = {      \
            .mclk_inv = false, \
            .bclk_inv = false, \
            .ws_inv = false,   \
        },                     \
    }

/* This configuration is used by default in bsp_audio_init() */
#define BSP_I2S_DUPLEX_MONO_CFG(_sample_rate)                                                         \
    {                                                                                                 \
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(_sample_rate),                                          \
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO), \
        .gpio_cfg = BSP_I2S_GPIO_CFG,                                                                 \
    }

    static const st77916_lcd_init_cmd_t vendor_specific_init_version_1[] = {
    {0xF0, (uint8_t []){0x28}, 1, 0},
    {0xF2, (uint8_t []){0x28}, 1, 0},
    {0x7C, (uint8_t []){0xD1}, 1, 0},
    {0x83, (uint8_t []){0xE0}, 1, 0},
    {0x84, (uint8_t []){0x61}, 1, 0},
    {0xF2, (uint8_t []){0x82}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x01}, 1, 0},
    {0xF1, (uint8_t []){0x01}, 1, 0},
    {0xB0, (uint8_t []){0x49}, 1, 0},
    {0xB1, (uint8_t []){0x4A}, 1, 0},
    {0xB2, (uint8_t []){0x1F}, 1, 0},
    {0xB4, (uint8_t []){0x46}, 1, 0},
    {0xB5, (uint8_t []){0x34}, 1, 0},
    {0xB6, (uint8_t []){0xD5}, 1, 0},
    {0xB7, (uint8_t []){0x30}, 1, 0},
    {0xB8, (uint8_t []){0x04}, 1, 0},
    {0xBA, (uint8_t []){0x00}, 1, 0},
    {0xBB, (uint8_t []){0x08}, 1, 0},
    {0xBC, (uint8_t []){0x08}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xC0, (uint8_t []){0x80}, 1, 0},
    {0xC1, (uint8_t []){0x10}, 1, 0},
    {0xC2, (uint8_t []){0x37}, 1, 0},
    {0xC3, (uint8_t []){0x80}, 1, 0},
    {0xC4, (uint8_t []){0x10}, 1, 0},
    {0xC5, (uint8_t []){0x37}, 1, 0},
    {0xC6, (uint8_t []){0xA9}, 1, 0},
    {0xC7, (uint8_t []){0x41}, 1, 0},
    {0xC8, (uint8_t []){0x01}, 1, 0},
    {0xC9, (uint8_t []){0xA9}, 1, 0},
    {0xCA, (uint8_t []){0x41}, 1, 0},
    {0xCB, (uint8_t []){0x01}, 1, 0},
    {0xD0, (uint8_t []){0x91}, 1, 0},
    {0xD1, (uint8_t []){0x68}, 1, 0},
    {0xD2, (uint8_t []){0x68}, 1, 0},
    {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
    // {0xDD, (uint8_t []){0x35}, 1, 0},
    // {0xDE, (uint8_t []){0x35}, 1, 0},
    // {0xDD, (uint8_t []){0x3F}, 1, 0},
    // {0xDE, (uint8_t []){0x3F}, 1, 0},
    {0xF1, (uint8_t []){0x10}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    {0xF0, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x70, 0x09, 0x12, 0x0C, 0x0B, 0x27, 0x38, 0x54, 0x4E, 0x19, 0x15, 0x15, 0x2C, 0x2F}, 14, 0},
    {0xE1, (uint8_t []){0x70, 0x08, 0x11, 0x0C, 0x0B, 0x27, 0x38, 0x43, 0x4C, 0x18, 0x14, 0x14, 0x2B, 0x2D}, 14, 0},
    // {0xE0, (uint8_t []){0xF0, 0x0E, 0x15, 0x0B, 0x0B, 0x07, 0x3C, 0x44, 0x51, 0x38, 0x15, 0x15, 0x32, 0x36}, 14, 0},
    // {0xE1, (uint8_t []){0xF0, 0x0D, 0x15, 0x0A, 0x0A, 0x26, 0x3B, 0x43, 0x50, 0x37, 0x14, 0x15, 0x31, 0x36}, 14, 0},
    {0xF0, (uint8_t []){0x10}, 1, 0},
    {0xF3, (uint8_t []){0x10}, 1, 0},
    {0xE0, (uint8_t []){0x08}, 1, 0},
    {0xE1, (uint8_t []){0x00}, 1, 0},
    {0xE2, (uint8_t []){0x0B}, 1, 0},
    {0xE3, (uint8_t []){0x00}, 1, 0},
    {0xE4, (uint8_t []){0xE0}, 1, 0},
    {0xE5, (uint8_t []){0x06}, 1, 0},
    {0xE6, (uint8_t []){0x21}, 1, 0},
    {0xE7, (uint8_t []){0x00}, 1, 0},
    {0xE8, (uint8_t []){0x05}, 1, 0},
    {0xE9, (uint8_t []){0x82}, 1, 0},
    {0xEA, (uint8_t []){0xDF}, 1, 0},
    {0xEB, (uint8_t []){0x89}, 1, 0},
    {0xEC, (uint8_t []){0x20}, 1, 0},
    {0xED, (uint8_t []){0x14}, 1, 0},
    {0xEE, (uint8_t []){0xFF}, 1, 0},
    {0xEF, (uint8_t []){0x00}, 1, 0},
    {0xF8, (uint8_t []){0xFF}, 1, 0},
    {0xF9, (uint8_t []){0x00}, 1, 0},
    {0xFA, (uint8_t []){0x00}, 1, 0},
    {0xFB, (uint8_t []){0x30}, 1, 0},
    {0xFC, (uint8_t []){0x00}, 1, 0},
    {0xFD, (uint8_t []){0x00}, 1, 0},
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0xFF, (uint8_t []){0x00}, 1, 0},
    {0x60, (uint8_t []){0x42}, 1, 0},
    {0x61, (uint8_t []){0xE0}, 1, 0},
    {0x62, (uint8_t []){0x40}, 1, 0},
    {0x63, (uint8_t []){0x40}, 1, 0},
    {0x64, (uint8_t []){0x02}, 1, 0},
    {0x65, (uint8_t []){0x00}, 1, 0},
    {0x66, (uint8_t []){0x40}, 1, 0},
    {0x67, (uint8_t []){0x03}, 1, 0},
    {0x68, (uint8_t []){0x00}, 1, 0},
    {0x69, (uint8_t []){0x00}, 1, 0},
    {0x6A, (uint8_t []){0x00}, 1, 0},
    {0x6B, (uint8_t []){0x00}, 1, 0},
    {0x70, (uint8_t []){0x42}, 1, 0},
    {0x71, (uint8_t []){0xE0}, 1, 0},
    {0x72, (uint8_t []){0x40}, 1, 0},
    {0x73, (uint8_t []){0x40}, 1, 0},
    {0x74, (uint8_t []){0x02}, 1, 0},
    {0x75, (uint8_t []){0x00}, 1, 0},
    {0x76, (uint8_t []){0x40}, 1, 0},
    {0x77, (uint8_t []){0x03}, 1, 0},
    {0x78, (uint8_t []){0x00}, 1, 0},
    {0x79, (uint8_t []){0x00}, 1, 0},
    {0x7A, (uint8_t []){0x00}, 1, 0},
    {0x7B, (uint8_t []){0x00}, 1, 0},
    // {0x80, (uint8_t []){0x38}, 1, 0},
    {0x80, (uint8_t []){0x38}, 1, 0},
    {0x81, (uint8_t []){0x00}, 1, 0},
    // {0x82, (uint8_t []){0x04}, 1, 0},
    {0x82, (uint8_t []){0x04}, 1, 0},
    {0x83, (uint8_t []){0x02}, 1, 0},
    // {0x84, (uint8_t []){0xDC}, 1, 0},
    {0x84, (uint8_t []){0xDC}, 1, 0},
    {0x85, (uint8_t []){0x00}, 1, 0},
    {0x86, (uint8_t []){0x00}, 1, 0},
    {0x87, (uint8_t []){0x00}, 1, 0},
    // {0x88, (uint8_t []){0x38}, 1, 0},
    {0x88, (uint8_t []){0x38}, 1, 0},
    {0x89, (uint8_t []){0x00}, 1, 0},
    // {0x8A, (uint8_t []){0x06}, 1, 0},
    {0x8A, (uint8_t []){0x06}, 1, 0},
    {0x8B, (uint8_t []){0x02}, 1, 0},
    // {0x8C, (uint8_t []){0xDE}, 1, 0},
    {0x8C, (uint8_t []){0xDE}, 1, 0},
    {0x8D, (uint8_t []){0x00}, 1, 0},
    {0x8E, (uint8_t []){0x00}, 1, 0},
    {0x8F, (uint8_t []){0x00}, 1, 0},
    // {0x90, (uint8_t []){0x38}, 1, 0},
    {0x90, (uint8_t []){0x38}, 1, 0},
    {0x91, (uint8_t []){0x00}, 1, 0},
    // {0x92, (uint8_t []){0x08}, 1, 0},
    {0x92, (uint8_t []){0x08}, 1, 0},
    {0x93, (uint8_t []){0x02}, 1, 0},
    // {0x94, (uint8_t []){0xE0}, 1, 0},
    {0x94, (uint8_t []){0xE0}, 1, 0},
    {0x95, (uint8_t []){0x00}, 1, 0},
    {0x96, (uint8_t []){0x00}, 1, 0},
    {0x97, (uint8_t []){0x00}, 1, 0},
    // {0x98, (uint8_t []){0x38}, 1, 0},
    {0x98, (uint8_t []){0x38}, 1, 0},
    {0x99, (uint8_t []){0x00}, 1, 0},
    // {0x9A, (uint8_t []){0x0A}, 1, 0},
    {0x9A, (uint8_t []){0x0A}, 1, 0},
    {0x9B, (uint8_t []){0x02}, 1, 0},
    // {0x9C, (uint8_t []){0xE2}, 1, 0},
    {0x9C, (uint8_t []){0xE2}, 1, 0},
    {0x9D, (uint8_t []){0x00}, 1, 0},
    {0x9E, (uint8_t []){0x00}, 1, 0},
    {0x9F, (uint8_t []){0x00}, 1, 0},
    // {0xA0, (uint8_t []){0x38}, 1, 0},
    {0xA0, (uint8_t []){0x38}, 1, 0},
    {0xA1, (uint8_t []){0x00}, 1, 0},
    // {0xA2, (uint8_t []){0x03}, 1, 0},
    {0xA2, (uint8_t []){0x03}, 1, 0},
    {0xA3, (uint8_t []){0x02}, 1, 0},
    // {0xA4, (uint8_t []){0xDB}, 1, 0},
    {0xA4, (uint8_t []){0xDB}, 1, 0},
    {0xA5, (uint8_t []){0x00}, 1, 0},
    {0xA6, (uint8_t []){0x00}, 1, 0},
    {0xA7, (uint8_t []){0x00}, 1, 0},
    // {0xA8, (uint8_t []){0x38}, 1, 0},
    {0xA8, (uint8_t []){0x38}, 1, 0},
    {0xA9, (uint8_t []){0x00}, 1, 0},
    // {0xAA, (uint8_t []){0x05}, 1, 0},
    {0xAA, (uint8_t []){0x05}, 1, 0},
    {0xAB, (uint8_t []){0x02}, 1, 0},
    // {0xAC, (uint8_t []){0xDD}, 1, 0},
    {0xAC, (uint8_t []){0xDD}, 1, 0},
    {0xAD, (uint8_t []){0x00}, 1, 0},
    {0xAE, (uint8_t []){0x00}, 1, 0},
    {0xAF, (uint8_t []){0x00}, 1, 0},
    // {0xB0, (uint8_t []){0x38}, 1, 0},
    {0xB0, (uint8_t []){0x38}, 1, 0},
    {0xB1, (uint8_t []){0x00}, 1, 0},
    // {0xB2, (uint8_t []){0x07}, 1, 0},
    {0xB2, (uint8_t []){0x07}, 1, 0},
    {0xB3, (uint8_t []){0x02}, 1, 0},
    // {0xB4, (uint8_t []){0xDF}, 1, 0},
    {0xB4, (uint8_t []){0xDF}, 1, 0},
    {0xB5, (uint8_t []){0x00}, 1, 0},
    {0xB6, (uint8_t []){0x00}, 1, 0},
    {0xB7, (uint8_t []){0x00}, 1, 0},
    // {0xB8, (uint8_t []){0x38}, 1, 0},
    {0xB8, (uint8_t []){0x38}, 1, 0},
    {0xB9, (uint8_t []){0x00}, 1, 0},
    // {0xBA, (uint8_t []){0x09}, 1, 0},
    {0xBA, (uint8_t []){0x09}, 1, 0},
    {0xBB, (uint8_t []){0x02}, 1, 0},
    // {0xBC, (uint8_t []){0xE1}, 1, 0},
    {0xBC, (uint8_t []){0xE1}, 1, 0},
    {0xBD, (uint8_t []){0x00}, 1, 0},
    {0xBE, (uint8_t []){0x00}, 1, 0},
    {0xBF, (uint8_t []){0x00}, 1, 0},
    // {0xC0, (uint8_t []){0x22}, 1, 0},
    {0xC0, (uint8_t []){0x22}, 1, 0},
    {0xC1, (uint8_t []){0xAA}, 1, 0},
    {0xC2, (uint8_t []){0x65}, 1, 0},
    {0xC3, (uint8_t []){0x74}, 1, 0},
    {0xC4, (uint8_t []){0x47}, 1, 0},
    {0xC5, (uint8_t []){0x56}, 1, 0},
    {0xC6, (uint8_t []){0x00}, 1, 0},
    {0xC7, (uint8_t []){0x88}, 1, 0},
    {0xC8, (uint8_t []){0x99}, 1, 0},
    {0xC9, (uint8_t []){0x33}, 1, 0},
    // {0xD0, (uint8_t []){0x11}, 1, 0},
    {0xD0, (uint8_t []){0x11}, 1, 0},
    {0xD1, (uint8_t []){0xAA}, 1, 0},
    {0xD2, (uint8_t []){0x65}, 1, 0},
    {0xD3, (uint8_t []){0x74}, 1, 0},
    {0xD4, (uint8_t []){0x47}, 1, 0},
    {0xD5, (uint8_t []){0x56}, 1, 0},
    {0xD6, (uint8_t []){0x00}, 1, 0},
    {0xD7, (uint8_t []){0x88}, 1, 0},
    {0xD8, (uint8_t []){0x99}, 1, 0},
    {0xD9, (uint8_t []){0x33}, 1, 0},
    {0xF3, (uint8_t []){0x01}, 1, 0},
    {0xF0, (uint8_t []){0x00}, 1, 0},
    // {0x3A, (uint8_t []){0x55}, 1, 0},
    {0x35, (uint8_t []){0x00}, 1, 0},
    {0x21, (uint8_t []){0x00}, 0, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x29, (uint8_t []){0x00}, 0, 0},
};
static const st77916_lcd_init_cmd_t vendor_specific_init_version_2[] = {
  {0xF0, (uint8_t []){0x28}, 1, 0},
  {0xF2, (uint8_t []){0x28}, 1, 0},
  {0x73, (uint8_t []){0xF0}, 1, 0},
  {0x7C, (uint8_t []){0xD1}, 1, 0},
  {0x83, (uint8_t []){0xE0}, 1, 0},
  {0x84, (uint8_t []){0x61}, 1, 0},
  {0xF2, (uint8_t []){0x82}, 1, 0},
  {0xF0, (uint8_t []){0x00}, 1, 0},
  {0xF0, (uint8_t []){0x01}, 1, 0},
  {0xF1, (uint8_t []){0x01}, 1, 0},
  {0xB0, (uint8_t []){0x56}, 1, 0},
  {0xB1, (uint8_t []){0x4D}, 1, 0},
  {0xB2, (uint8_t []){0x24}, 1, 0},
  {0xB4, (uint8_t []){0x87}, 1, 0},
  {0xB5, (uint8_t []){0x44}, 1, 0},
  {0xB6, (uint8_t []){0x8B}, 1, 0},
  {0xB7, (uint8_t []){0x40}, 1, 0},
  {0xB8, (uint8_t []){0x86}, 1, 0},
  {0xBA, (uint8_t []){0x00}, 1, 0},
  {0xBB, (uint8_t []){0x08}, 1, 0},
  {0xBC, (uint8_t []){0x08}, 1, 0},
  {0xBD, (uint8_t []){0x00}, 1, 0},
  {0xC0, (uint8_t []){0x80}, 1, 0},
  {0xC1, (uint8_t []){0x10}, 1, 0},
  {0xC2, (uint8_t []){0x37}, 1, 0},
  {0xC3, (uint8_t []){0x80}, 1, 0},
  {0xC4, (uint8_t []){0x10}, 1, 0},
  {0xC5, (uint8_t []){0x37}, 1, 0},
  {0xC6, (uint8_t []){0xA9}, 1, 0},
  {0xC7, (uint8_t []){0x41}, 1, 0},
  {0xC8, (uint8_t []){0x01}, 1, 0},
  {0xC9, (uint8_t []){0xA9}, 1, 0},
  {0xCA, (uint8_t []){0x41}, 1, 0},
  {0xCB, (uint8_t []){0x01}, 1, 0},
  {0xD0, (uint8_t []){0x91}, 1, 0},
  {0xD1, (uint8_t []){0x68}, 1, 0},
  {0xD2, (uint8_t []){0x68}, 1, 0},
  {0xF5, (uint8_t []){0x00, 0xA5}, 2, 0},
  {0xDD, (uint8_t []){0x4F}, 1, 0},
  {0xDE, (uint8_t []){0x4F}, 1, 0},
  {0xF1, (uint8_t []){0x10}, 1, 0},
  {0xF0, (uint8_t []){0x00}, 1, 0},
  {0xF0, (uint8_t []){0x02}, 1, 0},
  {0xE0, (uint8_t []){0xF0, 0x0A, 0x10, 0x09, 0x09, 0x36, 0x35, 0x33, 0x4A, 0x29, 0x15, 0x15, 0x2E, 0x34}, 14, 0},
  {0xE1, (uint8_t []){0xF0, 0x0A, 0x0F, 0x08, 0x08, 0x05, 0x34, 0x33, 0x4A, 0x39, 0x15, 0x15, 0x2D, 0x33}, 14, 0},
  {0xF0, (uint8_t []){0x10}, 1, 0},
  {0xF3, (uint8_t []){0x10}, 1, 0},
  {0xE0, (uint8_t []){0x07}, 1, 0},
  {0xE1, (uint8_t []){0x00}, 1, 0},
  {0xE2, (uint8_t []){0x00}, 1, 0},
  {0xE3, (uint8_t []){0x00}, 1, 0},
  {0xE4, (uint8_t []){0xE0}, 1, 0},
  {0xE5, (uint8_t []){0x06}, 1, 0},
  {0xE6, (uint8_t []){0x21}, 1, 0},
  {0xE7, (uint8_t []){0x01}, 1, 0},
  {0xE8, (uint8_t []){0x05}, 1, 0},
  {0xE9, (uint8_t []){0x02}, 1, 0},
  {0xEA, (uint8_t []){0xDA}, 1, 0},
  {0xEB, (uint8_t []){0x00}, 1, 0},
  {0xEC, (uint8_t []){0x00}, 1, 0},
  {0xED, (uint8_t []){0x0F}, 1, 0},
  {0xEE, (uint8_t []){0x00}, 1, 0},
  {0xEF, (uint8_t []){0x00}, 1, 0},
  {0xF8, (uint8_t []){0x00}, 1, 0},
  {0xF9, (uint8_t []){0x00}, 1, 0},
  {0xFA, (uint8_t []){0x00}, 1, 0},
  {0xFB, (uint8_t []){0x00}, 1, 0},
  {0xFC, (uint8_t []){0x00}, 1, 0},
  {0xFD, (uint8_t []){0x00}, 1, 0},
  {0xFE, (uint8_t []){0x00}, 1, 0},
  {0xFF, (uint8_t []){0x00}, 1, 0},
  {0x60, (uint8_t []){0x40}, 1, 0},
  {0x61, (uint8_t []){0x04}, 1, 0},
  {0x62, (uint8_t []){0x00}, 1, 0},
  {0x63, (uint8_t []){0x42}, 1, 0},
  {0x64, (uint8_t []){0xD9}, 1, 0},
  {0x65, (uint8_t []){0x00}, 1, 0},
  {0x66, (uint8_t []){0x00}, 1, 0},
  {0x67, (uint8_t []){0x00}, 1, 0},
  {0x68, (uint8_t []){0x00}, 1, 0},
  {0x69, (uint8_t []){0x00}, 1, 0},
  {0x6A, (uint8_t []){0x00}, 1, 0},
  {0x6B, (uint8_t []){0x00}, 1, 0},
  {0x70, (uint8_t []){0x40}, 1, 0},
  {0x71, (uint8_t []){0x03}, 1, 0},
  {0x72, (uint8_t []){0x00}, 1, 0},
  {0x73, (uint8_t []){0x42}, 1, 0},
  {0x74, (uint8_t []){0xD8}, 1, 0},
  {0x75, (uint8_t []){0x00}, 1, 0},
  {0x76, (uint8_t []){0x00}, 1, 0},
  {0x77, (uint8_t []){0x00}, 1, 0},
  {0x78, (uint8_t []){0x00}, 1, 0},
  {0x79, (uint8_t []){0x00}, 1, 0},
  {0x7A, (uint8_t []){0x00}, 1, 0},
  {0x7B, (uint8_t []){0x00}, 1, 0},
  {0x80, (uint8_t []){0x48}, 1, 0},
  {0x81, (uint8_t []){0x00}, 1, 0},
  {0x82, (uint8_t []){0x06}, 1, 0},
  {0x83, (uint8_t []){0x02}, 1, 0},
  {0x84, (uint8_t []){0xD6}, 1, 0},
  {0x85, (uint8_t []){0x04}, 1, 0},
  {0x86, (uint8_t []){0x00}, 1, 0},
  {0x87, (uint8_t []){0x00}, 1, 0},
  {0x88, (uint8_t []){0x48}, 1, 0},
  {0x89, (uint8_t []){0x00}, 1, 0},
  {0x8A, (uint8_t []){0x08}, 1, 0},
  {0x8B, (uint8_t []){0x02}, 1, 0},
  {0x8C, (uint8_t []){0xD8}, 1, 0},
  {0x8D, (uint8_t []){0x04}, 1, 0},
  {0x8E, (uint8_t []){0x00}, 1, 0},
  {0x8F, (uint8_t []){0x00}, 1, 0},
  {0x90, (uint8_t []){0x48}, 1, 0},
  {0x91, (uint8_t []){0x00}, 1, 0},
  {0x92, (uint8_t []){0x0A}, 1, 0},
  {0x93, (uint8_t []){0x02}, 1, 0},
  {0x94, (uint8_t []){0xDA}, 1, 0},
  {0x95, (uint8_t []){0x04}, 1, 0},
  {0x96, (uint8_t []){0x00}, 1, 0},
  {0x97, (uint8_t []){0x00}, 1, 0},
  {0x98, (uint8_t []){0x48}, 1, 0},
  {0x99, (uint8_t []){0x00}, 1, 0},
  {0x9A, (uint8_t []){0x0C}, 1, 0},
  {0x9B, (uint8_t []){0x02}, 1, 0},
  {0x9C, (uint8_t []){0xDC}, 1, 0},
  {0x9D, (uint8_t []){0x04}, 1, 0},
  {0x9E, (uint8_t []){0x00}, 1, 0},
  {0x9F, (uint8_t []){0x00}, 1, 0},
  {0xA0, (uint8_t []){0x48}, 1, 0},
  {0xA1, (uint8_t []){0x00}, 1, 0},
  {0xA2, (uint8_t []){0x05}, 1, 0},
  {0xA3, (uint8_t []){0x02}, 1, 0},
  {0xA4, (uint8_t []){0xD5}, 1, 0},
  {0xA5, (uint8_t []){0x04}, 1, 0},
  {0xA6, (uint8_t []){0x00}, 1, 0},
  {0xA7, (uint8_t []){0x00}, 1, 0},
  {0xA8, (uint8_t []){0x48}, 1, 0},
  {0xA9, (uint8_t []){0x00}, 1, 0},
  {0xAA, (uint8_t []){0x07}, 1, 0},
  {0xAB, (uint8_t []){0x02}, 1, 0},
  {0xAC, (uint8_t []){0xD7}, 1, 0},
  {0xAD, (uint8_t []){0x04}, 1, 0},
  {0xAE, (uint8_t []){0x00}, 1, 0},
  {0xAF, (uint8_t []){0x00}, 1, 0},
  {0xB0, (uint8_t []){0x48}, 1, 0},
  {0xB1, (uint8_t []){0x00}, 1, 0},
  {0xB2, (uint8_t []){0x09}, 1, 0},
  {0xB3, (uint8_t []){0x02}, 1, 0},
  {0xB4, (uint8_t []){0xD9}, 1, 0},
  {0xB5, (uint8_t []){0x04}, 1, 0},
  {0xB6, (uint8_t []){0x00}, 1, 0},
  {0xB7, (uint8_t []){0x00}, 1, 0},
  
  {0xB8, (uint8_t []){0x48}, 1, 0},
  {0xB9, (uint8_t []){0x00}, 1, 0},
  {0xBA, (uint8_t []){0x0B}, 1, 0},
  {0xBB, (uint8_t []){0x02}, 1, 0},
  {0xBC, (uint8_t []){0xDB}, 1, 0},
  {0xBD, (uint8_t []){0x04}, 1, 0},
  {0xBE, (uint8_t []){0x00}, 1, 0},
  {0xBF, (uint8_t []){0x00}, 1, 0},
  {0xC0, (uint8_t []){0x10}, 1, 0},
  {0xC1, (uint8_t []){0x47}, 1, 0},
  {0xC2, (uint8_t []){0x56}, 1, 0},
  {0xC3, (uint8_t []){0x65}, 1, 0},
  {0xC4, (uint8_t []){0x74}, 1, 0},
  {0xC5, (uint8_t []){0x88}, 1, 0},
  {0xC6, (uint8_t []){0x99}, 1, 0},
  {0xC7, (uint8_t []){0x01}, 1, 0},
  {0xC8, (uint8_t []){0xBB}, 1, 0},
  {0xC9, (uint8_t []){0xAA}, 1, 0},
  {0xD0, (uint8_t []){0x10}, 1, 0},
  {0xD1, (uint8_t []){0x47}, 1, 0},
  {0xD2, (uint8_t []){0x56}, 1, 0},
  {0xD3, (uint8_t []){0x65}, 1, 0},
  {0xD4, (uint8_t []){0x74}, 1, 0},
  {0xD5, (uint8_t []){0x88}, 1, 0},
  {0xD6, (uint8_t []){0x99}, 1, 0},
  {0xD7, (uint8_t []){0x01}, 1, 0},
  {0xD8, (uint8_t []){0xBB}, 1, 0},
  {0xD9, (uint8_t []){0xAA}, 1, 0},
  {0xF3, (uint8_t []){0x01}, 1, 0},
  {0xF0, (uint8_t []){0x00}, 1, 0},
  {0x35, (uint8_t []){0x00}, 1, 0},
  {0x21, (uint8_t []){0x00}, 1, 0},
  {0x11, (uint8_t []){0x00}, 1, 120},
  {0x29, (uint8_t []){0x00}, 1, 0},  
};

esp_err_t bsp_i2c_init(void)
{
    /* I2C was initialized before */
    if (i2c_initialized)
    {
        return ESP_OK;
    }

    i2c_master_bus_config_t i2c_bus_conf = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .i2c_port = BSP_I2C_NUM,
    };
    BSP_ERROR_CHECK_RETURN_ERR(i2c_new_master_bus(&i2c_bus_conf, &i2c_handle));

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = BSP_I2C_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = BSP_I2C_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = CONFIG_BSP_I2C_CLK_SPEED_HZ,
    };
    i2c_bus = i2c_bus_create(BSP_I2C_NUM, &conf);
    if (i2c_bus == NULL) {
        ESP_LOGE(TAG, "Failed to create i2c_bus wrapper");
        if (i2c_handle != NULL) {
            i2c_del_master_bus(i2c_handle);
            i2c_handle = NULL;
        }
        return ESP_FAIL;
    }

    //i2c_handle = i2c_bus_get_internal_bus_handle(i2c_bus);
    i2c_initialized = true;

    return ESP_OK;
}

esp_err_t bsp_i2c_deinit(void)
{
    esp_err_t ret = ESP_OK;
    if (i2c_bus != NULL) {
        ret = i2c_bus_delete(&i2c_bus);
    } else if (i2c_handle != NULL) {
        ret = i2c_del_master_bus(i2c_handle);
    }
    BSP_ERROR_CHECK_RETURN_ERR(ret);
    i2c_handle = NULL;
    i2c_initialized = false;
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)
{
    bsp_i2c_init();
    return i2c_handle;
}
i2c_bus_handle_t bsp_i2c_bus_get_handle(void)
{
    bsp_i2c_init();
    return i2c_bus;
}
sdmmc_card_t *bsp_sdcard_get_handle(void)
{
    return bsp_sdcard;
}

esp_err_t bsp_iot_button_create(button_handle_t btn_array[], int *btn_cnt, int btn_array_size)
{
    if (btn_array == NULL || btn_array_size < BSP_BUTTON_NUM) {
        return ESP_ERR_INVALID_ARG;
    }

    const button_config_t btn_cfg = {0};
    const button_gpio_config_t gpio_cfg = {
        .gpio_num = BSP_BUTTONS_IO_0,
        .active_level = 0,
        .enable_power_save = false,
        .disable_pull = false,
    };

    esp_err_t ret = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &btn_array[BSP_BUTTON_BOOT]);
    if (ret != ESP_OK) {
        return ret;
    }

    if (btn_cnt) {
        *btn_cnt = BSP_BUTTON_NUM;
    }

    return ESP_OK;
}

void bsp_sdcard_get_sdmmc_host(const int slot, sdmmc_host_t *config)
{
    assert(config);

    sdmmc_host_t host_config = SDMMC_HOST_DEFAULT();

    memcpy(config, &host_config, sizeof(sdmmc_host_t));
}

void bsp_sdcard_get_sdspi_host(const int slot, sdmmc_host_t *config)
{
    assert(config);
    memset(config, 0, sizeof(sdmmc_host_t));
    ESP_LOGE(TAG, "SD card SPI mode is not supported by HW!");
}

void bsp_sdcard_sdmmc_get_slot(const int slot, sdmmc_slot_config_t *config)
{
    assert(config);
    memset(config, 0, sizeof(sdmmc_slot_config_t));

    config->clk = BSP_SD_CLK;
    config->cmd = BSP_SD_CMD;
    config->d0 = BSP_SD_D0;
    config->d1 = BSP_SD_D1;
    config->d2 = BSP_SD_D2;
    config->d3 = BSP_SD_D3;
    config->d4 = GPIO_NUM_NC;
    config->d5 = GPIO_NUM_NC;
    config->d6 = GPIO_NUM_NC;
    config->d7 = GPIO_NUM_NC;
    config->cd = SDMMC_SLOT_NO_CD;
    config->wp = SDMMC_SLOT_NO_WP;
    config->width = 4;
    config->flags = 0;
}

void bsp_sdcard_sdspi_get_slot(const spi_host_device_t spi_host, sdspi_device_config_t *config)
{
    assert(config);
    memset(config, 0, sizeof(sdspi_device_config_t));
    ESP_LOGE(TAG, "SD card SPI mode is not supported by HW!");
}

static const char *bsp_sd_layout_name(bsp_sd_layout_t layout)
{
    switch (layout) {
    case BSP_SD_LAYOUT_SUPER_FLOPPY:
        return "SFD";
    case BSP_SD_LAYOUT_MBR:
        return "MBR";
    case BSP_SD_LAYOUT_GPT:
        return "GPT";
    case BSP_SD_LAYOUT_UNKNOWN:
    default:
        return "unknown";
    }
}

static const char *bsp_sd_fs_name(bsp_sd_fs_t fs_type)
{
    switch (fs_type) {
    case BSP_SD_FS_FAT32:
        return "FAT32";
    case BSP_SD_FS_FAT:
        return "FAT";
    case BSP_SD_FS_EXFAT:
        return "exFAT";
    case BSP_SD_FS_UNKNOWN:
    default:
        return "unknown";
    }
}

static uint16_t bsp_read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t bsp_read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint64_t bsp_read_le64(const uint8_t *data)
{
    return (uint64_t)bsp_read_le32(data) | ((uint64_t)bsp_read_le32(data + 4) << 32);
}

static bool bsp_is_power_of_two_u32(uint32_t value)
{
    return value != 0 && (value & (value - 1U)) == 0;
}

static bool bsp_has_boot_sector_signature(const uint8_t *sector)
{
    return bsp_read_le16(sector + BSP_MBR_SIGNATURE_OFFSET) == 0xAA55;
}

static bsp_sd_fs_t bsp_detect_fat_vbr(const uint8_t *sector)
{
    if (!bsp_has_boot_sector_signature(sector)) {
        return BSP_SD_FS_UNKNOWN;
    }

    if (memcmp(sector + 3, "EXFAT   ", 8) == 0) {
        return BSP_SD_FS_EXFAT;
    }

    const uint8_t jump = sector[0];
    if (jump != 0xEB && jump != 0xE9 && jump != 0xE8) {
        return BSP_SD_FS_UNKNOWN;
    }

    const uint16_t bytes_per_sector = bsp_read_le16(sector + 11);
    const uint8_t sectors_per_cluster = sector[13];
    const uint16_t reserved_sectors = bsp_read_le16(sector + 14);
    const uint8_t fat_count = sector[16];
    const uint16_t root_entry_count = bsp_read_le16(sector + 17);
    const uint16_t fat16_sectors = bsp_read_le16(sector + 22);
    const uint32_t fat32_sectors = bsp_read_le32(sector + 36);

    if (!bsp_is_power_of_two_u32(bytes_per_sector) ||
        bytes_per_sector < BSP_SD_SECTOR_SIZE ||
        bytes_per_sector > 4096 ||
        !bsp_is_power_of_two_u32(sectors_per_cluster) ||
        reserved_sectors == 0 ||
        (fat_count != 1 && fat_count != 2)) {
        return BSP_SD_FS_UNKNOWN;
    }

    if (memcmp(sector + 82, "FAT32   ", 8) == 0 ||
        (root_entry_count == 0 && fat32_sectors != 0)) {
        return BSP_SD_FS_FAT32;
    }

    if (memcmp(sector + 54, "FAT", 3) == 0 ||
        (root_entry_count != 0 && fat16_sectors != 0)) {
        return BSP_SD_FS_FAT;
    }

    return BSP_SD_FS_UNKNOWN;
}

static bool bsp_partition_entry_is_empty(const uint8_t *entry)
{
    for (int i = 0; i < 16; i++) {
        if (entry[i] != 0) {
            return false;
        }
    }
    return true;
}

static uint8_t *bsp_alloc_sd_sector_buffer(void)
{
    uint8_t *buffer = heap_caps_calloc(1, BSP_SD_SECTOR_SIZE,
                                       MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "No memory for SD sector probe buffer");
    }
    return buffer;
}

static esp_err_t bsp_read_sd_sector(sdmmc_card_t *card, uint32_t sector, uint8_t *buffer)
{
    if (card == NULL || buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (card->csd.sector_size != BSP_SD_SECTOR_SIZE) {
        ESP_LOGE(TAG, "Unsupported SD sector size: %u bytes", (unsigned)card->csd.sector_size);
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t err = sdmmc_read_sectors(card, buffer, sector, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read SD sector %" PRIu32 ": %s",
                 sector, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t bsp_probe_volume_at(sdmmc_card_t *card,
                                     uint64_t start_lba,
                                     uint64_t sector_count,
                                     bsp_sd_layout_t layout,
                                     uint8_t partition_index,
                                     uint8_t partition_type,
                                     bsp_sd_volume_t *volume,
                                     bool *saw_exfat)
{
    const uint64_t card_sectors = card->csd.capacity;
    if (start_lba >= card_sectors || sector_count == 0 ||
        sector_count > UINT32_MAX || start_lba > UINT32_MAX ||
        start_lba + sector_count > card_sectors) {
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t *sector = bsp_alloc_sd_sector_buffer();
    if (sector == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = bsp_read_sd_sector(card, (uint32_t)start_lba, sector);
    if (err != ESP_OK) {
        goto cleanup;
    }

    bsp_sd_fs_t fs_type = bsp_detect_fat_vbr(sector);
    if (fs_type == BSP_SD_FS_EXFAT) {
        if (saw_exfat) {
            *saw_exfat = true;
        }
        err = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }
    if (fs_type == BSP_SD_FS_UNKNOWN) {
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    *volume = (bsp_sd_volume_t) {
        .layout = layout,
        .fs_type = fs_type,
        .start_lba = (uint32_t)start_lba,
        .sector_count = (uint32_t)sector_count,
        .partition_index = partition_index,
        .partition_type = partition_type,
    };
    err = ESP_OK;

cleanup:
    free(sector);
    return err;
}

static bool bsp_is_gpt_protective_mbr(const uint8_t *sector)
{
    if (!bsp_has_boot_sector_signature(sector)) {
        return false;
    }

    for (uint8_t i = 0; i < 4; i++) {
        const uint8_t *entry = sector + BSP_MBR_PART_TABLE_OFFSET +
                               (i * BSP_MBR_PART_ENTRY_SIZE);
        if (entry[BSP_MBR_PART_TYPE_OFFSET] == 0xEE) {
            return true;
        }
    }
    return false;
}

static esp_err_t bsp_probe_gpt_volume(sdmmc_card_t *card,
                                      bsp_sd_volume_t *volume,
                                      bool *saw_exfat)
{
    uint8_t *sector = bsp_alloc_sd_sector_buffer();
    if (sector == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = bsp_read_sd_sector(card, BSP_GPT_HEADER_LBA, sector);
    if (err != ESP_OK) {
        goto cleanup;
    }

    if (memcmp(sector, "EFI PART", 8) != 0) {
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    const uint32_t header_size = bsp_read_le32(sector + 12);
    const uint64_t entries_lba = bsp_read_le64(sector + 72);
    const uint32_t entry_count = bsp_read_le32(sector + 80);
    const uint32_t entry_size = bsp_read_le32(sector + 84);

    if (header_size < 92 || header_size > BSP_SD_SECTOR_SIZE ||
        entries_lba == 0 || entries_lba > UINT32_MAX ||
        entry_count == 0 || entry_size < BSP_GPT_ENTRY_SIZE ||
        entry_size > BSP_SD_SECTOR_SIZE) {
        ESP_LOGW(TAG, "Invalid GPT header");
        err = ESP_ERR_NOT_FOUND;
        goto cleanup;
    }

    uint64_t loaded_sector = UINT64_MAX;
    const uint32_t scan_count = (entry_count < BSP_GPT_MAX_SCAN_ENTRIES) ?
                                entry_count : BSP_GPT_MAX_SCAN_ENTRIES;

    for (uint32_t i = 0; i < scan_count; i++) {
        const uint64_t entry_byte_offset = (uint64_t)i * entry_size;
        const uint64_t sector_lba = entries_lba + entry_byte_offset / BSP_SD_SECTOR_SIZE;
        const uint32_t sector_offset = entry_byte_offset % BSP_SD_SECTOR_SIZE;

        if (sector_lba > UINT32_MAX ||
            sector_offset + entry_size > BSP_SD_SECTOR_SIZE) {
            continue;
        }

        if (loaded_sector != sector_lba) {
            err = bsp_read_sd_sector(card, (uint32_t)sector_lba, sector);
            if (err != ESP_OK) {
                goto cleanup;
            }
            loaded_sector = sector_lba;
        }

        const uint8_t *entry = sector + sector_offset;
        if (bsp_partition_entry_is_empty(entry)) {
            continue;
        }

        const uint64_t first_lba = bsp_read_le64(entry + 32);
        const uint64_t last_lba = bsp_read_le64(entry + 40);
        if (first_lba == 0 || last_lba < first_lba) {
            continue;
        }

        err = bsp_probe_volume_at(card, first_lba, last_lba - first_lba + 1,
                                  BSP_SD_LAYOUT_GPT, (uint8_t)(i + 1), 0,
                                  volume, saw_exfat);
        if (err == ESP_OK) {
            goto cleanup;
        }
        if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_NOT_SUPPORTED) {
            goto cleanup;
        }
    }

    err = ESP_ERR_NOT_FOUND;

cleanup:
    free(sector);
    return err;
}

static esp_err_t bsp_probe_mbr_volume(sdmmc_card_t *card,
                                      const uint8_t *mbr,
                                      bsp_sd_volume_t *volume,
                                      bool *saw_exfat)
{
    if (!bsp_has_boot_sector_signature(mbr)) {
        return ESP_ERR_NOT_FOUND;
    }

    for (uint8_t i = 0; i < 4; i++) {
        const uint8_t *entry = mbr + BSP_MBR_PART_TABLE_OFFSET +
                               (i * BSP_MBR_PART_ENTRY_SIZE);
        const uint8_t partition_type = entry[BSP_MBR_PART_TYPE_OFFSET];
        const uint32_t first_lba = bsp_read_le32(entry + BSP_MBR_PART_LBA_OFFSET);
        const uint32_t sector_count = bsp_read_le32(entry + BSP_MBR_PART_SIZE_OFFSET);

        if (partition_type == 0 || first_lba == 0 || sector_count == 0) {
            continue;
        }

        esp_err_t err = bsp_probe_volume_at(card, first_lba, sector_count,
                                            BSP_SD_LAYOUT_MBR, i + 1, partition_type,
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

static esp_err_t bsp_probe_sd_volume(sdmmc_card_t *card, bsp_sd_volume_t *volume)
{
    memset(volume, 0, sizeof(*volume));

    uint8_t *sector0 = bsp_alloc_sd_sector_buffer();
    if (sector0 == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = bsp_read_sd_sector(card, 0, sector0);
    if (err != ESP_OK) {
        goto cleanup;
    }

    bool saw_exfat = false;
    const bsp_sd_fs_t sfd_fs = bsp_detect_fat_vbr(sector0);
    if (sfd_fs == BSP_SD_FS_FAT || sfd_fs == BSP_SD_FS_FAT32) {
        if (card->csd.capacity > UINT32_MAX) {
            ESP_LOGE(TAG, "Raw FAT/FAT32 volume is larger than 32-bit LBA range");
            err = ESP_ERR_NOT_SUPPORTED;
            goto cleanup;
        }

        *volume = (bsp_sd_volume_t) {
            .layout = BSP_SD_LAYOUT_SUPER_FLOPPY,
            .fs_type = sfd_fs,
            .start_lba = 0,
            .sector_count = (uint32_t)card->csd.capacity,
        };
        err = ESP_OK;
        goto cleanup;
    }
    if (sfd_fs == BSP_SD_FS_EXFAT) {
        ESP_LOGE(TAG, "SD card is exFAT; this firmware supports FAT/FAT32 only");
        err = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    const bool has_gpt = bsp_is_gpt_protective_mbr(sector0);
    if (has_gpt) {
        err = bsp_probe_gpt_volume(card, volume, &saw_exfat);
        if (err == ESP_OK) {
            goto cleanup;
        }
        if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_NOT_SUPPORTED) {
            goto cleanup;
        }
    }

    err = bsp_probe_mbr_volume(card, sector0, volume, &saw_exfat);
    if (err == ESP_OK) {
        goto cleanup;
    }
    if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_NOT_SUPPORTED) {
        goto cleanup;
    }

    if (saw_exfat) {
        ESP_LOGE(TAG, "Found exFAT partition; this firmware supports FAT/FAT32 only");
        err = ESP_ERR_NOT_SUPPORTED;
        goto cleanup;
    }

    ESP_LOGE(TAG, "%s",
             has_gpt ? "GPT detected, but no FAT/FAT32 partition VBR was found" :
                       "No FAT/FAT32 volume found in sector 0 or MBR partitions");
    err = ESP_ERR_NOT_FOUND;

cleanup:
    free(sector0);
    return err;
}

static bool bsp_sd_disk_is_current(BYTE pdrv)
{
    return bsp_sd_disk.card != NULL && bsp_sd_disk.pdrv == pdrv;
}

static DSTATUS bsp_sd_disk_initialize(BYTE pdrv)
{
    return bsp_sd_disk_is_current(pdrv) ? 0 : STA_NOINIT;
}

static DSTATUS bsp_sd_disk_status(BYTE pdrv)
{
    if (!bsp_sd_disk_is_current(pdrv)) {
        return STA_NOINIT;
    }

    if (!bsp_sd_disk.disk_status_check) {
        return 0;
    }

    return (sdmmc_get_status(bsp_sd_disk.card) == ESP_OK) ? 0 : STA_NOINIT;
}

static DRESULT bsp_sd_disk_read(BYTE pdrv, BYTE *buffer, DWORD sector, UINT count)
{
    if (!bsp_sd_disk_is_current(pdrv)) {
        return RES_NOTRDY;
    }
    if (count == 0) {
        return RES_OK;
    }
    if ((uint64_t)sector + count > bsp_sd_disk.sector_count) {
        return RES_PARERR;
    }

    esp_err_t err = sdmmc_read_sectors(bsp_sd_disk.card, buffer,
                                       bsp_sd_disk.start_lba + sector, count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD read failed at partition sector %" PRIu32 ": %s",
                 (uint32_t)sector, esp_err_to_name(err));
        return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT bsp_sd_disk_write(BYTE pdrv, const BYTE *buffer, DWORD sector, UINT count)
{
    if (!bsp_sd_disk_is_current(pdrv)) {
        return RES_NOTRDY;
    }
    if (count == 0) {
        return RES_OK;
    }
    if ((uint64_t)sector + count > bsp_sd_disk.sector_count) {
        return RES_PARERR;
    }

    esp_err_t err = sdmmc_write_sectors(bsp_sd_disk.card, buffer,
                                        bsp_sd_disk.start_lba + sector, count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD write failed at partition sector %" PRIu32 ": %s",
                 (uint32_t)sector, esp_err_to_name(err));
        return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT bsp_sd_disk_ioctl(BYTE pdrv, BYTE cmd, void *buffer)
{
    if (!bsp_sd_disk_is_current(pdrv)) {
        return RES_NOTRDY;
    }

    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *((DWORD *)buffer) = bsp_sd_disk.sector_count;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *((WORD *)buffer) = bsp_sd_disk.sector_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *((DWORD *)buffer) = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

static void bsp_call_sd_host_deinit(const sdmmc_host_t *host)
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

static const esp_vfs_fat_sdmmc_mount_config_t *bsp_sdcard_default_mount_config(void)
{
    static const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
        .max_files = 10,
        .allocation_unit_size = 16 * 1024,
    };

    return &mount_config;
}

static esp_err_t bsp_init_sdmmc_card_with_width(uint8_t bus_width, sdmmc_card_t **out_card)
{
    sdmmc_host_t host = {0};
    sdmmc_slot_config_t slot = {0};
    sdmmc_card_t *card = calloc(1, sizeof(*card));
    bool host_inited = false;

    if (out_card == NULL) {
        free(card);
        return ESP_ERR_INVALID_ARG;
    }
    *out_card = NULL;
    if (card == NULL) {
        return ESP_ERR_NO_MEM;
    }

    bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &host);
    bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &slot);
    slot.width = bus_width;

    esp_err_t err = host.init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC host init failed in %u-bit mode: %s",
                 (unsigned)bus_width, esp_err_to_name(err));
        goto fail;
    }
    host_inited = true;

    err = sdmmc_host_init_slot(host.slot, &slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SDMMC slot init failed in %u-bit mode: %s",
                 (unsigned)bus_width, esp_err_to_name(err));
        goto fail;
    }

    err = sdmmc_card_init(&host, card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card init failed in %u-bit mode: %s",
                 (unsigned)bus_width, esp_err_to_name(err));
        goto fail;
    }

    *out_card = card;
    return ESP_OK;

fail:
    if (host_inited) {
        bsp_call_sd_host_deinit(&host);
    }
    free(card);
    return err;
}

static void bsp_clear_partition_mount_state(void)
{
    memset(&bsp_sd_disk, 0, sizeof(bsp_sd_disk));
    bsp_sd_disk.pdrv = FF_DRV_NOT_USED;
    bsp_sd_fs = NULL;
    bsp_sd_drive[0] = '\0';
    bsp_sd_uses_partition_diskio = false;
}

static void bsp_cleanup_partition_mount(sdmmc_card_t *card)
{
    if (bsp_sd_fs != NULL && bsp_sd_drive[0] != '\0') {
        f_mount(NULL, bsp_sd_drive, 0);
        esp_vfs_fat_unregister_path(BSP_SD_MOUNT_POINT);
    }

    if (bsp_sd_disk.pdrv != FF_DRV_NOT_USED) {
        ff_diskio_unregister(bsp_sd_disk.pdrv);
    }

    bsp_clear_partition_mount_state();

    if (card != NULL) {
        bsp_call_sd_host_deinit(&card->host);
        free(card);
    }
    if (bsp_sdcard == card) {
        bsp_sdcard = NULL;
    }
}

static esp_err_t bsp_mount_initialized_sd_card_partition(sdmmc_card_t *card,
                                                        const bsp_sd_volume_t *volume,
                                                        const esp_vfs_fat_mount_config_t *mount_config)
{
    static const ff_diskio_impl_t diskio = {
        .init = bsp_sd_disk_initialize,
        .status = bsp_sd_disk_status,
        .read = bsp_sd_disk_read,
        .write = bsp_sd_disk_write,
        .ioctl = bsp_sd_disk_ioctl,
    };

    BYTE pdrv = FF_DRV_NOT_USED;
    esp_err_t err = ff_diskio_get_drive(&pdrv);
    if (err != ESP_OK || pdrv == FF_DRV_NOT_USED) {
        ESP_LOGE(TAG, "No free FatFs physical drive");
        return ESP_ERR_NO_MEM;
    }
    if (pdrv > 9) {
        ESP_LOGE(TAG, "FatFs drive number %u is unsupported by this mount path",
                 (unsigned)pdrv);
        return ESP_ERR_NOT_SUPPORTED;
    }

    bsp_sd_disk = (bsp_sd_disk_t) {
        .card = card,
        .pdrv = pdrv,
        .start_lba = volume->start_lba,
        .sector_count = volume->sector_count,
        .sector_size = BSP_SD_SECTOR_SIZE,
        .disk_status_check = mount_config->disk_status_check_enable,
    };
    ff_diskio_register(pdrv, &diskio);

    bsp_sd_drive[0] = (char)('0' + pdrv);
    bsp_sd_drive[1] = ':';
    bsp_sd_drive[2] = '\0';
    const esp_vfs_fat_conf_t conf = {
        .base_path = BSP_SD_MOUNT_POINT,
        .fat_drive = bsp_sd_drive,
        .max_files = mount_config->max_files,
    };

    err = esp_vfs_fat_register(&conf, &bsp_sd_fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_vfs_fat_register failed: %s", esp_err_to_name(err));
        bsp_cleanup_partition_mount(NULL);
        return err;
    }

    FRESULT fres = f_mount(bsp_sd_fs, bsp_sd_drive, 1);
    if (fres != FR_OK) {
        ESP_LOGE(TAG, "FatFs partition-aware mount failed with FRESULT=%d", (int)fres);
        bsp_cleanup_partition_mount(NULL);
        return ESP_FAIL;
    }

    bsp_sdcard = card;
    bsp_sd_uses_partition_diskio = true;
    ESP_LOGI(TAG,
             "SD card mounted at %s via BSP partition-aware mount (%s %s, start_lba=%" PRIu32 ", sectors=%" PRIu32 ")",
             BSP_SD_MOUNT_POINT,
             bsp_sd_layout_name(volume->layout),
             bsp_sd_fs_name(volume->fs_type),
             volume->start_lba,
             volume->sector_count);
    return ESP_OK;
}

static esp_err_t bsp_sdcard_mount_partition_aware(uint8_t bus_width,
                                                  const esp_vfs_fat_mount_config_t *mount_config)
{
    if (mount_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    sdmmc_card_t *card = NULL;
    esp_err_t err = bsp_init_sdmmc_card_with_width(bus_width, &card);
    if (err != ESP_OK) {
        return err;
    }

    bsp_sd_volume_t volume = {0};
    err = bsp_probe_sd_volume(card, &volume);
    if (err == ESP_OK) {
        err = bsp_mount_initialized_sd_card_partition(card, &volume, mount_config);
    }

    if (err != ESP_OK) {
        if (mount_config->format_if_mount_failed) {
            ESP_LOGW(TAG,
                     "SD auto-format is not supported by BSP partition-aware mount");
        }
        bsp_cleanup_partition_mount(card);
    }
    return err;
}

esp_err_t bsp_sdcard_sdmmc_mount(bsp_sdcard_cfg_t *cfg)
{
    sdmmc_host_t sdhost = {0};
    sdmmc_slot_config_t sdslot = {0};
    assert(cfg);

    if (!cfg->mount) {
        cfg->mount = bsp_sdcard_default_mount_config();
    }

    if (!cfg->host) {
        bsp_sdcard_get_sdmmc_host(SDMMC_HOST_SLOT_0, &sdhost);
        cfg->host = &sdhost;
    }

    if (!cfg->slot.sdmmc) {
        bsp_sdcard_sdmmc_get_slot(SDMMC_HOST_SLOT_0, &sdslot);
        cfg->slot.sdmmc = &sdslot;
    }

#if defined(CONFIG_FATFS_LFN_NONE)
    ESP_LOGW(TAG, "Warning: Long filenames on SD card are disabled in menuconfig!");
#endif

    return esp_vfs_fat_sdmmc_mount(BSP_SD_MOUNT_POINT, cfg->host, cfg->slot.sdmmc, cfg->mount, &bsp_sdcard);
}

esp_err_t bsp_sdcard_sdspi_mount(bsp_sdcard_cfg_t *cfg)
{
    ESP_LOGE(TAG, "SD card SPI mode is not supported by HW!");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_sdcard_mount(void)
{
    return bsp_sdcard_mount_with_width(4, bsp_sdcard_default_mount_config());
}

esp_err_t bsp_sdcard_mount_with_width(uint8_t bus_width,
                                      const esp_vfs_fat_sdmmc_mount_config_t *mount_config)
{
    if (bus_width != 1 && bus_width != 4) {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_vfs_fat_sdmmc_mount_config_t *active_mount_config =
        mount_config ? mount_config : bsp_sdcard_default_mount_config();

    esp_err_t err = bsp_sdcard_mount_partition_aware(bus_width, active_mount_config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SDMMC %u-bit partition-aware mount failed: %s",
                 (unsigned)bus_width, esp_err_to_name(err));
    }
    return err;
}

esp_err_t bsp_sdcard_unmount(void)
{
    if (bsp_sd_uses_partition_diskio) {
        bsp_cleanup_partition_mount(bsp_sdcard);
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;

    ret |= esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, bsp_sdcard);
    bsp_sdcard = NULL;

    return ret;
}

esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config)
{
    esp_err_t ret = ESP_FAIL;
    if (i2s_tx_chan && i2s_rx_chan) {
        /* Audio was initialized before */
        return ESP_OK;
    }

    /* Setup I2S peripheral */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
    BSP_ERROR_CHECK_RETURN_ERR(i2s_new_channel(&chan_cfg, &i2s_tx_chan, &i2s_rx_chan));

    /* Setup I2S channels */
    const i2s_std_config_t std_cfg_default = BSP_I2S_DUPLEX_MONO_CFG(22050);
    const i2s_std_config_t *p_i2s_cfg = &std_cfg_default;
    if (i2s_config != NULL) {
        p_i2s_cfg = i2s_config;
    }

    if (i2s_tx_chan != NULL) {
        ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_tx_chan, p_i2s_cfg), err, TAG, "I2S channel initialization failed");
        ESP_GOTO_ON_ERROR(i2s_channel_enable(i2s_tx_chan), err, TAG, "I2S enabling failed");
    }
    if (i2s_rx_chan != NULL) {
        ESP_GOTO_ON_ERROR(i2s_channel_init_std_mode(i2s_rx_chan, p_i2s_cfg), err, TAG, "I2S channel initialization failed");
        ESP_GOTO_ON_ERROR(i2s_channel_enable(i2s_rx_chan), err, TAG, "I2S enabling failed");
    }

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .rx_handle = i2s_rx_chan,
        .tx_handle = i2s_tx_chan,
    };
    i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    BSP_NULL_CHECK_GOTO(i2s_data_if, err);

    return ESP_OK;

err:
    if (i2s_tx_chan) {
        i2s_del_channel(i2s_tx_chan);
        i2s_tx_chan = NULL;
    }
    if (i2s_rx_chan) {
        i2s_del_channel(i2s_rx_chan);
        i2s_rx_chan = NULL;
    }
    i2s_data_if = NULL;

    return ret;
}

const audio_codec_data_if_t *bsp_audio_get_codec_itf(void)
{
    return i2s_data_if;
}

esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void)
{
    const audio_codec_data_if_t *i2s_data_if = bsp_audio_get_codec_itf();
    if (i2s_data_if == NULL) {
        /* Initilize I2C */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());
        /* Configure I2S peripheral and Power Amplifier */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_init(NULL));
        i2s_data_if = bsp_audio_get_codec_itf();
    }
    assert(i2s_data_if);

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    BSP_NULL_CHECK(i2c_ctrl_if, NULL);

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
    };
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    BSP_NULL_CHECK(es8311_dev, NULL);

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_dev_cfg);
}

esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void)
{
    const audio_codec_data_if_t *i2s_data_if = bsp_audio_get_codec_itf();
    if (i2s_data_if == NULL) {
        /* Initilize I2C */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_i2c_init());
        /* Configure I2S peripheral and Power Amplifier */
        BSP_ERROR_CHECK_RETURN_NULL(bsp_audio_init(NULL));
        i2s_data_if = bsp_audio_get_codec_itf();
    }
    assert(i2s_data_if);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = BSP_ES7210_CODEC_ADDR,
        .bus_handle = i2c_handle,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    BSP_NULL_CHECK(i2c_ctrl_if, NULL);

    es7210_codec_cfg_t es7210_cfg = {
        .ctrl_if = i2c_ctrl_if,
    };
    const audio_codec_if_t *es7210_dev = es7210_codec_new(&es7210_cfg);
    BSP_NULL_CHECK(es7210_dev, NULL);

    esp_codec_dev_cfg_t codec_es7210_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210_dev,
        .data_if = i2s_data_if,
    };
    return esp_codec_dev_new(&codec_es7210_dev_cfg);
}

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel, esp_lcd_panel_io_handle_t *ret_io)
{
    (void)config;
    if (ret_panel == NULL || ret_io == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_panel = NULL;
    *ret_io = NULL;

    // reset lcd
    gpio_config_t ioconf = {
        .pin_bit_mask = 1ULL << BSP_LCD_RST,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&ioconf), TAG, "LCD reset GPIO config failed");

    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_RST, 0), TAG, "LCD reset low failed");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(gpio_set_level(BSP_LCD_RST, 1), TAG, "LCD reset high failed");
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_err_t ret = ESP_OK;

    ESP_LOGD(TAG, "Initialize QSPI bus");
    const spi_bus_config_t buscfg = {            
    .data0_io_num = BSP_LCD_DATA0,                    
    .data1_io_num = BSP_LCD_DATA1,                   
    .sclk_io_num = BSP_LCD_PCLK,                   
    .data2_io_num = BSP_LCD_DATA2,                    
    .data3_io_num = BSP_LCD_DATA3,                    
    .data4_io_num = -1,                       
    .data5_io_num = -1,                      
    .data6_io_num = -1,                       
    .data7_io_num = -1,                      
    .max_transfer_sz = 0, 
    .flags = SPICOMMON_BUSFLAG_QUAD,       
    .intr_flags = 0,                            
  };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI init failed");

    ESP_LOGD(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config ={
    .cs_gpio_num = BSP_LCD_CS,               
    .dc_gpio_num = -1,                  
    .spi_mode = 0,                     
    .pclk_hz = 3 * 1000 * 1000,   
    .trans_queue_depth = CONFIG_BSP_LCD_TRANS_QUEUE_DEPTH,
    .on_color_trans_done = NULL,                            
    .user_ctx = NULL,                   
    .lcd_cmd_bits = 32,                 
    .lcd_param_bits = 8,                
    .flags = {                          
      .dc_low_on_data = 0,            
      .octal_mode = 0,                
      .quad_mode = 1,                 
      .sio_mode = 0,                  
      .lsb_first = 0,                 
      .cs_high_active = 0,            
    },                                  
  };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, ret_io), err, TAG, "New panel IO failed");


    printf("Install LCD driver of st77916\r\n");
    st77916_vendor_config_t vendor_config={  
        .init_cmds = vendor_specific_init_version_1,
        .init_cmds_size = sizeof(vendor_specific_init_version_1) / sizeof(st77916_lcd_init_cmd_t),
        .flags = {
        .use_qspi_interface = 1,
        },
    };


    /*-----------------  Read the screen version and select initialization parameters based on the version  -----------------*/
    //read id
    int lcd_cmd = 0x04;
    uint8_t register_data[4] = {0};
    size_t param_size = sizeof(register_data);
    lcd_cmd &= 0xff;
    lcd_cmd <<= 8;
    lcd_cmd |= LCD_OPCODE_READ_CMD << 24;  // Use the read opcode instead of write
    esp_err_t probe_ret = esp_lcd_panel_io_rx_param(*ret_io, lcd_cmd, register_data, param_size);
    if (probe_ret == ESP_OK) {
        printf("Register 0x04 data: %02x %02x %02x %02x\n", register_data[0], register_data[1], register_data[2], register_data[3]);
    } else {
        printf("Failed to read register 0x04, error code: %d\n", probe_ret);
    } 

    ESP_GOTO_ON_ERROR(esp_lcd_panel_io_del(*ret_io), err, TAG, "Delete probe panel IO failed");
    *ret_io = NULL;

    // reconfig spi bus
    io_config.pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ;
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, ret_io), err, TAG, "New panel IO failed");
    
    // Check register values and configure accordingly
    if (register_data[0] == 0x00 && register_data[1] == 0x7F && register_data[2] == 0x7F && register_data[3] == 0x7F) {
        vendor_config.init_cmds = vendor_specific_init_version_1;
        vendor_config.init_cmds_size = sizeof(vendor_specific_init_version_1) / sizeof(st77916_lcd_init_cmd_t);
        printf("Vendor-specific initialization for case 1.\n");
    }
    else if (register_data[0] == 0x00 && register_data[1] == 0x02 && register_data[2] == 0x7F && register_data[3] == 0x7F) {
        vendor_config.init_cmds = vendor_specific_init_version_2;
        vendor_config.init_cmds_size = sizeof(vendor_specific_init_version_2) / sizeof(st77916_lcd_init_cmd_t);
        printf("Vendor-specific initialization for case 2.\n");
    }
    /*-----------------------------------------------------------------------------------------------------------------------*/

    esp_lcd_panel_dev_config_t panel_config={
        .reset_gpio_num = BSP_LCD_RST,                                
        .rgb_ele_order = BSP_LCD_COLOR_SPACE,                                        
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,                                 
        .flags = {                                                    
        .reset_active_high = 0,                                   
        },                                                            
        .vendor_config = (void *) &vendor_config,                                  
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st77916(*ret_io, &panel_config, ret_panel), err, TAG, "New ST77916 panel failed");

    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(*ret_panel), err, TAG, "LCD panel reset failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(*ret_panel), err, TAG, "LCD panel init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(*ret_panel, true), err, TAG, "LCD panel display on failed");

    //esp_lcd_panel_swap_xy(*ret_panel, true);
    //esp_lcd_panel_mirror(*ret_panel,true,false);
    return ret;

err:
    if (*ret_panel) {
        esp_lcd_panel_del(*ret_panel);
        *ret_panel = NULL;
    }
    if (*ret_io) {
        esp_lcd_panel_io_del(*ret_io);
        *ret_io = NULL;
    }
    spi_bus_free(BSP_LCD_SPI_NUM);
    return ret;
}

esp_err_t bsp_touch_new(const bsp_display_cfg_t *cfg, esp_lcd_touch_handle_t *ret_touch)
{
    if (cfg == NULL || ret_touch == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *ret_touch = NULL;
    /* Initilize I2C */
    BSP_ERROR_CHECK_RETURN_ERR(bsp_i2c_init());


    i2c_master_bus_handle_t i2c_handle = NULL;
    ESP_RETURN_ON_ERROR(i2c_master_get_bus_handle(BSP_I2C_NUM,  &i2c_handle), TAG, "Get I2C bus handle failed");

    /* Initialize touch HW */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = BSP_LCD_TOUCH_RST,
        .int_gpio_num = BSP_LCD_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = cfg->touch_flags.swap_xy,
            .mirror_x = cfg->touch_flags.mirror_x,
            .mirror_y = cfg->touch_flags.mirror_y,
        },
    };
    
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    tp_io_config.scl_speed_hz = CONFIG_BSP_I2C_CLK_SPEED_HZ;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c((i2c_master_bus_handle_t)i2c_handle, &tp_io_config, &tp_io_handle),
                        TAG,
                        "New touch I2C panel IO failed");
    esp_err_t ret = esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, ret_touch);
    if (ret != ESP_OK && tp_io_handle != NULL) {
        esp_lcd_panel_io_del(tp_io_handle);
    }
    return ret;
}


#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)
{
    assert(cfg != NULL);

    bsp_display_config_t disp_config = {0};
    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_new(&disp_config, &panel_handle, &io_handle));

    ESP_LOGD(TAG, "Add LCD screen");
    #if CONFIG_BSP_LCD_TE_RESISTANT
    esp_lv_adapter_display_config_t disp_cfg =
            ESP_LV_ADAPTER_DISPLAY_SPI_WITH_PSRAM_TE_DEFAULT_CONFIG(
                panel_handle,
                io_handle,
                BSP_LCD_H_RES, BSP_LCD_V_RES,
                ESP_LV_ADAPTER_ROTATE_0,
                BSP_LCD_TE,
                BSP_LCD_PIXEL_CLOCK_HZ,
                4,
                BSP_LCD_BITS_PER_PIXEL
            );
    #else
    esp_lv_adapter_display_config_t disp_cfg = {
        .panel = panel_handle,
        .panel_io = io_handle,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = cfg->rotation,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = 50,
            /* Keep LVGL draw buffers in internal SRAM: SPI DMA reading from PSRAM can
             * underflow (snow on screen) when PSRAM bandwidth is consumed by XIP + app. */
            .use_psram = false,
            .enable_ppa_accel = false,
            .require_double_buffer = true,
        },
        .tear_avoid_mode = cfg->tear_avoid_mode,
    };
    #endif

    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    if (!disp)
    {
        return NULL;
    }

    return disp;
}

static lv_indev_t *bsp_display_indev_init(const bsp_display_cfg_t *cfg, lv_display_t *disp)
{
    assert(cfg != NULL);
    BSP_ERROR_CHECK_RETURN_NULL(bsp_touch_new(cfg, &tp));
    assert(tp);

    const esp_lv_adapter_touch_config_t touch_cfg = ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, tp);

    return esp_lv_adapter_register_touch(&touch_cfg);
}
#endif // (BSP_CONFIG_NO_GRAPHIC_LIB == 0)


esp_err_t bsp_display_brightness_init(void)
{
    const ledc_channel_config_t LCD_backlight_channel = {
        .gpio_num = BSP_LCD_BACKLIGHT,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = 1,
        .duty = 0,
        .hpoint = 0
    };
    const ledc_timer_config_t LCD_backlight_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = 1,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK
    };

    BSP_ERROR_CHECK_RETURN_ERR(ledc_timer_config(&LCD_backlight_timer));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_channel_config(&LCD_backlight_channel));

    return ESP_OK;
}

esp_err_t bsp_display_brightness_set(int brightness_percent)
{
    if (brightness_percent > 100) {
        brightness_percent = 100;
    } else if (brightness_percent < 0) {
        brightness_percent = 0;
    }

    int flipped_brightness = brightness_percent;

    brightness = (uint8_t)flipped_brightness;

    ESP_LOGI(TAG, "Setting flipped LCD backlight: %d%% (original: %d%%)", flipped_brightness, brightness_percent);

    uint32_t duty_cycle = (1023 * flipped_brightness) / 100;
    BSP_ERROR_CHECK_RETURN_ERR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH, duty_cycle));
    BSP_ERROR_CHECK_RETURN_ERR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_LEDC_CH));
    return ESP_OK;
}

int bsp_display_brightness_get(void)
{
#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)
    if (disp_drv == NULL)
    {
        ESP_LOGW(TAG, "Display is not initialized");
    }
#endif

    return brightness;
}

esp_err_t bsp_display_backlight_off(void)
{
    ESP_LOGI(TAG, "Backlight off");
    return bsp_display_brightness_set(0);
}

esp_err_t bsp_display_backlight_on(void)
{
    ESP_LOGI(TAG, "Backlight on");
    return bsp_display_brightness_set(100);
}

#if (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

lv_display_t *bsp_display_start(void)
{
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0}};
    return bsp_display_start_with_config(&cfg);
}

lv_display_t *bsp_display_start_with_config(bsp_display_cfg_t *cfg)
{
    lv_display_t *disp;

    assert(cfg != NULL);
    BSP_ERROR_CHECK_RETURN_NULL(esp_lv_adapter_init(&cfg->lv_adapter_cfg));

    BSP_NULL_CHECK(disp = bsp_display_lcd_init(cfg), NULL);

    BSP_NULL_CHECK(disp_indev = bsp_display_indev_init(cfg, disp), NULL);

    BSP_ERROR_CHECK_RETURN_NULL(bsp_display_brightness_init());

    ESP_ERROR_CHECK(esp_lv_adapter_start());

    disp_drv = disp;
    return disp;
}


lv_display_t *bsp_display_get_disp_dev(void)
{
    return disp_drv;
}

lv_indev_t *bsp_display_get_input_dev(void)
{
    return disp_indev;
}

esp_err_t bsp_display_lock(int32_t timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    esp_lv_adapter_unlock();
}

#endif // (BSP_CONFIG_NO_GRAPHIC_LIB == 0)

esp_err_t bsp_spiffs_mount(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = CONFIG_BSP_SPIFFS_MOUNT_POINT,
        .partition_label = CONFIG_BSP_SPIFFS_PARTITION_LABEL,
        .max_files = CONFIG_BSP_SPIFFS_MAX_FILES,
#ifdef CONFIG_BSP_SPIFFS_FORMAT_ON_MOUNT_FAIL
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif
    };

    esp_err_t ret_val = esp_vfs_spiffs_register(&conf);

    BSP_ERROR_CHECK_RETURN_ERR(ret_val);

    size_t total = 0, used = 0;
    ret_val = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret_val != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret_val));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    return ret_val;
}

esp_err_t bsp_spiffs_unmount(void)
{
    return esp_vfs_spiffs_unregister(CONFIG_BSP_SPIFFS_PARTITION_LABEL);
}

/**
 * @brief A universal function to retrieve a list of files with specific extensions in a designated directory
 * 
 * @param dir_path path (example "/sdcard")
 * @param extension target extension (example ".jpg", ".avi", ".png")
 * @param out 
 * @return esp_err_t 
 */
esp_err_t get_file_list_by_ext(const char *dir_path, const char *extension, generic_file_list_t *out)
{
    if (!dir_path || !extension || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    out->list  = NULL;
    out->count = 0;

    DIR *dir = opendir(dir_path);
    if (!dir) {
        return ESP_FAIL;
    }

    struct dirent *entry;
    int count = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) { 
            char *ext = strrchr(entry->d_name, '.');
            if (ext && strcasecmp(ext, extension) == 0) {
                count++;
            }
        }
    }

    if (count == 0) {
        closedir(dir);
        return ESP_ERR_NOT_FOUND;
    }

    char **list = (char **)malloc(sizeof(char *) * count);
    if (!list) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    rewinddir(dir);
    int idx = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            char *ext = strrchr(entry->d_name, '.');
            if (ext && strcasecmp(ext, extension) == 0) {

                size_t len = strlen(dir_path) + strlen(entry->d_name) + 2;
                char *full_path = (char *)malloc(len);
                if (!full_path) {
 
                    for (int i = 0; i < idx; i++) free(list[i]);
                    free(list);
                    closedir(dir);
                    return ESP_ERR_NO_MEM;
                }
                snprintf(full_path, len, "%s/%s", dir_path, entry->d_name);
                list[idx++] = full_path;
            }
        }
    }

    closedir(dir);
    out->list  = list;
    out->count = idx;
    return ESP_OK;
}


qmi8658_dev_t *bsp_qmi8658_drv_init(void)
{
    bsp_i2c_init();

    qmi8658_dev_t *_qmi8658_dev = calloc(1, sizeof(qmi8658_dev_t));
    ESP_ERROR_CHECK(qmi8658_init(_qmi8658_dev, i2c_handle, QMI8658_ADDRESS_HIGH));

    qmi8658_set_accel_range(_qmi8658_dev, QMI8658_ACCEL_RANGE_8G);
    qmi8658_set_accel_odr(_qmi8658_dev, QMI8658_ACCEL_ODR_500HZ);
    qmi8658_set_accel_unit_mps2(_qmi8658_dev, true);
    qmi8658_write_register(_qmi8658_dev, QMI8658_CTRL5, 0x03);

    return _qmi8658_dev;
}

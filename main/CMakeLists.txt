idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES waveshare__esp32_s3_touch_lcd_1_85B
                             espressif__esp_mmap_assets
                             espressif__esp_lv_fs
                             dbg_console)

# 将 assets/gif 目录打包为 assets 分区镜像, 随 idf.py flash 一同烧录
spiffs_create_partition_assets(
    assets
    "${CMAKE_CURRENT_SOURCE_DIR}/../assets/gif"
    FLASH_IN_PROJECT
    MMAP_FILE_SUPPORT_FORMAT ".gif")

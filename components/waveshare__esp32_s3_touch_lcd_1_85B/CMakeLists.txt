idf_component_register(
    SRCS "esp32_s3_touch_lcd_1_85B.c" ${SRC_VER}
    INCLUDE_DIRS "include" "include/bsp"
    PRIV_INCLUDE_DIRS "priv_include"
    REQUIRES
        fatfs
        esp_codec_dev
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_driver_ledc
        esp_driver_sdmmc
        esp_driver_sdspi
        esp_driver_spi
        esp_lcd
        esp_lvgl_adapter
        i2c_bus
        pcf85063a
        qmi8658
        sdmmc
    PRIV_REQUIRES esp_timer spiffs esp_psram
)

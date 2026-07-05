idf_component_register(
    SRCS
        "main.c"
        "app_console.c"
        "app_ui.c"
        "gif_player.c"
        "gif_playlist.c"
        "sd_media.c"
    INCLUDE_DIRS "."
    REQUIRES
        waveshare__esp32_s3_touch_lcd_1_85B
        dbg_console
        heap
)

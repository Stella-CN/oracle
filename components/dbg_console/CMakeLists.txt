idf_component_register(
    SRCS "dbg_console.c"
    INCLUDE_DIRS "include"
    REQUIRES console
    PRIV_REQUIRES esp_system esp_app_format heap
)

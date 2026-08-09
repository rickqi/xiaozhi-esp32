idf_component_register(
    SRCS
        "video_player_app.cc"
        "avi_parser.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lvgl_port
        esp_lcd
        brookesia_core
        esp_driver_spi
        log
    PRIV_REQUIRES
        main
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
)

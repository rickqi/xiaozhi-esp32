idf_component_register(
    SRCS
        "fluidbox_app.cc"
        "app_icon_custom_112_112.c"
        "fb_display.c"
        "sim.c"
        "render.c"
        "imu.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lvgl_port
        esp_lcd
        esp_driver_i2c
        esp_driver_spi
        brookesia_core
        waveshare__qmi8658
        log
    PRIV_REQUIRES
        main
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)

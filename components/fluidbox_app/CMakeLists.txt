idf_component_register(
    SRCS
        "fluidbox_app.cc"
        "sim.c"
        "render.c"
        "imu.c"
        "fb_display.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lvgl_port
        esp_lcd
        esp_driver_i2c
        esp_driver_spi
        brookesia_core
        waveshare__qmi8658
        log
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
)

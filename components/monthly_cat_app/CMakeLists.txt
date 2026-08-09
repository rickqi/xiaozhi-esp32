idf_component_register(
    SRCS "monthly_cat_app.cc"
         "app_icon_salarycat_112_112.c"
    INCLUDE_DIRS "."
    REQUIRES
        lvgl
        brookesia_core
    PRIV_REQUIRES
        main
)

idf_component_register(SRCS "braindlab.c" "ui_dashboard.c" "ui_energia.c" "air_conditioner.c"
                       REQUIRES esp_psram spi_flash esp_driver_gpio esp_driver_i2c esp_lcd
                       INCLUDE_DIRS ".")

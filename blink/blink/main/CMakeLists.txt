idf_component_register(SRCS "blink.c" "ui_labgeo.c" "ui_img_prensa.c" "ui_grafica.c" "uart_labgeo.c"
                       REQUIRES esp_psram spi_flash esp_driver_gpio esp_driver_i2c esp_lcd esp_driver_uart
                       INCLUDE_DIRS ".")

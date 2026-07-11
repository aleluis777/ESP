idf_component_register(SRCS "app_main.c" "hx711.c" "dial_caliper.c" "config_labgeo.c"
                             "almacenamiento.c" "programador_corrida.c" "uart_link.c"
                             "red_eth.c" "servidor_web.c"
                       REQUIRES esp_driver_gpio esp_driver_uart nvs_flash spiffs esp_timer
                                esp_eth esp_netif esp_event esp_http_server
                       INCLUDE_DIRS ".")

# Empaqueta la carpeta ../web (index.html/style.css/script.js) como imagen
# SPIFFS para la particion "www" (ver partitions.csv), y la incluye en
# "idf.py flash" -- no hace falta un paso de subida aparte, el sitio queda
# grabado en la flash junto con el firmware cada vez que se flashea.
spiffs_create_partition_image(www ../web FLASH_IN_PROJECT)

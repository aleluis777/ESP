# PRUEBA TEMPORAL: "app_main2.c" en vez de "app_main.c" -- prueba minima de
# solo Dial 1 por consola (ver app_main2.c). Para volver al firmware real,
# cambiar "app_main2.c" de nuevo por "app_main.c" aca abajo.
idf_component_register(SRCS "app_main2.c" "dial_caliper.c" "config_labgeo.c"
                             "almacenamiento.c" "programador_corrida.c" "uart_link.c"
                             "red_eth.c" "servidor_web.c" "ping_monitor.c"
                       REQUIRES esp_driver_gpio esp_driver_uart spiffs esp_timer
                                esp_eth esp_netif esp_event esp_http_server lwip
                                app_update
                       INCLUDE_DIRS ".")

# Empaqueta la carpeta ../web (index.html/style.css/script.js) como imagen
# SPIFFS para la particion "www" (ver partitions.csv), y la incluye en
# "idf.py flash" -- no hace falta un paso de subida aparte, el sitio queda
# grabado en la flash junto con el firmware cada vez que se flashea.
spiffs_create_partition_image(www ../web FLASH_IN_PROJECT)

# Etapa actual: config + salidas (reles) + maquina de estados de
# climatizacion, sin sensores reales todavia (ver sensores_temp.c: modulo
# simulado a proposito, ver comentario ahi) y sin red/servidor web/UART a la
# pantalla/RS-485 -- eso se suma en los proximos pasos, ver PENDIENTES en el
# README de este proyecto.
idf_component_register(SRCS "app_main.c" "config_braindlab.c" "salidas_braindlab.c"
                             "climatizacion.c" "sensores_temp.c"
                             "red_eth.c" "servidor_web.c"
                             "rtc_braindlab.c" "sensor_gestor.c"
                       REQUIRES esp_driver_gpio spiffs esp_timer
                                esp_eth esp_netif esp_event esp_http_server lwip
                                app_update driver
                       INCLUDE_DIRS ".")

# Empaqueta la carpeta ../web (index.html/style.css/script.js) como imagen
# SPIFFS para la particion "www" (ver partitions.csv), y la incluye en
# "idf.py flash" -- mismo patron que controlador_labgeo.
spiffs_create_partition_image(www ../web FLASH_IN_PROJECT)

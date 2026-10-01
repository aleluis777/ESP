# Etapa actual: config + salidas (reles) + maquina de estados de
# climatizacion + sensores reales (ADS1115 en sensores_temp.c, DS1307,
# AM2301A) + red/servidor web + UART hacia la pantalla braindlab. Falta
# RS-485 -- ver PENDIENTES en el README de este proyecto.
idf_component_register(SRCS "app_main.c" "config_braindlab.c" "salidas_braindlab.c"
                             "climatizacion.c" "sensores_temp.c"
                             "red_eth.c" "servidor_web.c"
                             "rtc_braindlab.c" "sensor_gestor.c" "uart_pantalla.c"
                             "snmp_braindlab.c"
                       REQUIRES esp_driver_gpio esp_driver_uart spiffs esp_timer
                                esp_eth esp_netif esp_event esp_http_server lwip
                                app_update driver snmp_lwip
                       INCLUDE_DIRS ".")

# Empaqueta la carpeta ../web (index.html/style.css/script.js) como imagen
# SPIFFS para la particion "www" (ver partitions.csv), y la incluye en
# "idf.py flash" -- mismo patron que controlador_labgeo.
spiffs_create_partition_image(www ../web FLASH_IN_PROJECT)

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Servidor HTTP + WebSocket (esp_http_server). El WebSocket en /ws es el
// socket en tiempo real pedido: cualquier cliente conectado ahi recibe un
// frame JSON cada vez que se llama a servidor_web_enviar_ws() (pensado para
// llamarse a la misma cadencia que uart_link_enviar_sensor_update(), hasta
// 5 veces por segundo).
//
// El sitio estatico (html/css/js) y el resto de los endpoints REST del
// LinaresETH viejo (OTA, config, historial) no estan portados todavia -- ver
// resumen de la sesion. Esto solo trae el servidor + el canal en vivo.

esp_err_t servidor_web_init(void);

// Manda 'json' (un string ya armado, terminado en '\0') a todos los clientes
// WebSocket conectados en /ws. No bloquea si no hay clientes.
void servidor_web_enviar_ws(const char *json);

#ifdef __cplusplus
}
#endif

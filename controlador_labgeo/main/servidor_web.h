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

// Callbacks para los endpoints de calibracion (POST /calibrar_cero y
// POST /calibrar_maximo) -- este archivo no sabe nada de HX711 ni de como
// se calculan pendiente/offset, solo avisa que llego el pedido. Mismo
// patron que uart_link_callbacks_t. Registrar con servidor_web_set_callbacks()
// ANTES de llamar a servidor_web_init().
typedef void (*servidor_web_cb_calibrar_cero_t)(void);
typedef void (*servidor_web_cb_calibrar_maximo_t)(float peso_n);

// Llego POST /configurar_red con {"ip":"...","gateway":"...","mascara":"..."}.
// Los tres strings son punteros validos solo durante la llamada (no
// guardarlos, copiarlos si hace falta) -- este archivo no sabe nada de
// config_labgeo.h, solo pasa lo que vino en el body tal cual.
typedef void (*servidor_web_cb_configurar_red_t)(const char *ip, const char *gateway, const char *mascara);

typedef struct {
    servidor_web_cb_calibrar_cero_t on_calibrar_cero;      // llego POST /calibrar_cero
    servidor_web_cb_calibrar_maximo_t on_calibrar_maximo;  // llego POST /calibrar_maximo con {"peso_n": N}
    servidor_web_cb_configurar_red_t on_configurar_red;    // llego POST /configurar_red
} servidor_web_callbacks_t;

void servidor_web_set_callbacks(servidor_web_callbacks_t callbacks);

#ifdef __cplusplus
}
#endif

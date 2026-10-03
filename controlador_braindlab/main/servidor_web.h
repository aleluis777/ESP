#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Servidor HTTP + WebSocket (esp_http_server), mismo patron que
// controlador_labgeo: sirve el sitio estatico (index.html/style.css/
// script.js, ver ../web) desde la particion SPIFFS "www", y tiene el
// WebSocket en /ws que empuja el estado de climatizacion en vivo.
//
// Llamar despues de red_eth_init(). OTA/subida de archivos (los que si
// tiene controlador_labgeo) no estan portados todavia -- esto trae el
// sitio + el canal en vivo + los endpoints de configuracion.
esp_err_t servidor_web_init(void);

// Manda 'json' (un string ya armado, terminado en '\0') a todos los
// clientes WebSocket conectados en /ws. No bloquea si no hay clientes ni si
// el servidor no llego a arrancar.
void servidor_web_enviar_ws(const char *json);

// Llego POST /configurar_red con {"ip":"...","gateway":"...","mascara":"..."}.
// Los tres strings son punteros validos solo durante la llamada (no
// guardarlos, copiarlos si hace falta) -- este archivo no sabe nada de
// config_braindlab.h, solo pasa lo que vino en el body tal cual. Mismo
// patron que servidor_web_cb_configurar_red_t en controlador_labgeo.
typedef void (*servidor_web_cb_configurar_red_t)(const char *ip, const char *gateway, const char *mascara);

// Llego POST /configurar_climatizacion con
// {"cantidad_aires":N,"temp_min":N,"temp_max":N,"temp_at":N,"temp_bypass":N,
//  "fails_max_bypass":N,"rotar_reserva":bool} -- equivalente de
// on_configurar_equipo en controlador_labgeo, pero para los parametros de
// climatizacion (ver config_climatizacion_t en config_braindlab.h) en vez
// de los del equipo de ensayo de labgeo.
typedef void (*servidor_web_cb_configurar_climatizacion_t)(uint8_t cantidad_aires, float temp_min, float temp_max,
                                                            float temp_at, float temp_bypass,
                                                            uint8_t fails_max_bypass, bool rotar_reserva);

// Llego POST /control_aire con {"indice":0-3,"encendido":bool} -- pide
// forzar a mano la salida AA(indice+1). app_main.c decide como convive esto
// con la maquina de estados automatica (ver climatizacion.h): el override
// persiste hasta que climatizacion_actualizar() cruce a un ciclo distinto
// del que tenia cuando se pidio el manual, ahi la automatica retoma el
// control solo. Este archivo no sabe nada de esa logica, solo avisa el pedido.
typedef void (*servidor_web_cb_control_aire_t)(uint8_t indice, bool encendido);

// Llego POST /control_bypass con {"solicitado":bool} -- pide forzar a mano
// BP_S (bypass_solicitado). NO controla bypass_activo (BPS_STATUS): esa es
// una lectura de un pin de entrada (realimentacion fisica real del nodo de
// bypass), no se puede escribir por software.
typedef void (*servidor_web_cb_control_bypass_t)(bool solicitado);

// Llego POST /control_at con {"activa":bool} -- pide forzar a mano OUT_AT
// (la alarma de alta temperatura). Mismo criterio de override que
// /control_aire: persiste hasta que la automatica cruce a otro ciclo.
typedef void (*servidor_web_cb_control_at_t)(bool activa);

// Llego POST /control_automatico (sin body) -- cancela cualquier override
// manual de AA1-4, AT y del bypass, la maquina automatica retoma el control
// de todo en el proximo ciclo.
typedef void (*servidor_web_cb_control_automatico_t)(void);

// Llego POST /configurar_rtc con {"fecha_hora":"YYYY-MM-DDTHH:MM"} o
// {"fecha_hora":"YYYY-MM-DDTHH:MM:SS"} -- formato que manda tal cual un
// <input type="datetime-local"> de HTML sin convertir nada del lado del
// navegador. Pide ajustar el DS1307 a esa fecha/hora. 'segundo' viene en 0
// si el body no lo incluia (el input datetime-local normalmente no manda
// segundos).
typedef void (*servidor_web_cb_configurar_rtc_t)(uint16_t anio, uint8_t mes, uint8_t dia,
                                                  uint8_t hora, uint8_t minuto, uint8_t segundo);

// Llego POST /calibrar_sensor con {"sensor":"t1|t2|t3|t4|temp_gestor|humedad_gestor",
// "valor_referencia":N} -- pide calibrar ese sensor de a uno: app_main.c
// toma la ultima lectura CRUDA ya cacheada (nunca lee el hardware de nuevo
// desde aca), calcula constante = valor_referencia - lectura_cruda, la
// guarda en config.json (ver config_calibracion_t en config_braindlab.h) Y
// la aplica de inmediato en RAM -- a diferencia de /configurar_red y
// /configurar_climatizacion, esto SI se nota en caliente, sin reiniciar
// (maximo el tiempo de una vuelta del loop de climatizacion, 2s). 'sensor'
// es un puntero valido solo durante la llamada.
typedef void (*servidor_web_cb_calibrar_sensor_t)(const char *sensor, float valor_referencia);

// Llego POST /sd_formatear con {"confirmacion":"FORMATEAR"} (servidor_web.c
// ya valido la confirmacion). Formatea la MicroSD -- borra todo el
// historico. Bloquea hasta que termina; el esp_err_t que devuelve viaja al
// navegador (ESP_ERR_NOT_FOUND = no hay tarjeta, ver registro_sd_formatear()).
typedef esp_err_t (*servidor_web_cb_formatear_sd_t)(void);

typedef struct {
    servidor_web_cb_configurar_red_t on_configurar_red;                       // llego POST /configurar_red
    servidor_web_cb_configurar_climatizacion_t on_configurar_climatizacion;   // llego POST /configurar_climatizacion
    servidor_web_cb_control_aire_t on_control_aire;                           // llego POST /control_aire
    servidor_web_cb_control_bypass_t on_control_bypass;                       // llego POST /control_bypass
    servidor_web_cb_control_at_t on_control_at;                               // llego POST /control_at
    servidor_web_cb_control_automatico_t on_control_automatico;               // llego POST /control_automatico
    servidor_web_cb_configurar_rtc_t on_configurar_rtc;                       // llego POST /configurar_rtc
    servidor_web_cb_calibrar_sensor_t on_calibrar_sensor;                     // llego POST /calibrar_sensor
    servidor_web_cb_formatear_sd_t on_formatear_sd;                           // llego POST /sd_formatear
} servidor_web_callbacks_t;

// Registrar ANTES de llamar a servidor_web_init(), mismo criterio que
// uart_link_set_callbacks()/servidor_web_set_callbacks() en controlador_labgeo.
void servidor_web_set_callbacks(servidor_web_callbacks_t callbacks);

#ifdef __cplusplus
}
#endif

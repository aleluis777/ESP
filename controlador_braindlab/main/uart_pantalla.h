#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Enlace UART con la pantalla tactil braindlab (ESP32-S3), protocolo binario
// propio (ver protocolo_braindlab.h / PROTOCOLO_UART_BRAINDLAB.md). Mismo
// patron que uart_link.c en controlador_labgeo: esta placa manda el estado
// completo periodicamente, la pantalla manda pedidos de control manual
// (los mismos que ya atienden /control_aire, /control_bypass, /control_at,
// /control_automatico de servidor_web.c -- la pantalla es un cliente mas de
// ese mismo mecanismo, reusa los callbacks de app_main.c).
//
// Pines: TX=GPIO26 (P6), RX=GPIO39 (P1) -- ver HARDWARE.md §14 y la
// conversacion sobre el reparto de pines (GPIO25 quedo para el IRQ del
// W5500 en vez de esto).

// Callbacks para los 4 pedidos de control que puede mandar la pantalla --
// literalmente las mismas firmas que servidor_web_cb_control_*_t, para
// poder registrar las MISMAS funciones de app_main.c en los dos lados
// (HTTP y UART) sin duplicar logica.
typedef void (*uart_pantalla_cb_control_aire_t)(uint8_t indice, bool encendido);
typedef void (*uart_pantalla_cb_control_bypass_t)(bool solicitado);
typedef void (*uart_pantalla_cb_control_at_t)(bool activa);
typedef void (*uart_pantalla_cb_control_automatico_t)(void);

typedef struct {
    uart_pantalla_cb_control_aire_t on_control_aire;
    uart_pantalla_cb_control_bypass_t on_control_bypass;
    uart_pantalla_cb_control_at_t on_control_at;
    uart_pantalla_cb_control_automatico_t on_control_automatico;
} uart_pantalla_callbacks_t;

// Registrar ANTES de uart_pantalla_init(), mismo criterio que
// servidor_web_set_callbacks().
void uart_pantalla_set_callbacks(uart_pantalla_callbacks_t callbacks);

// Configura el UART fisico y lanza la tarea de recepcion.
void uart_pantalla_init(void);

// Manda BRAINDLAB_CMD_ESTADO_UPDATE -- mismos campos que publicar_estado_ws()
// manda por WebSocket (ver protocolo_braindlab.h para el detalle de cada
// uno). 'temperaturas'/'temp_gestor'/'humedad_gestor' van en grados/% C
// (float), esta funcion los convierte a decimas (i16) para el cable.
// 'anio'=0 (con mes/dia/hora/minuto/segundo en 0) si no hay RTC disponible
// esta vuelta -- mismo criterio que fecha_hora="----" en el JSON del WS.
// Energia (medidor JSY-MK-333G): voltajes/corrientes R, S, T; si
// energia_ok=false la pantalla muestra "--". 'sd_estado' es un
// registro_sd_estado_t (0=OK, 1=sin tarjeta, 2=sin formato, 3=error
// escritura). 'fallas' es el bitmask BRAINDLAB_FALLA_* de
// protocolo_braindlab.h (0 = todos los modulos OK).
void uart_pantalla_enviar_estado(uint8_t ciclo, const float temperaturas[4],
                                  const bool salida_aire[4], const bool aire_manual[4],
                                  uint8_t indice_reserva, bool alarma_at, bool alarma_at_manual,
                                  bool bypass_solicitado, bool bypass_manual, bool bypass_activo,
                                  bool modo_manual, bool eth_conectado,
                                  bool gestor_ok, float temp_gestor, float humedad_gestor,
                                  uint32_t uptime_s,
                                  uint16_t anio, uint8_t mes, uint8_t dia,
                                  uint8_t hora, uint8_t minuto, uint8_t segundo,
                                  bool energia_ok, const float voltajes[3], const float corrientes[3],
                                  uint8_t sd_estado, uint8_t fallas);

#ifdef __cplusplus
}
#endif

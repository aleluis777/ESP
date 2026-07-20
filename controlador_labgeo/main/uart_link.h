#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Callbacks invocados desde la tarea de recepcion UART cuando llega un
// comando de la pantalla. Se llaman desde esa tarea -- si hacen algo largo,
// que sea rapido o que disparen otra tarea, para no atrasar el parser.
typedef void (*uart_link_cb_run_t)(uint8_t run_id);

typedef struct {
    uart_link_cb_run_t on_start;        // llego CMD_START
    uart_link_cb_run_t on_stop;         // llego CMD_STOP
    uart_link_cb_run_t on_request_run;  // llego CMD_REQUEST_RUN
} uart_link_callbacks_t;

// Inicializa el UART y lanza la tarea de recepcion. Llamar despues de
// registrar los callbacks con uart_link_set_callbacks().
void uart_link_init(void);

void uart_link_set_callbacks(uart_link_callbacks_t callbacks);

// Manda una lectura en vivo (CMD_SENSOR_UPDATE). Pensado para llamarse hasta
// 5 veces por segundo. 'estado_ensayo' es el mismo status 0..4 del boton
// unico que ya se manda por WebSocket (ver campo "estado" en servidor_web.c)
// -- se agrega al final del payload para que la pantalla fisica pueda
// mostrar el boton correcto sin tener que rearmar su propia maquina de
// estados a partir de START/STOP.
void uart_link_enviar_sensor_update(uint8_t run_id, int32_t dial1_um, int32_t dial2_um,
                                     int32_t peso_mN, uint32_t tiempo_ms, uint8_t estado_ensayo);

// Manda un bloque de hasta LABGEO_CHUNK_MAX_PUNTOS puntos (CMD_RUN_CHUNK).
// 'puntos_buf' ya tiene que venir en el formato de 16 bytes/punto del
// protocolo (ver protocolo_labgeo.h) -- es literalmente lo que devuelve
// almacenamiento_leer_corrida() en cada chunk, sin reempacar.
void uart_link_enviar_run_chunk(uint8_t run_id, const uint8_t *puntos_buf, uint8_t count, uint8_t es_ultimo);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdint.h>
#include "ui_labgeo.h"
#include "ui_grafica.h"

#ifdef __cplusplus
extern "C" {
#endif

// Inicializa el UART hacia el controlador de sensores y lanza la tarea que
// recibe datos de forma asincrona (no bloquea la tarea de LVGL ni app_main).
// Llamar una sola vez, despues de crear ambas vistas (ui_labgeo_create /
// ui_grafica_create), para que la tarea de recepcion tenga donde volcar los
// valores recibidos.
void uart_labgeo_init(ui_labgeo_t *ui, ui_grafica_t *grafica);

// ---------- Comandos hacia el controlador (Pantalla -> Controlador) ----------

// Inicia la corrida run_id (1 = Primera, 2 = Segunda). El controlador es quien
// lleva el cronometro y empieza a mandar LABGEO_CMD_SENSOR_UPDATE.
void uart_labgeo_enviar_start(uint8_t run_id);

// Detiene la corrida activa.
void uart_labgeo_enviar_stop(uint8_t run_id);

// Pide los datos guardados de run_id para graficarlos. Limpia el chart de
// inmediato (queda vacio hasta que empiecen a llegar los LABGEO_CMD_RUN_CHUNK).
void uart_labgeo_enviar_request_run(uint8_t run_id);

#ifdef __cplusplus
}
#endif

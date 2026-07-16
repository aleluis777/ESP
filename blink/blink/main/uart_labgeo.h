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

// ---------- Estado remoto del boton unico (Controlador -> Pantalla) ----------

// El controlador manda, dentro de cada SENSOR_UPDATE, el mismo status 0..4
// del boton unico que ya usa la web (0=inicial, 1=corrida1 iniciada,
// 2=corrida1 finalizada, 3=corrida2 iniciada, 4=corrida2 finalizada) -- asi
// esta pantalla queda sincronizada sin importar si el cambio de estado lo
// disparo ella misma (via uart_labgeo_enviar_start/stop) o la web. Se llama
// desde la tarea de recepcion UART, con el LVGL lock ya tomado.
typedef void (*uart_labgeo_cb_estado_t)(uint8_t estado_ensayo);

// Registrar ANTES de uart_labgeo_init(), mismo criterio que el resto de los
// callbacks de este proyecto.
void uart_labgeo_set_cb_estado(uart_labgeo_cb_estado_t cb);

#ifdef __cplusplus
}
#endif

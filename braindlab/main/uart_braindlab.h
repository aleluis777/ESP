#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "ui_dashboard.h"
#include "ui_energia.h"

#ifdef __cplusplus
extern "C" {
#endif

// Enlace UART con controlador_braindlab (ESP32), protocolo binario propio
// (ver protocolo_braindlab.h / PROTOCOLO_UART_BRAINDLAB.md). Mismo patron
// que uart_labgeo.c en el proyecto blink: esta pantalla recibe el estado
// completo periodicamente y lo pinta en el dashboard; los pedidos de
// control manual (forzar AA/bypass/AT a mano) se mandan al controlador,
// que los atiende con el mismo mecanismo que ya usa la web (/control_aire,
// etc. en servidor_web.c).
//
// Pines confirmados: TX=IO17, RX=IO18 (mismos que usa blink, misma placa
// ESP32-S3 + LCD RGB 800x480 + tactil GT911 -- ver HARDWARE.md §14).

// Inicializa el UART y lanza la tarea de recepcion, que va a ir aplicando
// el estado recibido directamente sobre 'ui' y 'energia' (con el LVGL lock
// tomado). Llamar una sola vez, despues de ui_dashboard_create() y
// ui_energia_create().
void uart_braindlab_init(ui_dashboard_t *ui, ui_energia_t *energia);

// ---------- Comandos hacia el controlador (Pantalla -> Controlador) ----------

// Fuerza a mano AA(indice+1) (0..3). Persiste del lado del controlador hasta
// que la automatica cruce a otro ciclo (ver app_main.c de
// controlador_braindlab).
void uart_braindlab_enviar_control_aire(uint8_t indice, bool encendido);

// Fuerza a mano bypass_solicitado (BP_S).
void uart_braindlab_enviar_control_bypass(bool solicitado);

// Fuerza a mano OUT_AT (la alarma).
void uart_braindlab_enviar_control_at(bool activa);

// Cancela cualquier override manual vigente (AA1-4, bypass, AT), la
// automatica retoma el control de todo.
void uart_braindlab_enviar_control_automatico(void);

#ifdef __cplusplus
}
#endif

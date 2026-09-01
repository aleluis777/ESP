#pragma once

// Driver de las salidas digitales de la placa "Gestor de Aire Acondicionado"
// (ver HARDWARE.md): 4 reles de A/A (AA1-AA4), la alarma de alta temperatura
// (OUT_AT), la solicitud de bypass por software (BP_S) y su realimentacion
// (BPS_STATUS). No incluye el buzzer ni el pulsador de reset todavia.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SALIDAS_NUM_AIRES 4

// Reset de pines + secuencia de inicializacion obligatoria de HARDWARE.md
// §10 (AA1-4 y OUT_AT en LOW, BP_S en HIGH -- activo en bajo, HIGH = sin
// bypass solicitado). Llamar una sola vez al arrancar, antes de cualquier
// otra funcion de este modulo.
esp_err_t salidas_init(void);

// indice: 0=AA1, 1=AA2, 2=AA3, 3=AA4 (ver HARDWARE.md §5.1). Activo en ALTO.
void salidas_set_aire(uint8_t indice, bool encendido);

// OUT_AT (GPIO13), activo en ALTO -- contacto seco de alarma en header P2.
void salidas_set_alarma_at(bool activa);

// BP_S (GPIO14), activo en BAJO -- pide al bloque de logica de bypass que
// active el puente. 'solicitado'=true escribe el nivel BAJO (pide bypass).
void salidas_set_bypass_solicitado(bool solicitado);

// BPS_STATUS (GPIO34, solo entrada, activo en BAJO) -- estado FISICO real
// del nodo de bypass. HARDWARE.md §11: "nunca asumir que el bypass esta
// activo por haber escrito BP_S=0, siempre confirmar leyendo BPS_STATUS".
// Devuelve true si el bypass esta fisicamente activo.
bool salidas_leer_bypass_activo(void);

#ifdef __cplusplus
}
#endif

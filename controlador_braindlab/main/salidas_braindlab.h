#pragma once

// Driver de las salidas digitales de la placa "Gestor de Aire Acondicionado"
// (ver HARDWARE.md): 4 reles de A/A (AA1-AA4), la alarma de alta temperatura
// (OUT_AT), la solicitud de bypass por software (BP_S) y su realimentacion
// (BPS_STATUS), y el buzzer Z1. El pulsador de reset (SW1, GPIO35) no se
// usa -- decision de diseno, no es un pendiente.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SALIDAS_NUM_AIRES 4

// Reset de pines + secuencia de inicializacion obligatoria de HARDWARE.md
// §10 (AA1-4 y OUT_AT en LOW, BP_S en HIGH). Llamar una sola vez al
// arrancar, antes de cualquier otra funcion de este modulo. OJO: dejar BP_S
// en HIGH al arrancar es a proposito -- ver comentario de
// salidas_set_bypass_solicitado() de abajo, es el estado fail-safe.
esp_err_t salidas_init(void);

// indice: 0=AA1, 1=AA2, 2=AA3, 3=AA4 (ver HARDWARE.md §5.1). Activo en ALTO.
void salidas_set_aire(uint8_t indice, bool encendido);

// OUT_AT (GPIO13), activo en ALTO -- contacto seco de alarma en header P2.
void salidas_set_alarma_at(bool activa);

// BP_S (GPIO14) -- pide al bloque de logica de bypass (transistores
// Q6-Q13, ver esquematico) que active el puente. Logica INVERTIDA a
// proposito (confirmado contra el esquematico): HIGH = bypass activo (el
// bloque de logica lo entiende como "sin control, bypasear"), LOW = bypass
// no solicitado (operacion normal). Es fail-safe: si el ESP32 se apaga o
// se resetea, el pin vuelve a flotar/quedar en el default de arranque
// (HIGH, ver salidas_init()) y el bypass queda activo solo, conectando el
// A/A directo en vez de dejarlo sin control. 'solicitado'=true escribe
// HIGH (pide bypass).
void salidas_set_bypass_solicitado(bool solicitado);

// BPS_STATUS (GPIO34, solo entrada) -- estado FISICO real del nodo de
// bypass. Misma logica invertida que BP_S (confirmado contra el
// esquematico): HIGH = bypass fisicamente activo. HARDWARE.md §11: "nunca
// asumir que el bypass esta activo por haber escrito BP_S, siempre
// confirmar leyendo BPS_STATUS". Devuelve true si el bypass esta
// fisicamente activo.
bool salidas_leer_bypass_activo(void);

// Buzzer Z1 (GPIO15 via Q12, activo en ALTO -- HARDWARE.md §8.3). Buzzer
// activo: basta con poner el pin en alto, no hace falta PWM. Bloquea al
// task que lo llama durante 'duracion_ms' (usa vTaskDelay). Por ahora solo
// se usa para el pitido de arranque desde app_main().
void salidas_buzzer_pitido(uint32_t duracion_ms);

#ifdef __cplusplus
}
#endif

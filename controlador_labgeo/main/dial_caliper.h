#pragma once

// Driver para los "diales" digitales (protocolo tipico de calibre/dial
// indicator barato: 3 lineas REQ/DATA/CLK + GND, con un transistor en la
// linea REQ para simular el boton de "encendido/dato" del calibre).
//
// *** IMPORTANTE — esto esta implementado con la version mas comun y
// documentada de este protocolo (la que se ve reproducida en varios
// proyectos de hobbistas para calibres chinos baratos), pero no la pude
// validar contra tu hardware real. Cosas que casi seguro van a necesitar
// ajuste una vez que pruebes con el calibre fisico (todas son un solo
// #define al principio de dial_caliper.c, no hace falta tocar la logica):
//
//   - DIAL_REQ_ACTIVO_ALTO: si el transistor conduce con REQ en alto o en bajo.
//   - DIAL_BIT_ORDEN_LSB_PRIMERO: si el primer bit clockeado es el menos o el
//     mas significativo.
//   - El layout de signo/magnitud en dial_caliper_leer() (ahora asume bit 20
//     = signo, bits 0-19 = magnitud en centesimas de mm) — puede variar
//     segun el clon del chip.
//
// Para calibrar esto sin adivinar, usa dial_caliper_leer_crudo() (te da los
// 24 bits tal cual salieron del calibre) mientras mueves el dial una
// distancia conocida y comparas.

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t pin_req;
    gpio_num_t pin_clk;
    gpio_num_t pin_data;
} dial_caliper_t;

void dial_caliper_init(dial_caliper_t *d, gpio_num_t pin_req, gpio_num_t pin_clk, gpio_num_t pin_data);

// Lee los 24 bits crudos del calibre tal cual llegan (sin decodificar signo
// ni escala). Util para calibrar el protocolo contra el hardware real.
bool dial_caliper_leer_crudo(dial_caliper_t *d, uint32_t *dato_crudo, uint32_t timeout_ms);

// Lee y decodifica una posicion en centesimas de milimetro (con signo).
// dial_mm = valor / 100.0
bool dial_caliper_leer(dial_caliper_t *d, int32_t *valor_centesimas_mm, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

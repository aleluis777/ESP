#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

// Driver bit-bang para el HX711 (celda de carga). Protocolo estandar del
// chip: SCK en reposo en bajo; DOUT baja cuando hay un dato listo; se pulsa
// SCK 24 veces leyendo DOUT en cada pulso (MSB primero), mas pulsos extra
// para fijar la ganancia del siguiente reading (25 pulsos = canal A, ganancia
// 128 - el modo mas comun, el mismo que usaba el codigo anterior en Arduino).

typedef struct {
    gpio_num_t pin_dout;
    gpio_num_t pin_sck;
} hx711_t;

void hx711_init(hx711_t *h, gpio_num_t pin_dout, gpio_num_t pin_sck);

// true si el chip tiene un dato listo para leer (DOUT en bajo).
bool hx711_listo(hx711_t *h);

// Bloquea hasta 'timeout_ms' esperando un dato listo y lo lee.
// Devuelve true si pudo leer, false si hizo timeout.
bool hx711_leer(hx711_t *h, int32_t *valor_crudo, uint32_t timeout_ms);

// Promedia 'n' lecturas crudas (bloqueante). Devuelve true si pudo leer todas.
bool hx711_leer_promedio(hx711_t *h, uint8_t n, int32_t *valor_promedio, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

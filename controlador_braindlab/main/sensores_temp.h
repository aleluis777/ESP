#pragma once

// Lectura real de los 4 sensores de temperatura (T1-T4, NTC 10K/Beta=3950)
// via el ADS1115 "U2" (I2C, SDA=GPIO21 SCL=GPIO22, direccion 0x49 -- ADDR a
// 3V3 -- ver HARDWARE.md §4.2 y la conversacion sobre el esquematico). U1
// (el segundo ADS1115 del esquematico) no se usa todavia.

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sensores_temp_init(void);

// Llena temperaturas[0..3] (T1..T4, °C), leyendo AIN0..AIN3 del ADS1115 y
// aplicando la ecuacion Beta sobre cada divisor NTC (10K fijo + NTC 10K/3950
// a GND). Si algun canal falla la lectura, ese indice se deja con el ultimo
// valor bueno (temperaturas[] persiste entre llamadas del lado del que
// llama) y devuelve el error del ultimo canal que fallo -- nunca inventa un
// numero.
esp_err_t sensores_temp_leer(float temperaturas[4]);

#ifdef __cplusplus
}
#endif

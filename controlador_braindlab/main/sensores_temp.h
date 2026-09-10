#pragma once

// Lectura real de los 4 sensores de temperatura (T1-T4, NTC 10K/Beta=3950)
// via el ADS1115 "U2" (I2C, SDA=GPIO21 SCL=GPIO22, direccion 0x4A -- ADDR a
// SDA -- ver HARDWARE.md §4.2 y la conversacion sobre el esquematico). U1
// (el segundo ADS1115 del esquematico) no se usa todavia.

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sensores_temp_init(void);

// Llena temperaturas[0..3] (T1..T4, °C), leyendo AIN0..AIN3 del ADS1115 y
// aplicando la ecuacion Beta sobre cada divisor NTC (10K fijo + NTC 10K/3950
// a GND), sumando calibracion[i] al resultado de cada canal (ver
// config_calibracion_t en config_braindlab.h -- constante de un solo punto,
// 0.0 si no se calibro todavia). Si algun canal falla la lectura, ese indice
// se deja con el ultimo valor bueno (temperaturas[] persiste entre llamadas
// del lado del que llama) y devuelve el error del ultimo canal que fallo --
// nunca inventa un numero.
//
// OJO -- NO llamar desde mas de una tarea: no hay mutex ni proteccion
// contra llamadas concurrentes. Si dos tareas llaman a esto al mismo
// tiempo, una le puede cambiar el canal al ADS1115 mientras la otra todavia
// esta esperando su propia conversion, y terminan leyendo el resultado del
// canal equivocado. En este proyecto la unica que la llama es
// tarea_climatizacion (app_main.c); cualquier otro consumidor (por ejemplo
// la calibracion via POST /calibrar_sensor) debe leer la ULTIMA lectura ya
// cacheada por esa tarea (ver s_ultimas_temperaturas_crudas en app_main.c)
// en vez de volver a llamar a esta funcion.
esp_err_t sensores_temp_leer(float temperaturas[4], const float calibracion[4]);

#ifdef __cplusplus
}
#endif

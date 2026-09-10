#pragma once

// Sensor digital de temperatura + humedad del gabinete ("Gestor"), chip
// AM2301A (net "HUM", GPIO27 -- ver HARDWARE.md §3/§4.2). Compatible con el
// protocolo de un hilo de la familia AM2301/DHT21/DHT22/AM2302/AM2321, que
// es el que implementa el componente "dht" de esp-idf-lib (ver
// idf_component.yml) -- no confundir con el Dallas 1-Wire real (DS18B20),
// es un protocolo propio de pulsos.

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Lee temperatura (C) y humedad (%) del AM2301A, sumando calibracion_temp y
// calibracion_humedad al resultado crudo (constante de un solo punto, 0.0
// si no se calibro todavia -- ver config_calibracion_t en
// config_braindlab.h). No hace falta init previo (el driver "dht" configura
// el GPIO en cada lectura). Como es un sensor de un hilo con timing
// estricto, es normal que falle de vez en cuando -- quien llama debe seguir
// mostrando el ultimo valor bueno si esto devuelve error, no cortar el
// resto del sistema por esto.
esp_err_t sensor_gestor_leer(float *temperatura, float *humedad, float calibracion_temp, float calibracion_humedad);

#ifdef __cplusplus
}
#endif

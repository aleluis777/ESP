#pragma once

// Constantes de calibracion (pendiente/offset de cada sensor), guardadas en
// NVS. Reemplaza el uso de EEPROM del codigo Arduino anterior.
//
// dial_mm = dial_crudo * pendiente + offset
// peso_N  = (celda_cruda - offset) / pendiente   (misma formula que usaba el
//           codigo anterior: celda = (raw - CARGAA2) / (1000*CARGAB2))

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float dial1_pendiente;
    float dial1_offset;
    float dial2_pendiente;
    float dial2_offset;
    float celda_pendiente;
    float celda_offset;
} config_calibracion_t;

// Inicializa NVS (nvs_flash_init, con el manejo estandar de "borrar y
// reintentar" si la particion esta corrupta o cambio de version).
esp_err_t config_labgeo_init(void);

// Carga la calibracion guardada. Si no hay nada guardado todavia, devuelve
// valores por defecto neutros (pendiente=1, offset=0) sin marcar error.
void config_labgeo_cargar(config_calibracion_t *cfg);

esp_err_t config_labgeo_guardar(const config_calibracion_t *cfg);

#ifdef __cplusplus
}
#endif

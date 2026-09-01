#pragma once

// Configuracion del equipo guardada en SPIFFS "www" como /www/config.json --
// mismo patron que config_labgeo.c en controlador_labgeo: lectura con
// defaults campo por campo, escritura atomica (tmp + rename), y cada
// seccion se guarda sin pisar las demas.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Parametros de la maquina de estados de climatizacion (ver climatizacion.h).
// Los 4 umbrales son los mismos 4 que setpointArray[4] en neuvov2.ino, pero
// renombrados para que su rol sea explicito (el .ino original solo los
// indexaba 0..3 sin nombre).
typedef struct {
    uint8_t cantidad_aires;    // N: 1-4, cuantos canales de rele se manejan
    float   temp_min;          // apaga todo (histeresis, se vuelve a este umbral desde cualquier etapa)
    float   temp_max;          // activa la etapa normal (N-1 unidades encendidas)
    float   temp_at;           // activa la etapa de alta temperatura (las N unidades + alarma)
    float   temp_bypass;       // activa el bypass
    uint8_t fails_max_bypass;  // escalones a "alta temperatura" antes de deshabilitar el bypass automatico
    bool    rotar_reserva;     // si la unidad "de reserva" rota semanalmente entre las N configuradas
} config_climatizacion_t;

typedef struct {
    char ip[16];
    char gateway[16];
    char mascara[16];
} config_red_t;

// Monta la particion SPIFFS "www". Idempotente.
esp_err_t config_braindlab_init(void);

void      config_braindlab_cargar_climatizacion(config_climatizacion_t *cfg);
esp_err_t config_braindlab_guardar_climatizacion(const config_climatizacion_t *cfg);

void      config_braindlab_cargar_red(config_red_t *cfg);
esp_err_t config_braindlab_guardar_red(const config_red_t *cfg);

#ifdef __cplusplus
}
#endif

#pragma once

// Configuracion del equipo guardada en SPIFFS "www" como /www/config.json --
// mismo patron que config_labgeo.c en controlador_labgeo: lectura con
// defaults campo por campo, escritura atomica (tmp + rename), y cada
// seccion se guarda sin pisar las demas.
//
// Thread-safe: todas las funciones cargar/guardar toman un mutex interno --
// se llaman desde varias tareas (servidor HTTP, agente SNMP,
// tarea_climatizacion) y cada guardar es un leer-modificar-escribir del
// mismo archivo; sin el mutex, dos guardados simultaneos pueden pisarse y
// perder la seccion que guardo el otro. Llamar config_braindlab_init() antes
// que cualquier otra (crea el mutex).

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

// Calibracion de un solo punto por sensor: se suma esta constante a la
// lectura cruda antes de usarla en cualquier lado (WS, UART, climatizacion).
// Se calcula a mano comparando contra un termometro/higrometro de
// referencia: constante = valor_real - valor_leido (ej. referencia=25.0,
// leido=27.5 => constante=-2.5). Arranca en 0.0 (sin corregir) hasta que se
// calibre. t1..t4 son los 4 NTC analogicos (ADS1115, ver sensores_temp.c);
// temp_gestor/humedad_gestor son el AM2301A digital del gabinete (ver
// sensor_gestor.c).
typedef struct {
    float t1;
    float t2;
    float t3;
    float t4;
    float temp_gestor;
    float humedad_gestor;
} config_calibracion_t;

// Agente SNMP (ver snmp_braindlab.c). Las communities se aplican solo al
// arrancar (no se pueden cambiar por SNMP a proposito: un SET mal hecho
// dejaria al gestor sin acceso). Los destinos de trap si se pueden cambiar
// en caliente por SNMP SET (rama .5 de BRAINDTIC-MIB). trap_ip="0.0.0.0" o
// trap_habilitado=false = ese destino no recibe traps.
#define CONFIG_SNMP_NUM_DESTINOS 2

typedef struct {
    char community_lectura[32];
    char community_escritura[32];
    char community_trap[32];
    char trap_ip[CONFIG_SNMP_NUM_DESTINOS][16];
    bool trap_habilitado[CONFIG_SNMP_NUM_DESTINOS];
} config_snmp_t;

// Monta la particion SPIFFS "www". Idempotente.
esp_err_t config_braindlab_init(void);

void      config_braindlab_cargar_climatizacion(config_climatizacion_t *cfg);
esp_err_t config_braindlab_guardar_climatizacion(const config_climatizacion_t *cfg);

void      config_braindlab_cargar_red(config_red_t *cfg);
esp_err_t config_braindlab_guardar_red(const config_red_t *cfg);

void      config_braindlab_cargar_calibracion(config_calibracion_t *cfg);
esp_err_t config_braindlab_guardar_calibracion(const config_calibracion_t *cfg);

void      config_braindlab_cargar_snmp(config_snmp_t *cfg);
esp_err_t config_braindlab_guardar_snmp(const config_snmp_t *cfg);

#ifdef __cplusplus
}
#endif

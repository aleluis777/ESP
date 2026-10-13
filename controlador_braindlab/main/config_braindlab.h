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
    // Rotacion semanal (ver tarea_climatizacion en app_main.c): el aire que
    // descansa esta semana, 1..N ("aire_alternar" en config.json, editable a
    // mano -- se aplica al reiniciar). Cada lunes->martes 00:00 (RTC) pasa
    // al siguiente (N -> 1), se guarda y el equipo se reinicia.
    uint8_t aire_alternar;
    // "AAAA-MM-DD" del martes de la ultima rotacion ("" = nunca roto) --
    // evita rotar dos veces la misma semana (reinicio, corte de luz).
    char    ultima_rotacion[11];
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
    char nombre[32];     // sysName     (MIB-2) -- se aplica en caliente
    char ubicacion[48];  // sysLocation (MIB-2) -- se aplica en caliente
    char contacto[48];   // sysContact  (MIB-2) -- se aplica en caliente
    bool escritura_habilitada; // false = todo SET responde noAccess (solo monitoreo)
} config_snmp_t;

// Sensores 1..6 (ver snmp_braindlab.c / web). Hardware fijo: 1..4 = NTC
// T1..T4 (solo temperatura), 5 = AM2301A del gabinete (temperatura +
// humedad), 6 = sin hardware todavia (siempre no instalado). Un sensor no
// instalado no cuenta para el control, no genera alarmas y se oculta del
// panel. Se aplica en caliente.
#define CONFIG_NUM_SENSORES 6
#define CONFIG_SENSOR_NOMBRE_MAX 32

// Tambien guarda los nombres de los aires AA1..AA4 (clave "aires" de
// config.json): son los nombres del sitio que ven el panel y SNMP.
typedef struct {
    char nombre[CONFIG_NUM_SENSORES][CONFIG_SENSOR_NOMBRE_MAX];
    bool instalado[CONFIG_NUM_SENSORES];
    char nombre_aire[4][CONFIG_SENSOR_NOMBRE_MAX];
} config_sensores_t;

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

void      config_braindlab_cargar_sensores(config_sensores_t *cfg);
esp_err_t config_braindlab_guardar_sensores(const config_sensores_t *cfg);

#ifdef __cplusplus
}
#endif

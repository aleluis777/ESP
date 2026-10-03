#pragma once

// Registro historico en la MicroSD (SPI, comparte bus con el W5500 -- ver
// HARDWARE.md §4.1). Una fila por minuto, un archivo CSV por dia:
//
//   /sd/LOG/AAAA/MM/DD.CSV     (ej. /sd/LOG/2026/10/01.CSV)
//
// Asi graficar un dia es leer un solo archivo (~1440 filas). Para vistas de
// mes/anio la idea es sumar despues archivos de resumen (promedio/min/max
// por hora o por dia), que se pueden generar a partir de estos diarios.
//
// Pines: SCK=18, MOSI=23, MISO=19 (bus SPI del W5500), /CS=GPIO0. GPIO0 es
// strapping, pero el CS de la SD esta en ALTO en reposo, que es justo lo que
// el ESP32 necesita para arrancar normal (ver HARDWARE.md §3).
//
// La escritura la hace un task propio (tarea_registro_sd): quien registra
// solo encola la muestra y sigue, asi una SD lenta o colgada nunca frena la
// climatizacion. Si no hay tarjeta, el task reintenta montar cada minuto
// (haya o no muestras) -- se puede poner en caliente, sin reiniciar.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Una fila del CSV. Si el medidor no responde (energia_ok=false) las
// columnas de energia se escriben vacias.
typedef struct {
    struct tm fecha_hora;      // hora del RTC -- define el archivo del dia
    float temperaturas[4];     // NTC 1-4, calibradas
    bool sensores_temp_ok;
    float temp_gestor;         // AM2301A, calibrado
    float humedad_gestor;
    bool gestor_ok;
    bool salida_aire[4];       // AA1-4 aplicadas (auto + override manual)
    bool alarma_at;
    bool bypass_activo;        // BPS_STATUS, estado fisico real
    uint8_t ciclo;
    bool energia_ok;           // medidor JSY-MK-333G (modbus_braindlab.c) respondiendo
    float voltajes[3];         // R, S, T [V]
    float corrientes[3];       // R, S, T [A]
} registro_sd_muestra_t;

// Inicializa el bus SPI (si nadie lo hizo todavia) y monta la SD en /sd.
// LLAMAR ANTES DE red_eth_init(): asi el bus queda a nombre de este modulo
// y ethernet_init no lo libera (spi_bus_free) cuando red_eth.c reinicia el
// W5500 -- si lo liberara con la SD colgada, la SD quedaria sin bus. Si no
// hay tarjeta devuelve error pero el bus queda inicializado igual y el
// task sigue reintentando montar en cada muestra.
esp_err_t registro_sd_init(void);

// Encola una muestra para escribir. No bloquea: si la cola esta llena
// (SD colgada) la muestra se descarta con un warning.
void registro_sd_encolar(const registro_sd_muestra_t *muestra);

// true si la SD esta montada en este momento.
bool registro_sd_montada(void);

// Estado de la SD para mostrar en la web (WS "sd_estado") y en la pantalla
// (UART, ESTADO_UPDATE offset 48). Los valores numericos viajan tal cual
// por los dos canales -- no reordenar.
//   OK              montada y la ultima escritura salio bien
//   SIN_TARJETA     no responde: no esta puesta o esta mal conectada (sin
//                   pin de deteccion no se puede distinguir de una dañada)
//   SIN_FORMATO     responde pero no tiene FAT32 (no se formatea sola)
//   ERROR_ESCRITURA estaba montada y fallo una escritura (se saco, llena o
//                   dañada). Queda asi hasta que una escritura salga bien o
//                   el reintento de montaje diga otra cosa.
//   FORMATEANDO     registro_sd_formatear() en curso.
typedef enum {
    REGISTRO_SD_OK              = 0,
    REGISTRO_SD_SIN_TARJETA     = 1,
    REGISTRO_SD_SIN_FORMATO     = 2,
    REGISTRO_SD_ERROR_ESCRITURA = 3,
    REGISTRO_SD_FORMATEANDO     = 4,
} registro_sd_estado_t;

registro_sd_estado_t registro_sd_estado(void);

// Formatea la SD en FAT -- BORRA TODO EL HISTORICO. Lo hace tarea_registro_sd
// (asi nunca pisa una escritura en curso); esta funcion espera el resultado,
// bloqueando a quien la llama hasta 'timeout_ms' (puede tardar varios
// segundos segun el tamaño de la tarjeta). Funciona con la tarjeta montada
// o en estado SIN_FORMATO (ej. exFAT de fabrica). Devuelve:
//   ESP_OK                 formateada y montada, estado OK
//   ESP_ERR_NOT_FOUND      no hay tarjeta (o no responde)
//   ESP_ERR_INVALID_STATE  ya hay un formateo en curso
//   ESP_ERR_TIMEOUT        no termino en 'timeout_ms' (sigue en el task)
//   otro                   fallo el formateo
esp_err_t registro_sd_formatear(uint32_t timeout_ms);

// ---------- Lectura del historico (la usa servidor_web.c) ----------
//
// La carpeta del mes ES el indice de dias: no hay archivo de indice aparte
// que se pueda desincronizar (borrado a mano, corte de luz, formateo).

// Dias con datos del mes 'anio'/'mes' (1..12): llena dias[i] (1..31) y
// kb[i] (tamaño del archivo, para saber si el dia esta completo, ~170 KB, o
// parcial). Devuelve cuantos dias encontro (0 si la carpeta no existe) o -1
// si la SD no esta montada. Los dias salen ordenados.
int registro_sd_dias_con_datos(int anio, int mes, uint8_t dias[31], uint32_t kb[31]);

// Ruta del CSV de un dia ("/sd/LOG/2026/10/01.CSV") en 'ruta'. Devuelve
// false si la fecha es invalida o la SD no esta montada (no verifica que el
// archivo exista -- eso lo dice fopen()).
bool registro_sd_ruta_dia(int anio, int mes, int dia, char *ruta, size_t largo);

#ifdef __cplusplus
}
#endif

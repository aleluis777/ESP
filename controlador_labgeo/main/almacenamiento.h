#pragma once

// Guarda el historial de cada corrida en SPIFFS (flash interna), un archivo
// binario por corrida con el mismo layout de 12 bytes/punto que usa la
// trama RUN_CHUNK del protocolo (dial1_um i32, peso_mN i32, tiempo_ms u32,
// todo little-endian) -- asi se puede mandar el archivo casi tal cual llega
// del disco, sin reempacar.
//
// Nota: el enunciado original pedia "NVS/LittleFS"; se uso SPIFFS en su
// lugar porque viene nativo en ESP-IDF (LittleFS es un componente manejado
// que se descarga aparte) y para este uso -- pocos archivos, se reescriben
// enteros, se leen secuenciales -- rinde igual de bien.

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t almacenamiento_init(void);

// Empieza una corrida nueva: trunca (o crea) el archivo de esa corrida.
esp_err_t almacenamiento_iniciar_corrida(uint8_t run_id);

// Agrega un punto al final del archivo de la corrida (fopen "ab" + fwrite).
esp_err_t almacenamiento_agregar_punto(uint8_t run_id, int32_t dial1_um, int32_t peso_mN, uint32_t tiempo_ms);

// Recorre el archivo de la corrida en bloques de hasta LABGEO_CHUNK_MAX_PUNTOS
// puntos, invocando 'cb' por cada bloque (puntos_buf = bytes crudos, listos
// para copiar directo al payload de un RUN_CHUNK). Si la corrida no tiene
// datos guardados, llama a 'cb' una vez con count=0, es_ultimo=true.
typedef void (*almacenamiento_chunk_cb_t)(const uint8_t *puntos_buf, uint8_t count, bool es_ultimo, void *ctx);
esp_err_t almacenamiento_leer_corrida(uint8_t run_id, almacenamiento_chunk_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

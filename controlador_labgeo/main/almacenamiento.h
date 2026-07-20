#pragma once

// Guarda el historial de TODAS las corridas en un solo archivo CSV,
// /www/corridas.csv (particion SPIFFS "www", la misma del sitio web) --
// append-only, nunca se trunca ni se borra nada. Cada corrida arranca con
// una linea marcadora ("---CORRIDA1---" / "---CORRIDA2---") seguida de sus
// puntos ("dial1_um,dial2_um,peso_mN,tiempo_ms" por linea) hasta la proxima
// linea marcadora (de cualquier corrida) o el fin del archivo.
//
// Por que en "www" y no en una particion aparte "storage": para que el
// usuario pueda bajarlo directo desde el navegador (como calibracion.json/
// sistema.json) sin necesitar un endpoint especial -- decision explicita del
// usuario, sabiendo que un "idf.py flash" completo (no "app-flash") pisa
// toda la particion "www" y se lleva el historial con eso.
//
// Por que texto (CSV) y no binario: se puede abrir/procesar directo sin
// parsear un formato binario a mano, y como nunca se trunca, ningun dato
// viejo se pierde -- a cambio, leer "la ultima corrida de tipo N" hace falta
// buscar el ultimo marcador de ese tipo (ver almacenamiento_leer_corrida()).

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// No monta nada por su cuenta -- depende de que config_labgeo_init() ya haya
// montado "www" antes (mismo orden que ya tiene app_main.c). Devuelve
// ESP_FAIL si "www" todavia no esta montada, para detectar un orden de
// inicializacion incorrecto en vez de fallar en silencio mas adelante.
esp_err_t almacenamiento_init(void);

// Agrega la linea marcadora de una corrida nueva ("---CORRIDA1---" o
// "---CORRIDA2---") al FINAL de corridas.csv -- nunca trunca ni borra nada
// de sesiones anteriores, ni de esta misma corrida ni de la otra.
esp_err_t almacenamiento_iniciar_corrida(uint8_t run_id);

// Agrega un punto (una linea "dial1_um,dial2_um,peso_mN,tiempo_ms") al final
// de corridas.csv. Pertenece a la sesion mas reciente (la del ultimo
// marcador escrito por almacenamiento_iniciar_corrida()), por eso no hace
// falta repetir el run_id en cada linea.
esp_err_t almacenamiento_agregar_punto(uint8_t run_id, int32_t dial1_um, int32_t dial2_um, int32_t peso_mN,
                                        uint32_t tiempo_ms);

// Busca la ULTIMA sesion guardada de 'run_id' (puede haber varias, de
// distintos dias) y recorre solo esos puntos en bloques de hasta
// LABGEO_CHUNK_MAX_PUNTOS, invocando 'cb' por cada bloque -- los reempaqueta
// al formato binario de 16 bytes/punto (mismo layout que la trama RUN_CHUNK)
// para no cambiarle el contrato a quien ya llama a esta funcion. Si esa
// corrida nunca se corrio, llama a 'cb' una vez con count=0, es_ultimo=true.
typedef void (*almacenamiento_chunk_cb_t)(const uint8_t *puntos_buf, uint8_t count, bool es_ultimo, void *ctx);
esp_err_t almacenamiento_leer_corrida(uint8_t run_id, almacenamiento_chunk_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

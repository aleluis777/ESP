#pragma once

// Constantes de calibracion (pendiente/offset de cada sensor) y de sistema
// (red, etc.), guardadas como dos archivos separados en SPIFFS "www":
// /www/calibracion.json y /www/sistema.json -- pensado para que el usuario
// pueda descargarlos desde el navegador como "el seteo del equipo" (ver
// servidor_web.c, /files.html). Reemplaza el mecanismo anterior por NVS.
//
// dial_mm = dial_crudo * pendiente + offset
// peso_N  = (celda_cruda - offset) / pendiente

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

// Configuracion de red (IP/gateway/mascara) pedida por el usuario para
// configurar sin reflashear. Por ahora solo se guarda en config.json --
// aplicarla de verdad al W5500 (reemplazar los valores fijos de red_eth.c)
// es un paso pendiente, todavia no implementado.
typedef struct {
    char ip[16];       // "255.255.255.255" + '\0' = 16 bytes, alcanza siempre
    char gateway[16];
    char mascara[16];
} config_red_t;

// Datos generales del equipo -- nombre para identificarlo (por ejemplo si
// hay mas de uno en la misma red), diametro de la probeta en mm (grafica.html
// lo usa para el area y el desplazamiento de referencia del grafico "versus"
// de la Corrida 2), y la unidad en la que se quiere ver el peso en pantalla
// ("Kg" o "KN"). Igual que config_red_t: el firmware solo lo guarda, no hace
// ningun calculo con esto todavia.
typedef struct {
    char nombre[32];
    float diametro; // mm
    char unidad[4]; // "Kg" o "KN" + '\0'
} config_equipo_t;

// Monta la particion SPIFFS "www" (donde vive config.json, junto con el
// sitio estatico). Idempotente: si servidor_web_init() (u otra llamada
// previa) ya la monto, no hace nada y devuelve ESP_OK -- por eso es seguro
// llamarla primero, antes de saber si va a haber red o no.
esp_err_t config_labgeo_init(void);

// Lee config.json y carga los parametros de calibracion en 'cfg'. Si el
// archivo no existe, esta corrupto, o le falta algun campo puntual, ESE
// campo se completa con el valor por defecto (config_labgeo_defaults.h) --
// un campo faltante no descarta el resto del archivo.
void config_labgeo_cargar(config_calibracion_t *cfg);

// Guarda 'cfg' en config.json, bajo la clave "celda"/"dial1"/"dial2" --
// preserva cualquier otra seccion del archivo que ya hubiera (por ejemplo
// "red"), no lo pisa entero. Escritura atomica (escribe a un .tmp y hace
// rename() al final) para no dejar un JSON a medio escribir si se corta la
// luz a mitad de la escritura -- SPIFFS no es transaccional como era NVS.
esp_err_t config_labgeo_guardar(const config_calibracion_t *cfg);

// Mismo criterio que config_labgeo_cargar()/config_labgeo_guardar(), pero
// para la seccion "red" del archivo (IP/gateway/mascara).
void config_labgeo_cargar_red(config_red_t *cfg);
esp_err_t config_labgeo_guardar_red(const config_red_t *cfg);

// Mismo criterio, para la seccion "equipo" del archivo (nombre/area/
// desplazamiento/unidad).
void config_labgeo_cargar_equipo(config_equipo_t *cfg);
esp_err_t config_labgeo_guardar_equipo(const config_equipo_t *cfg);

#ifdef __cplusplus
}
#endif

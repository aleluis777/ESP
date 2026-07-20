// Guarda TODAS las corridas en un solo archivo CSV append-only,
// /www/corridas.csv -- ver almacenamiento.h para el porque de este diseno
// (un solo archivo, en "www", nunca se trunca).
//
// Formato del archivo:
//   ---CORRIDA1---
//   dial1_um,dial2_um,peso_mN,tiempo_ms
//   dial1_um,dial2_um,peso_mN,tiempo_ms
//   ---CORRIDA2---
//   dial1_um,dial2_um,peso_mN,tiempo_ms
//   ---CORRIDA1---
//   ...
//
// Una linea marcadora siempre empieza con "---" (ninguna linea de datos
// puede empezar asi: el primer campo es dial1_um, y aunque fuera negativo
// el signo es un solo '-' seguido de un digito, nunca "---") -- por eso
// alcanza con mirar los primeros 3 caracteres para distinguir "esto es un
// marcador" de "esto es un punto", sin ambiguedad.

#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "almacenamiento.h"
#include "protocolo_labgeo.h"
#include "esp_spiffs.h"
#include "esp_log.h"

static const char *TAG = "ALMACENAMIENTO";

#define RUTA_CORRIDAS "/www/corridas.csv"
#define PUNTO_BYTES 16

// Offset (en corridas.csv) del ULTIMO marcador escrito de cada run_id (1 o
// 2), cacheado en RAM para no tener que escanear el archivo entero cada vez
// que la pantalla pide ver una grafica (REQUEST_RUN) -- ver el comentario
// arriba de almacenamiento_leer_corrida(). Indice 0 sin usar (run_id es 1 o
// 2); -1 = "todavia no se escribio ninguna corrida de este tipo en este
// arranque", fuerza el escaneo de respaldo (por ejemplo, justo despues de
// reiniciar el equipo y pedir una corrida de antes del reinicio).
static long s_offset_ultima_corrida[3] = { -1, -1, -1 };

// Arma "---CORRIDA1---" / "---CORRIDA2---" segun run_id.
static void marcador_de(uint8_t run_id, char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "---CORRIDA%u---", (unsigned)run_id);
}

static bool es_linea_marcador(const char *linea)
{
    return strncmp(linea, "---", 3) == 0;
}

// No monta "www" -- eso ya lo hace config_labgeo_init(), que corre antes en
// app_main.c. Solo confirma que ya este montada, para detectar un orden de
// inicializacion incorrecto en vez de fallar en silencio mas adelante.
esp_err_t almacenamiento_init(void)
{
    if (!esp_spiffs_mounted("www")) {
        ESP_LOGE(TAG, "SPIFFS 'www' no esta montada todavia -- llamar config_labgeo_init() antes");
        return ESP_FAIL;
    }

    size_t total = 0, usado = 0;
    esp_spiffs_info("www", &total, &usado);
    ESP_LOGI(TAG, "SPIFFS 'www' lista para %s: %u/%u bytes usados", RUTA_CORRIDAS, (unsigned)usado, (unsigned)total);
    return ESP_OK;
}

// Agrega el marcador de una corrida nueva al final del archivo (fopen "a" =
// append, crea el archivo si es la primera vez que se llama a esto).
esp_err_t almacenamiento_iniciar_corrida(uint8_t run_id)
{
    FILE *f = fopen(RUTA_CORRIDAS, "a");
    if (!f) {
        ESP_LOGE(TAG, "No se pudo abrir %s para agregar el marcador de corrida", RUTA_CORRIDAS);
        return ESP_FAIL;
    }

    // fseek a SEEK_END antes de leer la posicion: en modo "a" el indicador de
    // posicion no queda confiable hasta la primera escritura en todos los
    // backends de stdio -- este fseek garantiza que offset_marcador sea
    // realmente donde va a arrancar la linea del marcador.
    fseek(f, 0, SEEK_END);
    long offset_marcador = ftell(f);

    char marcador[24];
    marcador_de(run_id, marcador, sizeof(marcador));
    fprintf(f, "%s\n", marcador);
    fclose(f);

    if (run_id == 1 || run_id == 2) {
        s_offset_ultima_corrida[run_id] = offset_marcador;
    }

    ESP_LOGI(TAG, "Corrida %u: marcador '%s' agregado en %s (append, no se borro nada)",
             (unsigned)run_id, marcador, RUTA_CORRIDAS);
    return ESP_OK;
}

// Agrega una linea CSV al final del archivo. No hace falta repetir el
// run_id en la linea -- ya queda implicito por el marcador que la precede.
esp_err_t almacenamiento_agregar_punto(uint8_t run_id, int32_t dial1_um, int32_t dial2_um, int32_t peso_mN,
                                        uint32_t tiempo_ms)
{
    (void)run_id;

    FILE *f = fopen(RUTA_CORRIDAS, "a");
    if (!f) {
        ESP_LOGE(TAG, "No se pudo abrir %s para agregar un punto", RUTA_CORRIDAS);
        return ESP_FAIL;
    }

    fprintf(f, "%" PRId32 ",%" PRId32 ",%" PRId32 ",%" PRIu32 "\n", dial1_um, dial2_um, peso_mN, tiempo_ms);
    fclose(f);
    return ESP_OK;
}

esp_err_t almacenamiento_leer_corrida(uint8_t run_id, almacenamiento_chunk_cb_t cb, void *ctx)
{
    char marcador[24];
    marcador_de(run_id, marcador, sizeof(marcador));
    size_t marcador_len = strlen(marcador);

    FILE *f = fopen(RUTA_CORRIDAS, "r");
    if (!f) {
        ESP_LOGW(TAG, "%s no existe todavia (nunca se guardo ninguna corrida)", RUTA_CORRIDAS);
        cb(NULL, 0, true, ctx);
        return ESP_OK;
    }

    char linea[80];
    long offset_ultima_sesion = (run_id == 1 || run_id == 2) ? s_offset_ultima_corrida[run_id] : -1;

    if (offset_ultima_sesion >= 0) {
        // Camino rapido: ya sabemos donde escribio almacenamiento_iniciar_corrida()
        // el marcador mas reciente de este run_id en ESTE arranque -- nos
        // ahorramos escanear corridas.csv entero (que solo crece, nunca se
        // trunca, y puede tener dias/semanas de historial acumulado).
        ESP_LOGI(TAG, "Corrida %u: usando offset cacheado %ld (sin escanear el archivo)",
                 (unsigned)run_id, offset_ultima_sesion);
    } else {
        // Camino lento de respaldo: solo hace falta si todavia no se
        // arranco ninguna corrida de este tipo en este arranque (por
        // ejemplo, justo despues de reiniciar el equipo y pedir ver una
        // corrida de antes del reinicio) -- recorre el archivo entero
        // buscando la ULTIMA linea que sea el marcador de ESTE run_id.
        long offset_linea = 0;
        while (fgets(linea, sizeof(linea), f)) {
            if (strncmp(linea, marcador, marcador_len) == 0) {
                offset_ultima_sesion = offset_linea;
            }
            offset_linea = ftell(f);
        }
    }

    if (offset_ultima_sesion < 0) {
        ESP_LOGW(TAG, "Corrida %u: nunca se guardo ninguna sesion todavia", (unsigned)run_id);
        fclose(f);
        cb(NULL, 0, true, ctx);
        return ESP_OK;
    }

    // Segunda pasada: parados justo en el marcador encontrado, lo salteamos
    // y leemos punto por punto hasta el PROXIMO marcador (de cualquier
    // corrida -- ahi es donde termina esta sesion) o el fin del archivo.
    // Se reempaqueta al formato binario de 16 bytes/punto en bloques de
    // hasta LABGEO_CHUNK_MAX_PUNTOS, igual que se manda por RUN_CHUNK -- no
    // cambia el contrato con quien llama a esta funcion.
    fseek(f, offset_ultima_sesion, SEEK_SET);
    fgets(linea, sizeof(linea), f); // saltea la linea del marcador en si

    uint8_t buf[PUNTO_BYTES * LABGEO_CHUNK_MAX_PUNTOS];
    uint8_t count = 0;
    long puntos_totales = 0;

    while (fgets(linea, sizeof(linea), f)) {
        if (es_linea_marcador(linea)) {
            break; // empezo la proxima sesion -- esta ya termino
        }

        int32_t dial1_um, dial2_um, peso_mN;
        uint32_t tiempo_ms;
        if (sscanf(linea, "%" SCNd32 ",%" SCNd32 ",%" SCNd32 ",%" SCNu32,
                   &dial1_um, &dial2_um, &peso_mN, &tiempo_ms) != 4) {
            ESP_LOGW(TAG, "Linea rara en %s, se ignora: %s", RUTA_CORRIDAS, linea);
            continue;
        }

        uint16_t idx = 0;
        uint8_t *punto = &buf[count * PUNTO_BYTES];
        labgeo_put_i32(punto, &idx, dial1_um);
        labgeo_put_i32(punto, &idx, dial2_um);
        labgeo_put_i32(punto, &idx, peso_mN);
        labgeo_put_u32(punto, &idx, tiempo_ms);
        count++;
        puntos_totales++;

        if (count == LABGEO_CHUNK_MAX_PUNTOS) {
            cb(buf, count, false, ctx);
            count = 0;
        }
    }

    fclose(f);
    cb(buf, count, true, ctx); // ultimo bloque (parcial o vacio) -- marca el final
    ESP_LOGI(TAG, "Corrida %u: %ld puntos de la ultima sesion enviados desde %s",
             (unsigned)run_id, puntos_totales, RUTA_CORRIDAS);
    return ESP_OK;
}

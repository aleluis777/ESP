// Guarda el historial de cada corrida como un archivo binario en SPIFFS
// (uno por corrida: corrida1.dat / corrida2.dat). Cada "punto" ocupa
// exactamente PUNTO_BYTES (12) y usa el mismo layout que la trama RUN_CHUNK
// del protocolo (dial1_um i32, peso_mN i32, tiempo_ms u32, little-endian) --
// asi almacenamiento_leer_corrida() puede pasarle los bytes crudos del
// archivo directo a uart_link sin tener que reempacar nada.

#include <stdio.h>
#include "almacenamiento.h"
#include "protocolo_labgeo.h"
#include "esp_spiffs.h"
#include "esp_log.h"

static const char *TAG = "ALMACENAMIENTO";

#define PUNTO_BYTES 12

// Monta la particion SPIFFS "storage" (definida en partitions.csv) en
// /spiffs. format_if_mount_failed=true significa que si la particion esta
// corrupta o es la primera vez que arranca, la formatea sola en vez de
// fallar.
esp_err_t almacenamiento_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo montar SPIFFS (%s)", esp_err_to_name(err));
        return err;
    }

    size_t total = 0, usado = 0;
    esp_spiffs_info("storage", &total, &usado);
    ESP_LOGI(TAG, "SPIFFS montado: %u/%u bytes usados", (unsigned)usado, (unsigned)total);
    return ESP_OK;
}

// Arma la ruta del archivo de una corrida (ej: run_id=1 -> "/spiffs/corrida1.dat").
static void ruta_corrida(uint8_t run_id, char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "/spiffs/corrida%u.dat", (unsigned)run_id);
}

// Se llama al arrancar una corrida nueva: abrir en modo "wb" trunca el
// archivo si ya existia de una corrida anterior con el mismo run_id, asi
// cada corrida empieza siempre desde cero puntos.
esp_err_t almacenamiento_iniciar_corrida(uint8_t run_id)
{
    char ruta[32];
    ruta_corrida(run_id, ruta, sizeof(ruta));

    FILE *f = fopen(ruta, "wb"); // "wb" trunca el archivo si ya existia
    if (!f) {
        ESP_LOGE(TAG, "No se pudo crear %s", ruta);
        return ESP_FAIL;
    }
    fclose(f);
    ESP_LOGI(TAG, "Corrida %u: archivo de datos reiniciado (%s)", (unsigned)run_id, ruta);
    return ESP_OK;
}

// Agrega un punto al final del archivo (modo "ab" = append). Lo llama
// programador_corrida.c cada vez que se cumple un checkpoint del
// cronograma de la corrida activa.
esp_err_t almacenamiento_agregar_punto(uint8_t run_id, int32_t dial1_um, int32_t peso_mN, uint32_t tiempo_ms)
{
    char ruta[32];
    ruta_corrida(run_id, ruta, sizeof(ruta));

    FILE *f = fopen(ruta, "ab");
    if (!f) {
        ESP_LOGE(TAG, "No se pudo abrir %s para agregar punto", ruta);
        return ESP_FAIL;
    }

    // Arma los 12 bytes del punto en el mismo formato little-endian que
    // usa la trama RUN_CHUNK (ver protocolo_labgeo.h).
    uint8_t buf[PUNTO_BYTES];
    uint16_t idx = 0;
    labgeo_put_i32(buf, &idx, dial1_um);
    labgeo_put_i32(buf, &idx, peso_mN);
    labgeo_put_u32(buf, &idx, tiempo_ms);

    size_t escritos = fwrite(buf, 1, PUNTO_BYTES, f);
    fclose(f);

    if (escritos != PUNTO_BYTES) {
        ESP_LOGE(TAG, "Escritura incompleta en %s (posible flash llena)", ruta);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Recorre el archivo de una corrida en bloques de hasta
// LABGEO_CHUNK_MAX_PUNTOS puntos (16), llamando a 'cb' por cada bloque. No
// carga el archivo entero en RAM -- lo lee de a pedazos del tamano justo
// para caber en una trama RUN_CHUNK.
esp_err_t almacenamiento_leer_corrida(uint8_t run_id, almacenamiento_chunk_cb_t cb, void *ctx)
{
    char ruta[32];
    ruta_corrida(run_id, ruta, sizeof(ruta));

    FILE *f = fopen(ruta, "rb");
    if (!f) {
        // Todavia no se corrio esta corrida (nunca se llamo a
        // almacenamiento_iniciar_corrida para este run_id). Avisamos con un
        // "chunk vacio y ultimo" para que quien pidio los datos no se quede
        // esperando mas tramas que nunca van a llegar.
        ESP_LOGW(TAG, "Corrida %u sin datos guardados (%s no existe)", (unsigned)run_id, ruta);
        cb(NULL, 0, true, ctx);
        return ESP_OK;
    }

    // Medimos el tamano total del archivo primero, para saber en que
    // momento el bloque que estamos mandando es el ultimo.
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t buf[PUNTO_BYTES * LABGEO_CHUNK_MAX_PUNTOS];
    long leido_total = 0;

    while (1) {
        size_t leidos = fread(buf, 1, sizeof(buf), f);
        uint8_t count = (uint8_t)(leidos / PUNTO_BYTES);
        leido_total += (long)count * PUNTO_BYTES;
        bool es_ultimo = (leido_total >= total);
        cb(buf, count, es_ultimo, ctx);
        if (es_ultimo) {
            break;
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "Corrida %u: %ld bytes enviados desde %s", (unsigned)run_id, total, ruta);
    return ESP_OK;
}

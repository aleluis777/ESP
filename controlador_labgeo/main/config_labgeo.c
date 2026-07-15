// Guarda/lee la configuracion del equipo como dos archivos JSON separados
// en la particion SPIFFS "www" (la misma del sitio estatico):
//   - calibracion.json: pendiente/offset de cada sensor (celda, diales).
//   - sistema.json: configuracion general (red por ahora; nombre del
//     equipo u otras opciones despues).
// Separados a proposito: son datos con ciclos de vida distintos (la
// celda se recalibra seguido, la red casi nunca cambia) y asi una
// escritura de uno no arriesga corromper al otro. El usuario los puede
// bajar por separado desde el navegador como "el seteo del equipo".
//
// Cada archivo puede tener mas de una seccion (calibracion.json hoy tiene
// "celda"/"dial1"/"dial2") -- por eso guardar_seccion() SIEMPRE lee primero
// todo el arbol JSON que ya hay en ese archivo, actualiza solo la seccion
// pedida, y recien ahi escribe todo de vuelta. Si en cambio cada guardado
// armara un JSON nuevo desde cero, guardar un sensor pisaria (borraria) la
// calibracion ya guardada de los otros.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config_labgeo.h"
#include "config_labgeo_defaults.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "CONFIG_LABGEO";
#define RUTA_CALIBRACION     "/www/calibracion.json"
#define RUTA_CALIBRACION_TMP "/www/calibracion.json.tmp"
#define RUTA_SISTEMA         "/www/sistema.json"
#define RUTA_SISTEMA_TMP     "/www/sistema.json.tmp"

// Idempotente: si servidor_web_init() (o una llamada anterior a esta misma
// funcion) ya monto "www", no hace nada. Hace falta que quede montada ANTES
// de que corran los sensores (que no dependen de la red), por eso este
// modulo es dueno del montaje en vez de depender de que arranque el
// servidor web primero.
esp_err_t config_labgeo_init(void)
{
    if (esp_spiffs_mounted("www")) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/www",
        .partition_label = "www",
        .max_files = 6, // +2 respecto al original: cada archivo puede tener su .tmp abierto a la vez que otro
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo montar la particion 'www' (%s)", esp_err_to_name(err));
    }
    return err;
}

// Lee 'ruta' completo y lo devuelve parseado. Si no existe o esta corrupto,
// devuelve un objeto JSON vacio (nunca NULL) -- asi los que leen una
// seccion puntual no tienen que manejar el caso NULL aparte del "no tiene
// esta clave todavia".
static cJSON *cargar_json_archivo(const char *ruta)
{
    FILE *f = fopen(ruta, "r");
    if (!f) {
        return cJSON_CreateObject();
    }

    char buf[768];
    size_t leidos = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[leidos] = '\0';

    cJSON *raiz = cJSON_Parse(buf);
    if (!raiz) {
        ESP_LOGW(TAG, "%s invalido (JSON corrupto), se trata como vacio", ruta);
        return cJSON_CreateObject();
    }
    return raiz;
}

// Serializa 'raiz' completo y lo escribe en 'ruta_final' (via 'ruta_tmp').
// Escritura atomica: SPIFFS no es transaccional como era NVS -- si el chip
// se resetea a mitad de escribir el archivo directamente, queda un JSON
// truncado (corrupto). Escribiendo primero al .tmp y recien al final
// haciendo rename() al nombre real, nunca hay un instante en que el
// archivo real exista a medio escribir.
//
// OJO con SPIFFS_rename(): a diferencia del rename() POSIX normal, SPIFFS
// NO permite renombrar sobre un destino que ya existe -- devuelve
// SPIFFS_ERR_CONFLICTING_NAME y falla. Por eso hace falta borrar el destino
// primero (remove(), ignorando el error si no existia) antes del rename().
// Sigue siendo mucho mejor que escribir directo: la ventana de riesgo pasa
// a ser "se corta la luz justo entre el remove y el rename" (falta el
// archivo hasta el proximo guardado exitoso) en vez de "JSON truncado e
// invalido para siempre" (que es lo que pasaba escribiendo directo).
static esp_err_t guardar_json_archivo(const char *ruta_tmp, const char *ruta_final, const cJSON *raiz)
{
    char *texto = cJSON_Print(raiz);
    if (!texto) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(ruta_tmp, "w");
    if (!f) {
        free(texto);
        return ESP_FAIL;
    }
    size_t escritos = fwrite(texto, 1, strlen(texto), f);
    fclose(f);
    free(texto);

    if (escritos == 0) {
        return ESP_FAIL;
    }

    remove(ruta_final); // ignorar error: es normal que no exista todavia (primer guardado)

    if (rename(ruta_tmp, ruta_final) != 0) {
        ESP_LOGE(TAG, "rename() de %s a %s fallo", ruta_tmp, ruta_final);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Lee un campo numerico/string de un objeto JSON; si no existe o no es del
// tipo esperado, devuelve el default sin marcar error (asi un campo
// faltante no tira toda la seccion).
static float leer_campo_num(const cJSON *obj, const char *clave, float valor_default)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, clave);
    return cJSON_IsNumber(item) ? (float)item->valuedouble : valor_default;
}

static void leer_campo_str(const cJSON *obj, const char *clave, char *destino, size_t destino_len,
                            const char *valor_default)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, clave);
    const char *origen = cJSON_IsString(item) ? item->valuestring : valor_default;
    strncpy(destino, origen, destino_len - 1);
    destino[destino_len - 1] = '\0';
}

// ---------------------------------------------------------------------------
// calibracion.json: celda + dial1 + dial2
// ---------------------------------------------------------------------------

void config_labgeo_cargar(config_calibracion_t *cfg)
{
    // Defaults primero, siempre -- si algo falla mas abajo (archivo
    // ausente, JSON corrupto, objeto de un sensor faltante), 'cfg' ya queda
    // en un estado valido y el sistema arranca igual.
    cfg->celda_offset    = CELDA_OFFSET_DEFAULT;
    cfg->celda_pendiente = CELDA_PENDIENTE_DEFAULT;
    cfg->dial1_offset    = DIAL1_OFFSET_DEFAULT;
    cfg->dial1_pendiente = DIAL1_PENDIENTE_DEFAULT;
    cfg->dial2_offset    = DIAL2_OFFSET_DEFAULT;
    cfg->dial2_pendiente = DIAL2_PENDIENTE_DEFAULT;

    cJSON *raiz = cargar_json_archivo(RUTA_CALIBRACION);

    const cJSON *celda = cJSON_GetObjectItemCaseSensitive(raiz, "celda");
    if (cJSON_IsObject(celda)) {
        cfg->celda_offset    = leer_campo_num(celda, "offset", CELDA_OFFSET_DEFAULT);
        cfg->celda_pendiente = leer_campo_num(celda, "pendiente", CELDA_PENDIENTE_DEFAULT);
    }

    const cJSON *dial1 = cJSON_GetObjectItemCaseSensitive(raiz, "dial1");
    if (cJSON_IsObject(dial1)) {
        cfg->dial1_offset    = leer_campo_num(dial1, "offset", DIAL1_OFFSET_DEFAULT);
        cfg->dial1_pendiente = leer_campo_num(dial1, "pendiente", DIAL1_PENDIENTE_DEFAULT);
    }

    const cJSON *dial2 = cJSON_GetObjectItemCaseSensitive(raiz, "dial2");
    if (cJSON_IsObject(dial2)) {
        cfg->dial2_offset    = leer_campo_num(dial2, "offset", DIAL2_OFFSET_DEFAULT);
        cfg->dial2_pendiente = leer_campo_num(dial2, "pendiente", DIAL2_PENDIENTE_DEFAULT);
    }

    cJSON_Delete(raiz);
    ESP_LOGI(TAG, "Calibracion cargada de %s", RUTA_CALIBRACION);
}

esp_err_t config_labgeo_guardar(const config_calibracion_t *cfg)
{
    cJSON *raiz = cargar_json_archivo(RUTA_CALIBRACION); // conserva los otros sensores que no se estan tocando ahora

    cJSON_DeleteItemFromObject(raiz, "celda");
    cJSON *celda = cJSON_CreateObject();
    cJSON_AddNumberToObject(celda, "offset", cfg->celda_offset);
    cJSON_AddNumberToObject(celda, "pendiente", cfg->celda_pendiente);
    cJSON_AddItemToObject(raiz, "celda", celda);

    cJSON_DeleteItemFromObject(raiz, "dial1");
    cJSON *dial1 = cJSON_CreateObject();
    cJSON_AddNumberToObject(dial1, "offset", cfg->dial1_offset);
    cJSON_AddNumberToObject(dial1, "pendiente", cfg->dial1_pendiente);
    cJSON_AddItemToObject(raiz, "dial1", dial1);

    cJSON_DeleteItemFromObject(raiz, "dial2");
    cJSON *dial2 = cJSON_CreateObject();
    cJSON_AddNumberToObject(dial2, "offset", cfg->dial2_offset);
    cJSON_AddNumberToObject(dial2, "pendiente", cfg->dial2_pendiente);
    cJSON_AddItemToObject(raiz, "dial2", dial2);

    esp_err_t err = guardar_json_archivo(RUTA_CALIBRACION_TMP, RUTA_CALIBRACION, raiz);
    cJSON_Delete(raiz);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Calibracion guardada en %s", RUTA_CALIBRACION);
    }
    return err;
}

// ---------------------------------------------------------------------------
// sistema.json: red (y lo que se sume despues -- nombre del equipo, etc.)
// ---------------------------------------------------------------------------

void config_labgeo_cargar_red(config_red_t *cfg)
{
    cJSON *raiz = cargar_json_archivo(RUTA_SISTEMA);

    const cJSON *red = cJSON_GetObjectItemCaseSensitive(raiz, "red");
    if (cJSON_IsObject(red)) {
        leer_campo_str(red, "ip", cfg->ip, sizeof(cfg->ip), RED_IP_DEFAULT);
        leer_campo_str(red, "gateway", cfg->gateway, sizeof(cfg->gateway), RED_GATEWAY_DEFAULT);
        leer_campo_str(red, "mascara", cfg->mascara, sizeof(cfg->mascara), RED_MASCARA_DEFAULT);
    } else {
        strncpy(cfg->ip, RED_IP_DEFAULT, sizeof(cfg->ip) - 1);
        cfg->ip[sizeof(cfg->ip) - 1] = '\0';
        strncpy(cfg->gateway, RED_GATEWAY_DEFAULT, sizeof(cfg->gateway) - 1);
        cfg->gateway[sizeof(cfg->gateway) - 1] = '\0';
        strncpy(cfg->mascara, RED_MASCARA_DEFAULT, sizeof(cfg->mascara) - 1);
        cfg->mascara[sizeof(cfg->mascara) - 1] = '\0';
    }

    cJSON_Delete(raiz);
    ESP_LOGI(TAG, "Configuracion de red cargada de %s", RUTA_SISTEMA);
}

esp_err_t config_labgeo_guardar_red(const config_red_t *cfg)
{
    cJSON *raiz = cargar_json_archivo(RUTA_SISTEMA); // conserva otras secciones de sistema.json que se sumen despues

    cJSON_DeleteItemFromObject(raiz, "red");
    cJSON *red = cJSON_CreateObject();
    cJSON_AddStringToObject(red, "ip", cfg->ip);
    cJSON_AddStringToObject(red, "gateway", cfg->gateway);
    cJSON_AddStringToObject(red, "mascara", cfg->mascara);
    cJSON_AddItemToObject(raiz, "red", red);

    esp_err_t err = guardar_json_archivo(RUTA_SISTEMA_TMP, RUTA_SISTEMA, raiz);
    cJSON_Delete(raiz);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Configuracion de red guardada en %s (ip=%s gw=%s mask=%s) -- todavia no aplicada, falta el paso 3",
                 RUTA_SISTEMA, cfg->ip, cfg->gateway, cfg->mascara);
    }
    return err;
}

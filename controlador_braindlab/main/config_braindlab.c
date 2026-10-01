// Ver config_braindlab.h. Estructura calcada de config_labgeo.c
// (controlador_labgeo): un unico archivo /www/config.json con secciones
// ("climatizacion", "red"), lectura de un objeto JSON vacio si el archivo no
// existe o esta corrupto (nunca NULL), y escritura atomica via .tmp + rename
// para no dejar el JSON a medio escribir si se corta la luz.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "config_braindlab.h"
#include "config_braindlab_defaults.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "CONFIG_BRAINDLAB";
#define RUTA_CONFIG     "/www/config.json"
#define RUTA_CONFIG_TMP "/www/config.json.tmp"

// Tamanio maximo de config.json que se lee. Con la seccion "snmp" el JSON
// formateado (cJSON_Print, con tabs) ya ronda los 700 bytes: el buffer de
// 768 que habia antes quedaba justo y cortaba el archivo en silencio (JSON
// corrupto => TODAS las secciones vuelven a sus defaults). Va al heap y no
// al stack porque se llama desde tareas con stack chico.
#define CONFIG_MAX_BYTES 2048

// Ver "Thread-safe" en config_braindlab.h. Se crea en config_braindlab_init().
static SemaphoreHandle_t s_mutex = NULL;

static void bloquear(void)
{
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void desbloquear(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

esp_err_t config_braindlab_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (esp_spiffs_mounted("www")) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/www",
        .partition_label = "www",
        .max_files = 6,
        // false: ahora main/CMakeLists.txt graba un sitio real en la
        // particion al flashear (spiffs_create_partition_image desde
        // ../web, igual que controlador_labgeo) -- si el mount falla
        // preferimos verlo en el log antes que formatear y perder ese
        // sitio o un config.json ya guardado.
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo montar la particion 'www' (%s)", esp_err_to_name(err));
    }
    return err;
}

static cJSON *cargar_json_archivo(const char *ruta)
{
    FILE *f = fopen(ruta, "r");
    if (!f) {
        return cJSON_CreateObject();
    }

    char *buf = malloc(CONFIG_MAX_BYTES);
    if (!buf) {
        fclose(f);
        ESP_LOGE(TAG, "Sin memoria para leer %s, se trata como vacio", ruta);
        return cJSON_CreateObject();
    }
    size_t leidos = fread(buf, 1, CONFIG_MAX_BYTES - 1, f);
    fclose(f);
    buf[leidos] = '\0';
    if (leidos == CONFIG_MAX_BYTES - 1) {
        ESP_LOGW(TAG, "%s llena el buffer de lectura (%d bytes) -- probablemente cortado", ruta, CONFIG_MAX_BYTES);
    }

    cJSON *raiz = cJSON_Parse(buf);
    free(buf);
    if (!raiz) {
        ESP_LOGW(TAG, "%s invalido (JSON corrupto), se trata como vacio", ruta);
        return cJSON_CreateObject();
    }
    return raiz;
}

static esp_err_t guardar_json_archivo(const cJSON *raiz)
{
    char *texto = cJSON_Print(raiz);
    if (!texto) {
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(RUTA_CONFIG_TMP, "w");
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

    remove(RUTA_CONFIG); // ignorar error: es normal que no exista todavia (primer guardado)

    if (rename(RUTA_CONFIG_TMP, RUTA_CONFIG) != 0) {
        ESP_LOGE(TAG, "rename() de %s a %s fallo", RUTA_CONFIG_TMP, RUTA_CONFIG);
        return ESP_FAIL;
    }
    return ESP_OK;
}

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
// Seccion "climatizacion"
// ---------------------------------------------------------------------------

void config_braindlab_cargar_climatizacion(config_climatizacion_t *cfg)
{
    cfg->cantidad_aires   = CANTIDAD_AIRES_DEFAULT;
    cfg->temp_min         = TEMP_MIN_DEFAULT;
    cfg->temp_max         = TEMP_MAX_DEFAULT;
    cfg->temp_at          = TEMP_AT_DEFAULT;
    cfg->temp_bypass      = TEMP_BYPASS_DEFAULT;
    cfg->fails_max_bypass = FAILS_MAX_BYPASS_DEFAULT;
    cfg->rotar_reserva    = ROTAR_RESERVA_DEFAULT;

    bloquear();
    cJSON *raiz = cargar_json_archivo(RUTA_CONFIG);

    const cJSON *clima = cJSON_GetObjectItemCaseSensitive(raiz, "climatizacion");
    if (cJSON_IsObject(clima)) {
        cfg->cantidad_aires   = (uint8_t)leer_campo_num(clima, "cantidad_aires", CANTIDAD_AIRES_DEFAULT);
        cfg->temp_min         = leer_campo_num(clima, "temp_min", TEMP_MIN_DEFAULT);
        cfg->temp_max         = leer_campo_num(clima, "temp_max", TEMP_MAX_DEFAULT);
        cfg->temp_at          = leer_campo_num(clima, "temp_at", TEMP_AT_DEFAULT);
        cfg->temp_bypass      = leer_campo_num(clima, "temp_bypass", TEMP_BYPASS_DEFAULT);
        cfg->fails_max_bypass = (uint8_t)leer_campo_num(clima, "fails_max_bypass", FAILS_MAX_BYPASS_DEFAULT);
        const cJSON *rotar = cJSON_GetObjectItemCaseSensitive(clima, "rotar_reserva");
        cfg->rotar_reserva = cJSON_IsBool(rotar) ? cJSON_IsTrue(rotar) : ROTAR_RESERVA_DEFAULT;
    }

    if (cfg->cantidad_aires < 1 || cfg->cantidad_aires > 4) {
        ESP_LOGW(TAG, "cantidad_aires=%u fuera de rango (1-4), forzando a %d",
                 cfg->cantidad_aires, CANTIDAD_AIRES_DEFAULT);
        cfg->cantidad_aires = CANTIDAD_AIRES_DEFAULT;
    }

    cJSON_Delete(raiz);
    desbloquear();
    ESP_LOGI(TAG, "Climatizacion cargada: N=%u tmin=%.2f tmax=%.2f at=%.2f bypass=%.2f fails_max=%u rotar=%d",
             cfg->cantidad_aires, cfg->temp_min, cfg->temp_max, cfg->temp_at, cfg->temp_bypass,
             cfg->fails_max_bypass, cfg->rotar_reserva);
}

esp_err_t config_braindlab_guardar_climatizacion(const config_climatizacion_t *cfg)
{
    bloquear();
    cJSON *raiz = cargar_json_archivo(RUTA_CONFIG); // conserva "red" y cualquier otra seccion

    cJSON_DeleteItemFromObject(raiz, "climatizacion");
    cJSON *clima = cJSON_CreateObject();
    cJSON_AddNumberToObject(clima, "cantidad_aires", cfg->cantidad_aires);
    cJSON_AddNumberToObject(clima, "temp_min", cfg->temp_min);
    cJSON_AddNumberToObject(clima, "temp_max", cfg->temp_max);
    cJSON_AddNumberToObject(clima, "temp_at", cfg->temp_at);
    cJSON_AddNumberToObject(clima, "temp_bypass", cfg->temp_bypass);
    cJSON_AddNumberToObject(clima, "fails_max_bypass", cfg->fails_max_bypass);
    cJSON_AddBoolToObject(clima, "rotar_reserva", cfg->rotar_reserva);
    cJSON_AddItemToObject(raiz, "climatizacion", clima);

    esp_err_t err = guardar_json_archivo(raiz);
    cJSON_Delete(raiz);
    desbloquear();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Climatizacion guardada en %s", RUTA_CONFIG);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Seccion "red"
// ---------------------------------------------------------------------------

void config_braindlab_cargar_red(config_red_t *cfg)
{
    bloquear();
    cJSON *raiz = cargar_json_archivo(RUTA_CONFIG);

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
    desbloquear();
    ESP_LOGI(TAG, "Configuracion de red cargada de %s", RUTA_CONFIG);
}

esp_err_t config_braindlab_guardar_red(const config_red_t *cfg)
{
    bloquear();
    cJSON *raiz = cargar_json_archivo(RUTA_CONFIG);

    cJSON_DeleteItemFromObject(raiz, "red");
    cJSON *red = cJSON_CreateObject();
    cJSON_AddStringToObject(red, "ip", cfg->ip);
    cJSON_AddStringToObject(red, "gateway", cfg->gateway);
    cJSON_AddStringToObject(red, "mascara", cfg->mascara);
    cJSON_AddItemToObject(raiz, "red", red);

    esp_err_t err = guardar_json_archivo(raiz);
    cJSON_Delete(raiz);
    desbloquear();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Configuracion de red guardada en %s (ip=%s gw=%s mask=%s)",
                 RUTA_CONFIG, cfg->ip, cfg->gateway, cfg->mascara);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Seccion "calibracion"
// ---------------------------------------------------------------------------

void config_braindlab_cargar_calibracion(config_calibracion_t *cfg)
{
    cfg->t1             = CALIBRACION_DEFAULT;
    cfg->t2             = CALIBRACION_DEFAULT;
    cfg->t3             = CALIBRACION_DEFAULT;
    cfg->t4             = CALIBRACION_DEFAULT;
    cfg->temp_gestor    = CALIBRACION_DEFAULT;
    cfg->humedad_gestor = CALIBRACION_DEFAULT;

    bloquear();
    cJSON *raiz = cargar_json_archivo(RUTA_CONFIG);

    const cJSON *calib = cJSON_GetObjectItemCaseSensitive(raiz, "calibracion");
    if (cJSON_IsObject(calib)) {
        cfg->t1             = leer_campo_num(calib, "t1", CALIBRACION_DEFAULT);
        cfg->t2             = leer_campo_num(calib, "t2", CALIBRACION_DEFAULT);
        cfg->t3             = leer_campo_num(calib, "t3", CALIBRACION_DEFAULT);
        cfg->t4             = leer_campo_num(calib, "t4", CALIBRACION_DEFAULT);
        cfg->temp_gestor    = leer_campo_num(calib, "temp_gestor", CALIBRACION_DEFAULT);
        cfg->humedad_gestor = leer_campo_num(calib, "humedad_gestor", CALIBRACION_DEFAULT);
    }

    cJSON_Delete(raiz);
    desbloquear();
    ESP_LOGI(TAG, "Calibracion cargada: t1=%+.2f t2=%+.2f t3=%+.2f t4=%+.2f temp_gestor=%+.2f humedad_gestor=%+.2f",
             cfg->t1, cfg->t2, cfg->t3, cfg->t4, cfg->temp_gestor, cfg->humedad_gestor);
}

esp_err_t config_braindlab_guardar_calibracion(const config_calibracion_t *cfg)
{
    bloquear();
    cJSON *raiz = cargar_json_archivo(RUTA_CONFIG); // conserva "climatizacion"/"red"

    cJSON_DeleteItemFromObject(raiz, "calibracion");
    cJSON *calib = cJSON_CreateObject();
    cJSON_AddNumberToObject(calib, "t1", cfg->t1);
    cJSON_AddNumberToObject(calib, "t2", cfg->t2);
    cJSON_AddNumberToObject(calib, "t3", cfg->t3);
    cJSON_AddNumberToObject(calib, "t4", cfg->t4);
    cJSON_AddNumberToObject(calib, "temp_gestor", cfg->temp_gestor);
    cJSON_AddNumberToObject(calib, "humedad_gestor", cfg->humedad_gestor);
    cJSON_AddItemToObject(raiz, "calibracion", calib);

    esp_err_t err = guardar_json_archivo(raiz);
    cJSON_Delete(raiz);
    desbloquear();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Calibracion guardada en %s", RUTA_CONFIG);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Seccion "snmp"
// ---------------------------------------------------------------------------

void config_braindlab_cargar_snmp(config_snmp_t *cfg)
{
    static const char *const claves_ip[CONFIG_SNMP_NUM_DESTINOS] = { "trap1_ip", "trap2_ip" };
    static const char *const claves_hab[CONFIG_SNMP_NUM_DESTINOS] = { "trap1_habilitado", "trap2_habilitado" };

    bloquear();
    cJSON *raiz = cargar_json_archivo(RUTA_CONFIG);

    // Sin seccion "snmp" se pasa NULL: cJSON_GetObjectItemCaseSensitive(NULL, ...)
    // devuelve NULL y cada campo cae a su default.
    const cJSON *snmp = cJSON_GetObjectItemCaseSensitive(raiz, "snmp");
    if (!cJSON_IsObject(snmp)) {
        snmp = NULL;
    }
    leer_campo_str(snmp, "community_lectura", cfg->community_lectura, sizeof(cfg->community_lectura),
                   SNMP_COMMUNITY_LECTURA_DEFAULT);
    leer_campo_str(snmp, "community_escritura", cfg->community_escritura, sizeof(cfg->community_escritura),
                   SNMP_COMMUNITY_ESCRITURA_DEFAULT);
    leer_campo_str(snmp, "community_trap", cfg->community_trap, sizeof(cfg->community_trap),
                   SNMP_COMMUNITY_TRAP_DEFAULT);
    for (int i = 0; i < CONFIG_SNMP_NUM_DESTINOS; i++) {
        leer_campo_str(snmp, claves_ip[i], cfg->trap_ip[i], sizeof(cfg->trap_ip[i]), SNMP_TRAP_IP_DEFAULT);
        const cJSON *hab = cJSON_GetObjectItemCaseSensitive(snmp, claves_hab[i]);
        cfg->trap_habilitado[i] = cJSON_IsBool(hab) ? cJSON_IsTrue(hab) : SNMP_TRAP_HABILITADO_DEFAULT;
    }

    cJSON_Delete(raiz);
    desbloquear();
    ESP_LOGI(TAG, "SNMP cargado: trap1=%s (%s) trap2=%s (%s)",
             cfg->trap_ip[0], cfg->trap_habilitado[0] ? "on" : "off",
             cfg->trap_ip[1], cfg->trap_habilitado[1] ? "on" : "off");
}

esp_err_t config_braindlab_guardar_snmp(const config_snmp_t *cfg)
{
    bloquear();
    cJSON *raiz = cargar_json_archivo(RUTA_CONFIG); // conserva las demas secciones

    cJSON_DeleteItemFromObject(raiz, "snmp");
    cJSON *snmp = cJSON_CreateObject();
    cJSON_AddStringToObject(snmp, "community_lectura", cfg->community_lectura);
    cJSON_AddStringToObject(snmp, "community_escritura", cfg->community_escritura);
    cJSON_AddStringToObject(snmp, "community_trap", cfg->community_trap);
    cJSON_AddStringToObject(snmp, "trap1_ip", cfg->trap_ip[0]);
    cJSON_AddBoolToObject(snmp, "trap1_habilitado", cfg->trap_habilitado[0]);
    cJSON_AddStringToObject(snmp, "trap2_ip", cfg->trap_ip[1]);
    cJSON_AddBoolToObject(snmp, "trap2_habilitado", cfg->trap_habilitado[1]);
    cJSON_AddItemToObject(raiz, "snmp", snmp);

    esp_err_t err = guardar_json_archivo(raiz);
    cJSON_Delete(raiz);
    desbloquear();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SNMP guardado en %s", RUTA_CONFIG);
    }
    return err;
}

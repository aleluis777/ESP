// Implementacion -- ver auth_web.h.
//
// Hash: SHA-256 iterado con salt aleatorio de 16 bytes (AUTH_ITERACIONES
// vueltas, ~decenas de ms en el ESP32 con SHA por hardware) -- encarece
// probar contrasenas si alguien llegara a leer la flash. Se usa la API PSA
// (psa/crypto.h) porque ESP-IDF 6 trae mbedTLS 4 / TF-PSA-Crypto; el PSA ya
// lo inicializa ESP-IDF al arrancar.

#include <string.h>
#include <stdio.h>
#include "auth_web.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#include "cJSON.h"

static const char *TAG = "AUTH_WEB";

#define NVS_NAMESPACE      "auth"
#define SALT_LEN           16
#define HASH_LEN           32
#define AUTH_ITERACIONES   2048

#define TOKEN_BYTES        16
#define TOKEN_HEX_LEN      (TOKEN_BYTES * 2)
#define AUTH_MAX_SESIONES  4

// Anti fuerza bruta: tras FALLOS_MAX intentos fallidos seguidos, se
// rechaza todo login durante BLOQUEO_S (aunque la contrasena sea correcta).
#define FALLOS_MAX         5
#define BLOQUEO_S          60

static uint8_t s_salt[SALT_LEN];
static uint8_t s_hash[HASH_LEN];
static bool s_por_defecto = false;
static bool s_listo = false;

typedef struct {
    bool activa;
    char token[TOKEN_HEX_LEN + 1];
    int64_t ultima_actividad_us;
} sesion_t;

static sesion_t s_sesiones[AUTH_MAX_SESIONES];
static int s_fallos = 0;
static int64_t s_bloqueado_hasta_us = 0;

// ------------------------------------------------------------------ hash

static esp_err_t calcular_hash(const char *password, const uint8_t salt[SALT_LEN], uint8_t salida[HASH_LEN])
{
    // Vuelta 0: H(salt || password). Resto: H(anterior || salt).
    uint8_t buf[SALT_LEN + AUTH_PASSWORD_MAX + HASH_LEN];
    size_t pw_len = strlen(password);
    if (pw_len > AUTH_PASSWORD_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buf, salt, SALT_LEN);
    memcpy(buf + SALT_LEN, password, pw_len);

    size_t largo = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, buf, SALT_LEN + pw_len, salida, HASH_LEN, &largo) != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    for (int i = 1; i < AUTH_ITERACIONES; i++) {
        memcpy(buf, salida, HASH_LEN);
        memcpy(buf + HASH_LEN, salt, SALT_LEN);
        if (psa_hash_compute(PSA_ALG_SHA_256, buf, HASH_LEN + SALT_LEN, salida, HASH_LEN, &largo) != PSA_SUCCESS) {
            return ESP_FAIL;
        }
    }
    memset(buf, 0, sizeof(buf)); // no dejar la contrasena en el stack
    return ESP_OK;
}

// Comparacion en tiempo constante (no corta en el primer byte distinto).
static bool iguales(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t dif = 0;
    for (size_t i = 0; i < n; i++) {
        dif |= a[i] ^ b[i];
    }
    return dif == 0;
}

static bool verificar_password(const char *password)
{
    uint8_t calculado[HASH_LEN];
    if (calcular_hash(password, s_salt, calculado) != ESP_OK) {
        return false;
    }
    return iguales(calculado, s_hash, HASH_LEN);
}

// Genera salt nuevo + hash y lo graba en NVS.
static esp_err_t guardar_password(const char *password, bool por_defecto)
{
    uint8_t salt[SALT_LEN], hash[HASH_LEN];
    esp_fill_random(salt, sizeof(salt));
    esp_err_t err = calcular_hash(password, salt, hash);
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t nvs;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs, "salt", salt, sizeof(salt));
    if (err == ESP_OK) err = nvs_set_blob(nvs, "hash", hash, sizeof(hash));
    if (err == ESP_OK) err = nvs_set_u8(nvs, "defecto", por_defecto ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        memcpy(s_salt, salt, sizeof(salt));
        memcpy(s_hash, hash, sizeof(hash));
        s_por_defecto = por_defecto;
    }
    return err;
}

esp_err_t auth_web_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS sin paginas libres o de otra version -- se borra y se reinicializa");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init() fallo (%s)", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t nvs;
    size_t salt_len = sizeof(s_salt), hash_len = sizeof(s_hash);
    uint8_t defecto = 0;
    bool cargado = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        cargado = nvs_get_blob(nvs, "salt", s_salt, &salt_len) == ESP_OK && salt_len == SALT_LEN &&
                  nvs_get_blob(nvs, "hash", s_hash, &hash_len) == ESP_OK && hash_len == HASH_LEN;
        nvs_get_u8(nvs, "defecto", &defecto);
        nvs_close(nvs);
    }

    if (cargado) {
        s_por_defecto = defecto != 0;
        ESP_LOGI(TAG, "Contrasena cargada de NVS%s", s_por_defecto ? " (sigue siendo la de fabrica)" : "");
    } else {
        err = guardar_password(AUTH_PASSWORD_DEFAULT, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo grabar la contrasena de fabrica (%s)", esp_err_to_name(err));
            return err;
        }
        ESP_LOGW(TAG, "Sin contrasena en NVS -- se grabo la de fabrica (usuario \"%s\")", AUTH_USUARIO);
    }
    s_listo = true;
    return ESP_OK;
}

bool auth_web_password_por_defecto(void)
{
    return s_por_defecto;
}

// ------------------------------------------------------------------ sesiones

static sesion_t *buscar_sesion(httpd_req_t *req)
{
    char token[TOKEN_HEX_LEN + 1];
    size_t len = sizeof(token);
    if (httpd_req_get_cookie_val(req, "sesion", token, &len) != ESP_OK || strlen(token) != TOKEN_HEX_LEN) {
        return NULL;
    }

    int64_t ahora = esp_timer_get_time();
    for (int i = 0; i < AUTH_MAX_SESIONES; i++) {
        sesion_t *s = &s_sesiones[i];
        if (!s->activa) {
            continue;
        }
        if (ahora - s->ultima_actividad_us > (int64_t)AUTH_SESION_INACTIVA_S * 1000000LL) {
            s->activa = false; // expirada
            continue;
        }
        if (iguales((const uint8_t *)token, (const uint8_t *)s->token, TOKEN_HEX_LEN)) {
            return s;
        }
    }
    return NULL;
}

bool auth_web_sesion_valida(httpd_req_t *req)
{
    if (!s_listo) {
        // Sin NVS no hay contrasena que verificar -- antes que dejar la web
        // abierta, se niega todo (se ve en el log por que).
        return false;
    }
    sesion_t *s = buscar_sesion(req);
    if (!s) {
        return false;
    }
    s->ultima_actividad_us = esp_timer_get_time();
    return true;
}

bool auth_web_requerir(httpd_req_t *req)
{
    if (auth_web_sesion_valida(req)) {
        return true;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"sesion requerida\"}");
    return false;
}

static sesion_t *crear_sesion(void)
{
    // Slot libre, o si no hay, el de actividad mas vieja.
    sesion_t *elegida = &s_sesiones[0];
    for (int i = 0; i < AUTH_MAX_SESIONES; i++) {
        if (!s_sesiones[i].activa) {
            elegida = &s_sesiones[i];
            break;
        }
        if (s_sesiones[i].ultima_actividad_us < elegida->ultima_actividad_us) {
            elegida = &s_sesiones[i];
        }
    }

    uint8_t aleatorio[TOKEN_BYTES];
    esp_fill_random(aleatorio, sizeof(aleatorio));
    for (int i = 0; i < TOKEN_BYTES; i++) {
        snprintf(&elegida->token[i * 2], 3, "%02x", aleatorio[i]);
    }
    elegida->activa = true;
    elegida->ultima_actividad_us = esp_timer_get_time();
    return elegida;
}

// ------------------------------------------------------------------ handlers

// Lee el body (JSON chico) a buf. false si vino vacio o no entra.
static bool leer_body(httpd_req_t *req, char *buf, size_t tam)
{
    if (req->content_len == 0 || req->content_len >= tam) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body vacio o demasiado largo");
        return false;
    }
    size_t recibidos = 0;
    while (recibidos < req->content_len) {
        int n = httpd_req_recv(req, buf + recibidos, req->content_len - recibidos);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "error leyendo el body");
            return false;
        }
        recibidos += (size_t)n;
    }
    buf[recibidos] = '\0';
    return true;
}

static void responder_json(httpd_req_t *req, const char *estado, const char *json)
{
    if (estado) {
        httpd_resp_set_status(req, estado);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
}

// POST /login -- body {"usuario":"admin","password":"..."}.
static esp_err_t login_handler(httpd_req_t *req)
{
    char body[200];
    if (!leer_body(req, body, sizeof(body))) {
        return ESP_OK;
    }

    int64_t ahora = esp_timer_get_time();
    if (ahora < s_bloqueado_hasta_us) {
        int faltan = (int)((s_bloqueado_hasta_us - ahora) / 1000000LL) + 1;
        char json[96];
        snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"demasiados intentos, espere %d s\"}", faltan);
        responder_json(req, "429 Too Many Requests", json);
        return ESP_OK;
    }

    cJSON *raiz = cJSON_Parse(body);
    const cJSON *usuario = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "usuario") : NULL;
    const cJSON *password = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "password") : NULL;
    bool ok = s_listo && cJSON_IsString(usuario) && cJSON_IsString(password) &&
              strcmp(usuario->valuestring, AUTH_USUARIO) == 0 &&
              verificar_password(password->valuestring);
    cJSON_Delete(raiz);

    if (!ok) {
        if (++s_fallos >= FALLOS_MAX) {
            s_fallos = 0;
            s_bloqueado_hasta_us = ahora + (int64_t)BLOQUEO_S * 1000000LL;
            ESP_LOGW(TAG, "Login: %d intentos fallidos -- bloqueado %d s", FALLOS_MAX, BLOQUEO_S);
        } else {
            ESP_LOGW(TAG, "Login fallido (%d/%d)", s_fallos, FALLOS_MAX);
        }
        responder_json(req, "401 Unauthorized", "{\"ok\":false,\"error\":\"usuario o contrasena incorrectos\"}");
        return ESP_OK;
    }

    s_fallos = 0;
    sesion_t *s = crear_sesion();
    // El string de la cookie tiene que seguir vivo hasta que se envie la
    // respuesta -- static alcanza (una sola tarea HTTP).
    static char cookie[96];
    snprintf(cookie, sizeof(cookie), "sesion=%s; Path=/; HttpOnly; SameSite=Strict", s->token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    ESP_LOGI(TAG, "Login correcto de \"%s\"", AUTH_USUARIO);
    responder_json(req, NULL, s_por_defecto ? "{\"ok\":true,\"por_defecto\":true}" : "{\"ok\":true,\"por_defecto\":false}");
    return ESP_OK;
}

// POST /logout -- cierra la sesion actual (si habia) y borra la cookie.
static esp_err_t logout_handler(httpd_req_t *req)
{
    sesion_t *s = s_listo ? buscar_sesion(req) : NULL;
    if (s) {
        s->activa = false;
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "sesion=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
    responder_json(req, NULL, "{\"ok\":true}");
    return ESP_OK;
}

// POST /cambiar_password -- body {"actual":"...","nueva":"..."}. Requiere
// sesion Y la contrasena actual (por si alguien deja la sesion abierta).
// Cierra todas las demas sesiones.
static esp_err_t cambiar_password_handler(httpd_req_t *req)
{
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }
    char body[200];
    if (!leer_body(req, body, sizeof(body))) {
        return ESP_OK;
    }

    cJSON *raiz = cJSON_Parse(body);
    const cJSON *actual = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "actual") : NULL;
    const cJSON *nueva = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "nueva") : NULL;

    const char *error = NULL;
    if (!cJSON_IsString(actual) || !cJSON_IsString(nueva)) {
        error = "{\"ok\":false,\"error\":\"faltan \\\"actual\\\"/\\\"nueva\\\"\"}";
    } else if (!verificar_password(actual->valuestring)) {
        error = "{\"ok\":false,\"error\":\"la contrasena actual no es correcta\"}";
    } else if (strlen(nueva->valuestring) < AUTH_PASSWORD_MIN || strlen(nueva->valuestring) > AUTH_PASSWORD_MAX) {
        error = "{\"ok\":false,\"error\":\"la nueva contrasena debe tener entre 6 y 64 caracteres\"}";
    } else if (strcmp(nueva->valuestring, AUTH_PASSWORD_DEFAULT) == 0) {
        error = "{\"ok\":false,\"error\":\"la nueva contrasena no puede ser la de fabrica\"}";
    }

    if (error) {
        cJSON_Delete(raiz);
        responder_json(req, "400 Bad Request", error);
        return ESP_OK;
    }

    esp_err_t err = guardar_password(nueva->valuestring, false);
    cJSON_Delete(raiz);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo guardar la contrasena nueva (%s)", esp_err_to_name(err));
        responder_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"no se pudo guardar en NVS\"}");
        return ESP_OK;
    }

    // Cierra las otras sesiones; la actual sigue.
    sesion_t *propia = buscar_sesion(req);
    for (int i = 0; i < AUTH_MAX_SESIONES; i++) {
        if (&s_sesiones[i] != propia) {
            s_sesiones[i].activa = false;
        }
    }
    ESP_LOGI(TAG, "Contrasena cambiada");
    responder_json(req, NULL, "{\"ok\":true}");
    return ESP_OK;
}

// GET /api/sesion -- para que la web sepa si mostrar el aviso de
// contrasena de fabrica.
static esp_err_t sesion_handler(httpd_req_t *req)
{
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }
    char json[80];
    snprintf(json, sizeof(json), "{\"usuario\":\"%s\",\"por_defecto\":%s}",
             AUTH_USUARIO, s_por_defecto ? "true" : "false");
    responder_json(req, NULL, json);
    return ESP_OK;
}

static const httpd_uri_t s_uri_login = {
    .uri = "/login", .method = HTTP_POST, .handler = login_handler,
};
static const httpd_uri_t s_uri_logout = {
    .uri = "/logout", .method = HTTP_POST, .handler = logout_handler,
};
static const httpd_uri_t s_uri_cambiar_password = {
    .uri = "/cambiar_password", .method = HTTP_POST, .handler = cambiar_password_handler,
};
static const httpd_uri_t s_uri_sesion = {
    .uri = "/api/sesion", .method = HTTP_GET, .handler = sesion_handler,
};

void auth_web_registrar_handlers(httpd_handle_t servidor)
{
    httpd_register_uri_handler(servidor, &s_uri_login);
    httpd_register_uri_handler(servidor, &s_uri_logout);
    httpd_register_uri_handler(servidor, &s_uri_cambiar_password);
    httpd_register_uri_handler(servidor, &s_uri_sesion);
}

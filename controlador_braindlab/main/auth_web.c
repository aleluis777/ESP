// Implementacion -- ver auth_web.h.
//
// Hash: SHA-256 iterado con salt aleatorio de 16 bytes (AUTH_ITERACIONES
// vueltas, ~decenas de ms en el ESP32 con SHA por hardware) -- encarece
// probar contrasenas si alguien llegara a leer la flash o el .bin. Se usa
// la API PSA (psa/crypto.h) porque ESP-IDF 6 trae mbedTLS 4 /
// TF-PSA-Crypto; el PSA ya lo inicializa ESP-IDF al arrancar. El mismo
// calculo esta en tools/generar_hash_root.py -- si se cambia uno, cambiar
// el otro.

#include <string.h>
#include <stdio.h>
#include "auth_web.h"
#include "auth_root_fabrica.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#include "cJSON.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "eventos.h"

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

// Credencial de un usuario. Claves NVS: admin = "salt"/"hash"/"defecto"
// (las mismas de la version con un solo usuario, asi no se pierde la
// contrasena ya guardada); root = "r_salt"/"r_hash" (si no existen, rige
// el hash de fabrica de auth_root_fabrica.h).
typedef struct {
    const char *usuario;
    const char *clave_salt;
    const char *clave_hash;
    uint8_t salt[SALT_LEN];
    uint8_t hash[HASH_LEN];
    bool por_defecto; // sigue siendo la de fabrica
} credencial_t;

static credencial_t s_admin = { .usuario = AUTH_USUARIO_ADMIN, .clave_salt = "salt", .clave_hash = "hash" };
static credencial_t s_root = { .usuario = AUTH_USUARIO_ROOT, .clave_salt = "r_salt", .clave_hash = "r_hash" };
static bool s_listo = false;

typedef struct {
    bool activa;
    auth_nivel_t nivel;
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

static bool verificar_password(const credencial_t *c, const char *password)
{
    uint8_t calculado[HASH_LEN];
    if (calcular_hash(password, c->salt, calculado) != ESP_OK) {
        return false;
    }
    return iguales(calculado, c->hash, HASH_LEN);
}

// Genera salt nuevo + hash y lo graba en NVS.
static esp_err_t guardar_password(credencial_t *c, const char *password, bool por_defecto)
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
    err = nvs_set_blob(nvs, c->clave_salt, salt, sizeof(salt));
    if (err == ESP_OK) err = nvs_set_blob(nvs, c->clave_hash, hash, sizeof(hash));
    if (err == ESP_OK && c == &s_admin) err = nvs_set_u8(nvs, "defecto", por_defecto ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err == ESP_OK) {
        memcpy(c->salt, salt, sizeof(salt));
        memcpy(c->hash, hash, sizeof(hash));
        c->por_defecto = por_defecto;
    }
    return err;
}

// true si encontro salt+hash del usuario en NVS.
static bool cargar_de_nvs(nvs_handle_t nvs, credencial_t *c)
{
    size_t salt_len = sizeof(c->salt), hash_len = sizeof(c->hash);
    return nvs_get_blob(nvs, c->clave_salt, c->salt, &salt_len) == ESP_OK && salt_len == SALT_LEN &&
           nvs_get_blob(nvs, c->clave_hash, c->hash, &hash_len) == ESP_OK && hash_len == HASH_LEN;
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

    bool admin_cargado = false, root_cargado = false;
    uint8_t defecto = 0;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        admin_cargado = cargar_de_nvs(nvs, &s_admin);
        root_cargado = cargar_de_nvs(nvs, &s_root);
        nvs_get_u8(nvs, "defecto", &defecto);
        nvs_close(nvs);
    }

    if (admin_cargado) {
        s_admin.por_defecto = defecto != 0;
    } else {
        err = guardar_password(&s_admin, AUTH_PASSWORD_DEFAULT, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "No se pudo grabar la contrasena de fabrica del admin (%s)", esp_err_to_name(err));
            return err;
        }
        ESP_LOGW(TAG, "Sin contrasena de admin en NVS -- se grabo la de fabrica");
    }

    // Root: no se graba la de fabrica en NVS -- se usa directo del firmware
    // hasta que alguien la cambie (asi un firmware nuevo con otro hash de
    // fabrica aplica en los equipos que nunca la cambiaron).
    if (!root_cargado) {
        memcpy(s_root.salt, AUTH_ROOT_FABRICA_SALT, SALT_LEN);
        memcpy(s_root.hash, AUTH_ROOT_FABRICA_HASH, HASH_LEN);
        s_root.por_defecto = true;
    }

    ESP_LOGI(TAG, "Contrasenas cargadas: admin %s, root %s",
             s_admin.por_defecto ? "de fabrica" : "propia", s_root.por_defecto ? "de fabrica" : "propia (NVS)");
    s_listo = true;
    return ESP_OK;
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

auth_nivel_t auth_web_nivel(httpd_req_t *req)
{
    if (!s_listo) {
        // Sin NVS no hay contrasena que verificar -- antes que dejar la web
        // abierta, se niega todo (se ve en el log por que).
        return AUTH_NIVEL_NINGUNO;
    }
    sesion_t *s = buscar_sesion(req);
    if (!s) {
        return AUTH_NIVEL_NINGUNO;
    }
    s->ultima_actividad_us = esp_timer_get_time();
    return s->nivel;
}

bool auth_web_sesion_valida(httpd_req_t *req)
{
    return auth_web_nivel(req) != AUTH_NIVEL_NINGUNO;
}

static void responder_json(httpd_req_t *req, const char *estado, const char *json)
{
    if (estado) {
        httpd_resp_set_status(req, estado);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
}

static bool requerir_nivel(httpd_req_t *req, auth_nivel_t minimo)
{
    auth_nivel_t nivel = auth_web_nivel(req);
    if (nivel >= minimo) {
        return true;
    }
    if (nivel == AUTH_NIVEL_NINGUNO) {
        responder_json(req, "401 Unauthorized", "{\"ok\":false,\"error\":\"sesion requerida\"}");
    } else {
        responder_json(req, "403 Forbidden", "{\"ok\":false,\"error\":\"requiere usuario root\"}");
    }
    return false;
}

bool auth_web_requerir(httpd_req_t *req)
{
    return requerir_nivel(req, AUTH_NIVEL_ADMIN);
}

bool auth_web_requerir_root(httpd_req_t *req)
{
    return requerir_nivel(req, AUTH_NIVEL_ROOT);
}

static sesion_t *crear_sesion(auth_nivel_t nivel)
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
    elegida->nivel = nivel;
    elegida->ultima_actividad_us = esp_timer_get_time();
    return elegida;
}

// Cierra las sesiones de un nivel (al cambiarle la contrasena), menos
// 'excepto' (la de quien hizo el cambio).
static void cerrar_sesiones(auth_nivel_t nivel, const sesion_t *excepto)
{
    for (int i = 0; i < AUTH_MAX_SESIONES; i++) {
        if (&s_sesiones[i] != excepto && s_sesiones[i].nivel == nivel) {
            s_sesiones[i].activa = false;
        }
    }
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

// IP del cliente (para el registro de eventos de seguridad).
static void ip_cliente(httpd_req_t *req, char *ip, size_t largo)
{
    struct sockaddr_in6 origen;
    socklen_t origen_len = sizeof(origen);
    strlcpy(ip, "?", largo);
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&origen, &origen_len) != 0) {
        return;
    }
    if (IN6_IS_ADDR_V4MAPPED(&origen.sin6_addr)) {
        struct in_addr v4 = { .s_addr = origen.sin6_addr.un.u32_addr[3] };
        inet_ntop(AF_INET, &v4, ip, largo);
    } else {
        inet_ntop(AF_INET6, &origen.sin6_addr, ip, largo);
    }
}

// POST /login -- body {"usuario":"admin"|"root","password":"..."}.
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

    const credencial_t *c = NULL;
    auth_nivel_t nivel = AUTH_NIVEL_NINGUNO;
    if (cJSON_IsString(usuario)) {
        if (strcmp(usuario->valuestring, AUTH_USUARIO_ADMIN) == 0) {
            c = &s_admin;
            nivel = AUTH_NIVEL_ADMIN;
        } else if (strcmp(usuario->valuestring, AUTH_USUARIO_ROOT) == 0) {
            c = &s_root;
            nivel = AUTH_NIVEL_ROOT;
        }
    }
    bool ok = s_listo && c && cJSON_IsString(password) && verificar_password(c, password->valuestring);
    // Usuario tal como lo escribieron (solo para el registro): recortado y
    // sin caracteres raros.
    char usuario_txt[17] = "?";
    if (cJSON_IsString(usuario)) {
        strlcpy(usuario_txt, usuario->valuestring, sizeof(usuario_txt));
        for (char *p = usuario_txt; *p; p++) {
            if (*p < ' ' || *p > '~') *p = '?';
        }
    }
    cJSON_Delete(raiz);
    char ip[46];
    ip_cliente(req, ip, sizeof(ip));

    if (!ok) {
        if (++s_fallos >= FALLOS_MAX) {
            s_fallos = 0;
            s_bloqueado_hasta_us = ahora + (int64_t)BLOQUEO_S * 1000000LL;
            ESP_LOGW(TAG, "Login: %d intentos fallidos -- bloqueado %d s", FALLOS_MAX, BLOQUEO_S);
            eventos_registrar(EVENTO_AVISO, EVENTO_SEGURIDAD, "web",
                              "Login bloqueado %d s tras %d intentos fallidos (ultimo: usuario %s desde %s)",
                              BLOQUEO_S, FALLOS_MAX, usuario_txt, ip);
        } else {
            ESP_LOGW(TAG, "Login fallido (%d/%d)", s_fallos, FALLOS_MAX);
            eventos_registrar(EVENTO_AVISO, EVENTO_SEGURIDAD, "web", "Inicio de sesion fallido: usuario %s desde %s",
                              usuario_txt, ip);
        }
        responder_json(req, "401 Unauthorized", "{\"ok\":false,\"error\":\"usuario o contrasena incorrectos\"}");
        return ESP_OK;
    }

    s_fallos = 0;
    sesion_t *s = crear_sesion(nivel);
    // El string de la cookie tiene que seguir vivo hasta que se envie la
    // respuesta -- static alcanza (una sola tarea HTTP).
    static char cookie[96];
    snprintf(cookie, sizeof(cookie), "sesion=%s; Path=/; HttpOnly; SameSite=Strict", s->token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    ESP_LOGI(TAG, "Login correcto de \"%s\"", c->usuario);
    eventos_registrar(EVENTO_INFO, EVENTO_SEGURIDAD, "web", "Inicio de sesion: %s desde %s", c->usuario, ip);

    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":true,\"nivel\":\"%s\",\"admin_por_defecto\":%s}",
             c->usuario, s_admin.por_defecto ? "true" : "false");
    responder_json(req, NULL, json);
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

// POST /cambiar_password -- SOLO root. Body:
//   {"usuario":"admin","nueva":"..."}                   -> resetea la del cliente
//   {"usuario":"root","actual":"...","nueva":"..."}     -> cambia la maestra
// La maestra pide la actual por si alguien deja la sesion root abierta.
// Cierra las sesiones abiertas del usuario cambiado (menos la propia).
static esp_err_t cambiar_password_handler(httpd_req_t *req)
{
    if (!auth_web_requerir_root(req)) {
        return ESP_OK;
    }
    char body[256];
    if (!leer_body(req, body, sizeof(body))) {
        return ESP_OK;
    }

    cJSON *raiz = cJSON_Parse(body);
    const cJSON *usuario = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "usuario") : NULL;
    const cJSON *actual = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "actual") : NULL;
    const cJSON *nueva = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "nueva") : NULL;

    credencial_t *c = NULL;
    if (cJSON_IsString(usuario)) {
        if (strcmp(usuario->valuestring, AUTH_USUARIO_ADMIN) == 0) c = &s_admin;
        else if (strcmp(usuario->valuestring, AUTH_USUARIO_ROOT) == 0) c = &s_root;
    }
    size_t minimo = (c == &s_root) ? AUTH_PASSWORD_ROOT_MIN : AUTH_PASSWORD_MIN;

    const char *error = NULL;
    if (!c || !cJSON_IsString(nueva)) {
        error = "{\"ok\":false,\"error\":\"faltan \\\"usuario\\\" (admin/root) o \\\"nueva\\\"\"}";
    } else if (c == &s_root && (!cJSON_IsString(actual) || !verificar_password(&s_root, actual->valuestring))) {
        error = "{\"ok\":false,\"error\":\"la contrasena maestra actual no es correcta\"}";
    } else if (strlen(nueva->valuestring) < minimo || strlen(nueva->valuestring) > AUTH_PASSWORD_MAX) {
        error = (c == &s_root)
            ? "{\"ok\":false,\"error\":\"la maestra debe tener entre 12 y 64 caracteres\"}"
            : "{\"ok\":false,\"error\":\"la contrasena debe tener entre 6 y 64 caracteres\"}";
    } else if (c == &s_admin && strcmp(nueva->valuestring, AUTH_PASSWORD_DEFAULT) == 0) {
        error = "{\"ok\":false,\"error\":\"no puede ser la de fabrica\"}";
    }

    if (error) {
        cJSON_Delete(raiz);
        responder_json(req, "400 Bad Request", error);
        return ESP_OK;
    }

    esp_err_t err = guardar_password(c, nueva->valuestring, false);
    cJSON_Delete(raiz);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo guardar la contrasena de \"%s\" (%s)", c->usuario, esp_err_to_name(err));
        responder_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"no se pudo guardar en NVS\"}");
        return ESP_OK;
    }

    cerrar_sesiones(c == &s_root ? AUTH_NIVEL_ROOT : AUTH_NIVEL_ADMIN, buscar_sesion(req));
    ESP_LOGW(TAG, "Contrasena de \"%s\" cambiada (por root)", c->usuario);
    eventos_registrar(EVENTO_AVISO, EVENTO_SEGURIDAD, "web:root", "Contrasena de %s cambiada", c->usuario);
    responder_json(req, NULL, "{\"ok\":true}");
    return ESP_OK;
}

// GET /api/sesion -- quien esta logueado y si las contrasenas siguen
// siendo las de fabrica (la web muestra avisos solo a root, que es quien
// puede cambiarlas).
static esp_err_t sesion_handler(httpd_req_t *req)
{
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }
    bool es_root = auth_web_nivel(req) == AUTH_NIVEL_ROOT;
    char json[128];
    snprintf(json, sizeof(json), "{\"usuario\":\"%s\",\"admin_por_defecto\":%s,\"root_por_defecto\":%s}",
             es_root ? AUTH_USUARIO_ROOT : AUTH_USUARIO_ADMIN,
             s_admin.por_defecto ? "true" : "false",
             s_root.por_defecto ? "true" : "false");
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

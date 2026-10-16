// Servidor HTTP (esp_http_server): sirve el sitio estatico (index.html,
// style.css, script.js -- ver carpeta ../web) desde la particion SPIFFS
// "www", y tiene el WebSocket en /ws que empuja el estado de climatizacion
// en vivo. Mismo patron que servidor_web.c de controlador_labgeo, recortado
// a lo que hace falta ac's (sin calibracion/OTA/subida de archivos todavia).
//
// El sitio se graba en flash al compilar (spiffs_create_partition_image en
// main/CMakeLists.txt), en la particion "www" (separada de "storage",
// reservada para historial -- ver partitions.csv).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "registro_sd.h"
#include "eventos.h"
#include "snmp_braindlab.h"
#include "servidor_web.h"
#include "auth_web.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "cJSON.h"

static const char *TAG = "SERVIDOR_WEB";

#define WS_MAX_CLIENTES 4
// Sockets totales del servidor (HTTP + WS). El maximo permitido es 13 (16-3).
#define HTTPD_MAX_SOCKETS (WS_MAX_CLIENTES + 8)

static httpd_handle_t s_servidor = NULL;
static servidor_web_callbacks_t s_callbacks;

// Se llama antes de servidor_web_init() para que app_main.c decida que
// hacer cuando llega un POST de configuracion -- este archivo no sabe nada
// de config_braindlab.h, solo avisa que llego el pedido. Mismo patron que
// servidor_web_set_callbacks() en controlador_labgeo.
void servidor_web_set_callbacks(servidor_web_callbacks_t callbacks)
{
    s_callbacks = callbacks;
}

// Monta la particion SPIFFS "www" (la que arma spiffs_create_partition_image
// a partir de ../web). Idempotente: config_braindlab_init() ya la monta
// antes (necesita el config.json ahi) -- si para cuando arranca el servidor
// web ya esta montada, no hace nada.
static esp_err_t montar_www(void)
{
    if (esp_spiffs_mounted("www")) {
        return ESP_OK;
    }

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/www",
        .partition_label = "www",
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo montar la particion 'www' (%s)", esp_err_to_name(err));
    }
    return err;
}

// Loguea cada peticion HTTP que entra (metodo, URI, IP del cliente) -- mismo
// criterio que controlador_labgeo.
static void log_peticion(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_in6 origen;
    socklen_t origen_len = sizeof(origen);
    char ip[46] = "?";

    if (getpeername(fd, (struct sockaddr *)&origen, &origen_len) == 0) {
        if (IN6_IS_ADDR_V4MAPPED(&origen.sin6_addr)) {
            struct in_addr v4 = { .s_addr = origen.sin6_addr.un.u32_addr[3] };
            inet_ntop(AF_INET, &v4, ip, sizeof(ip));
        } else {
            inet_ntop(AF_INET6, &origen.sin6_addr, ip, sizeof(ip));
        }
    }

    ESP_LOGI(TAG, "%s %s  <- %s (fd=%d)", http_method_str(req->method), req->uri, ip, fd);
}

// Origen de un evento hecho desde la web: "web:admin" o "web:root".
static const char *origen_web(httpd_req_t *req)
{
    return auth_web_nivel(req) == AUTH_NIVEL_ROOT ? "web:root" : "web:admin";
}

typedef struct {
    const char *ruta_spiffs;
    const char *content_type;
    // Nivel minimo para verlo: NINGUNO = publico (login.html y lo que
    // necesita), ADMIN = cualquier sesion, ROOT = solo el fabricante.
    auth_nivel_t nivel;
} archivo_estatico_t;

// Sirve un archivo de /www en pedazos (httpd_resp_send_chunk), sin cargarlo
// entero en RAM.
static esp_err_t archivo_estatico_handler(httpd_req_t *req)
{
    log_peticion(req);

    const archivo_estatico_t *info = (const archivo_estatico_t *)req->user_ctx;

    // Pagina protegida sin sesion -> al login (redirect, no 401: es el
    // navegador pidiendo una pagina, no un fetch). Con sesion de admin en
    // una pagina de root -> 403, no al login (ya esta logueado).
    if (info->nivel != AUTH_NIVEL_NINGUNO) {
        auth_nivel_t nivel = auth_web_nivel(req);
        if (nivel == AUTH_NIVEL_NINGUNO) {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/login.html");
            httpd_resp_send(req, NULL, 0);
            return ESP_OK;
        }
        if (nivel < info->nivel) {
            httpd_resp_set_status(req, "403 Forbidden");
            httpd_resp_set_type(req, "text/html");
            httpd_resp_sendstr(req, "<!DOCTYPE html><meta charset=\"UTF-8\"><title>Acceso denegado</title>"
                                    "<p style=\"font-family:sans-serif\">Acceso denegado: esta pagina es solo "
                                    "para el usuario root. <a href=\"/\">Volver al panel</a></p>");
            return ESP_OK;
        }
    }

    FILE *f = fopen(info->ruta_spiffs, "r");
    if (!f) {
        ESP_LOGW(TAG, "No se encontro %s (se compilo con 'idf.py flash' despues de agregar web/?)",
                 info->ruta_spiffs);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, info->content_type);
    // CSS/JS/logo: el navegador los reusa 5 min sin volver a pedirlos (menos
    // conexiones al ESP32 al pasar de una pagina a otra). Un archivo nuevo
    // subido por /upload se ve a los 5 min como mucho, o ya con Ctrl+F5.
    // HTML: siempre se pide de nuevo (pasa por el chequeo de sesion).
    bool es_html = strcmp(info->content_type, "text/html") == 0;
    httpd_resp_set_hdr(req, "Cache-Control", es_html ? "no-cache" : "max-age=300");

    char buf[512];
    size_t leidos;
    while ((leidos = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, leidos) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static const archivo_estatico_t s_archivo_index       = { "/www/index.html", "text/html", AUTH_NIVEL_ADMIN };
static const archivo_estatico_t s_archivo_style       = { "/www/style.css", "text/css", AUTH_NIVEL_NINGUNO };
static const archivo_estatico_t s_archivo_script      = { "/www/script.js", "application/javascript", AUTH_NIVEL_ADMIN };
static const archivo_estatico_t s_archivo_ota_html     = { "/www/ota.html", "text/html", AUTH_NIVEL_ROOT };
static const archivo_estatico_t s_archivo_configurar_html = { "/www/configurar.html", "text/html", AUTH_NIVEL_ADMIN };
static const archivo_estatico_t s_archivo_logo         = { "/www/logo.png", "image/png", AUTH_NIVEL_NINGUNO };
static const archivo_estatico_t s_archivo_files_html   = { "/www/files.html", "text/html", AUTH_NIVEL_ROOT };
static const archivo_estatico_t s_archivo_historico_html = { "/www/historico.html", "text/html", AUTH_NIVEL_ADMIN };
static const archivo_estatico_t s_archivo_eventos_html = { "/www/eventos.html", "text/html", AUTH_NIVEL_ADMIN };
static const archivo_estatico_t s_archivo_login_html   = { "/www/login.html", "text/html", AUTH_NIVEL_NINGUNO };
static const archivo_estatico_t s_archivo_comun_js     = { "/www/comun.js", "application/javascript", AUTH_NIVEL_NINGUNO };
static const archivo_estatico_t s_archivo_root_html    = { "/www/root.html", "text/html", AUTH_NIVEL_ROOT };

// "/" y "/index.html" apuntan al mismo archivo -- asi entrar directo a la IP
// del controlador ya muestra la pagina.
static const httpd_uri_t s_uri_raiz = {
    .uri = "/", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_index,
};
static const httpd_uri_t s_uri_index = {
    .uri = "/index.html", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_index,
};
static const httpd_uri_t s_uri_style = {
    .uri = "/style.css", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_style,
};
static const httpd_uri_t s_uri_script = {
    .uri = "/script.js", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_script,
};
static const httpd_uri_t s_uri_ota_html = {
    .uri = "/ota.html", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_ota_html,
};
static const httpd_uri_t s_uri_configurar_html = {
    .uri = "/configurar.html", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_configurar_html,
};
static const httpd_uri_t s_uri_logo = {
    .uri = "/logo.png", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_logo,
};
static const httpd_uri_t s_uri_historico_html = {
    .uri = "/historico.html", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_historico_html,
};
static const httpd_uri_t s_uri_eventos_html = {
    .uri = "/eventos.html", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_eventos_html,
};
static const httpd_uri_t s_uri_files_html = {
    .uri = "/files.html", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_files_html,
};
static const httpd_uri_t s_uri_login_html = {
    .uri = "/login.html", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_login_html,
};
static const httpd_uri_t s_uri_comun_js = {
    .uri = "/comun.js", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_comun_js,
};
static const httpd_uri_t s_uri_root_html = {
    .uri = "/root.html", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_root_html,
};

// Lista lo que hay grabado en la particion SPIFFS "www" -- mismo formato
// que controlador_labgeo: {"total_bytes":N,"usados_bytes":N,"archivos":
// [{"nombre":"index.html","bytes":1234},...]}
static esp_err_t listado_www_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir_root(req)) {
        return ESP_OK;
    }

    size_t total = 0, usados = 0;
    esp_spiffs_info("www", &total, &usados);

    httpd_resp_set_type(req, "application/json");

    char cabecera[96];
    snprintf(cabecera, sizeof(cabecera), "{\"total_bytes\":%u,\"usados_bytes\":%u,\"archivos\":[",
             (unsigned)total, (unsigned)usados);
    httpd_resp_sendstr_chunk(req, cabecera);

    DIR *dir = opendir("/www");
    if (!dir) {
        ESP_LOGW(TAG, "No se pudo abrir /www (particion 'www' sin montar?)");
        httpd_resp_sendstr_chunk(req, "]}");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }

    bool primero = true;
    struct dirent *entrada;
    char linea[300];
    while ((entrada = readdir(dir)) != NULL) {
        char ruta[280];
        snprintf(ruta, sizeof(ruta), "/www/%s", entrada->d_name);

        struct stat st;
        long bytes = (stat(ruta, &st) == 0) ? (long)st.st_size : -1;

        snprintf(linea, sizeof(linea), "%s{\"nombre\":\"%s\",\"bytes\":%ld}",
                 primero ? "" : ",", entrada->d_name, bytes);
        httpd_resp_sendstr_chunk(req, linea);
        primero = false;
    }
    closedir(dir);

    httpd_resp_sendstr_chunk(req, "]}");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static const httpd_uri_t s_uri_listado = {
    .uri = "/files", .method = HTTP_GET,
    .handler = listado_www_handler,
};

// Nombre de archivo aceptado para /upload y /descargar: sin '/' (SPIFFS
// "www" es plano, sin subcarpetas) y sin ".." (no se puede salir de /www).
static bool nombre_archivo_valido(const char *nombre)
{
    return nombre[0] != '\0' && strchr(nombre, '/') == NULL && strstr(nombre, "..") == NULL;
}

// Solo para /descargar -- las rutas fijas de arriba ya mandan su
// Content-Type correcto a mano; esto es nada mas para lo que se sube por
// /upload y no tiene ruta fija propia.
static const char *content_type_por_extension(const char *nombre)
{
    const char *punto = strrchr(nombre, '.');
    if (!punto) {
        return "application/octet-stream";
    }
    if (strcmp(punto, ".html") == 0) return "text/html";
    if (strcmp(punto, ".css") == 0) return "text/css";
    if (strcmp(punto, ".js") == 0) return "application/javascript";
    if (strcmp(punto, ".json") == 0) return "application/json";
    if (strcmp(punto, ".csv") == 0) return "text/csv";
    if (strcmp(punto, ".png") == 0) return "image/png";
    if (strcmp(punto, ".jpg") == 0 || strcmp(punto, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(punto, ".txt") == 0) return "text/plain";
    return "application/octet-stream";
}

// POST /upload -- sube (o sobreescribe) un archivo en /www. El nombre viene
// en la cabecera "X-Filename" (no en el body, para no tener que parsear
// multipart/form-data), el contenido crudo es el body entero, escrito a
// disco en pedazos igual que archivo_estatico_handler pero al reves.
#define UPLOAD_BUF_LEN 512

static esp_err_t upload_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir_root(req)) {
        return ESP_OK;
    }

    char nombre[64];
    size_t hdr_len = httpd_req_get_hdr_value_len(req, "X-Filename");
    if (hdr_len == 0 || hdr_len >= sizeof(nombre) ||
        httpd_req_get_hdr_value_str(req, "X-Filename", nombre, sizeof(nombre)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "falta o es invalida la cabecera \"X-Filename\"");
        return ESP_FAIL;
    }
    if (!nombre_archivo_valido(nombre)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nombre de archivo invalido (sin '/' ni '..')");
        return ESP_FAIL;
    }

    char ruta[80];
    snprintf(ruta, sizeof(ruta), "/www/%s", nombre);

    FILE *f = fopen(ruta, "wb");
    if (!f) {
        ESP_LOGE(TAG, "upload: no se pudo crear %s", ruta);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no se pudo crear el archivo en /www");
        return ESP_FAIL;
    }

    char buf[UPLOAD_BUF_LEN];
    size_t restantes = req->content_len;
    bool error = false;
    while (restantes > 0) {
        size_t a_leer = restantes < sizeof(buf) ? restantes : sizeof(buf);
        int leidos = httpd_req_recv(req, buf, a_leer);
        if (leidos == HTTPD_SOCK_ERR_TIMEOUT) {
            continue; // reintenta -- mismo criterio que el ejemplo de esp_http_server
        }
        if (leidos <= 0 || fwrite(buf, 1, leidos, f) != (size_t)leidos) {
            error = true;
            break;
        }
        restantes -= (size_t)leidos;
    }
    fclose(f);

    if (error) {
        ESP_LOGE(TAG, "upload: fallo recibiendo/escribiendo %s (particion 'www' llena?)", ruta);
        remove(ruta); // no dejar un archivo a medio escribir
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "error recibiendo el archivo");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "upload: %s guardado (%u bytes)", ruta, (unsigned)req->content_len);

    char resp[128];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"nombre\":\"%s\",\"bytes\":%u}",
             nombre, (unsigned)req->content_len);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static const httpd_uri_t s_uri_upload = {
    .uri = "/upload", .method = HTTP_POST, .handler = upload_handler,
};

// config.json para quien no es root: la community de escritura de SNMP
// (permite apagar aires si el SET esta habilitado) se reemplaza por
// "********". La web nunca la necesita: el campo se deja vacio a proposito.
// Si el archivo no se puede leer o parsear no se manda nada (nunca crudo).
static esp_err_t enviar_config_sin_secretos(httpd_req_t *req, const char *ruta)
{
    FILE *f = fopen(ruta, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }
    const size_t max = 4096;
    char *buf = malloc(max);
    if (!buf) {
        fclose(f);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sin memoria");
        return ESP_FAIL;
    }
    size_t leidos = fread(buf, 1, max - 1, f);
    fclose(f);
    buf[leidos] = '\0';
    cJSON *raiz = cJSON_Parse(buf);
    free(buf);
    if (!raiz) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "config.json invalido");
        return ESP_FAIL;
    }

    cJSON *snmp = cJSON_GetObjectItemCaseSensitive(raiz, "snmp");
    if (cJSON_IsObject(snmp) && cJSON_HasObjectItem(snmp, "community_escritura")) {
        cJSON_ReplaceItemInObjectCaseSensitive(snmp, "community_escritura", cJSON_CreateString("********"));
    }
    char *texto = cJSON_PrintUnformatted(raiz);
    cJSON_Delete(raiz);
    if (!texto) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sin memoria");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, texto);
    cJSON_free(texto);
    return err;
}

// GET /descargar?nombre=xxx -- para ver/bajar un archivo de /www que no
// tenga ya su propia ruta fija arriba. Las rutas fijas siguen siendo la
// forma normal de servir index.html/style.css/etc, esto es solo el
// complemento para lo demas (por ejemplo, algo subido por /upload).
static esp_err_t descargar_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }

    char query[96];
    char nombre[64];
    if (httpd_req_get_url_query_len(req) == 0 ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "nombre", nombre, sizeof(nombre)) != ESP_OK ||
        !nombre_archivo_valido(nombre)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "falta \"nombre\" valido en la query");
        return ESP_FAIL;
    }

    char ruta[80];
    snprintf(ruta, sizeof(ruta), "/www/%s", nombre);

    // config.json (y su .tmp) solo sale completo para root (respaldo desde
    // files.html); al resto se le entrega sin la community de escritura.
    if (strncmp(nombre, "config.json", 11) == 0 && auth_web_nivel(req) != AUTH_NIVEL_ROOT) {
        return enviar_config_sin_secretos(req, ruta);
    }

    FILE *f = fopen(ruta, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, content_type_por_extension(nombre));

    char buf[512];
    size_t leidos;
    while ((leidos = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, leidos) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t s_uri_descargar = {
    .uri = "/descargar", .method = HTTP_GET, .handler = descargar_handler,
};

// DELETE /descargar?nombre=xxx -- borra un archivo de /www (mismo endpoint
// y misma validacion de nombre que el GET, nada mas cambia el metodo).
static esp_err_t eliminar_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir_root(req)) {
        return ESP_OK;
    }

    char query[96];
    char nombre[64];
    if (httpd_req_get_url_query_len(req) == 0 ||
        httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "nombre", nombre, sizeof(nombre)) != ESP_OK ||
        !nombre_archivo_valido(nombre)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "falta \"nombre\" valido en la query");
        return ESP_FAIL;
    }

    char ruta[80];
    snprintf(ruta, sizeof(ruta), "/www/%s", nombre);

    if (remove(ruta) != 0) {
        ESP_LOGW(TAG, "eliminar: no se pudo borrar %s (no existe?)", ruta);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "eliminar: %s borrado", ruta);

    char resp[96];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"nombre\":\"%s\"}", nombre);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static const httpd_uri_t s_uri_eliminar = {
    .uri = "/descargar", .method = HTTP_DELETE, .handler = eliminar_handler,
};

// POST /configurar_red -- body JSON {"ip":"...","gateway":"...","mascara":"..."}.
// Por ahora solo guarda en config.json (via el callback de app_main.c);
// aplicarlo de verdad al W5500 sigue pendiente (red_eth.c usa su IP fija),
// mismo estado que controlador_labgeo.
static esp_err_t configurar_red_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }

    char body[160];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body vacio o invalido");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *raiz = cJSON_Parse(body);
    const cJSON *ip_item   = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "ip") : NULL;
    const cJSON *gw_item   = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "gateway") : NULL;
    const cJSON *mask_item = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "mascara") : NULL;

    if (!cJSON_IsString(ip_item) || !cJSON_IsString(gw_item) || !cJSON_IsString(mask_item)) {
        cJSON_Delete(raiz);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "faltan \"ip\"/\"gateway\"/\"mascara\" (strings) en el body");
        return ESP_FAIL;
    }

    if (s_callbacks.on_configurar_red) {
        s_callbacks.on_configurar_red(ip_item->valuestring, gw_item->valuestring, mask_item->valuestring);
    }
    eventos_registrar(EVENTO_INFO, EVENTO_CONFIG, origen_web(req),
                      "Red guardada: IP %.15s gateway %.15s mascara %.15s (se aplica al reiniciar)",
                      ip_item->valuestring, gw_item->valuestring, mask_item->valuestring);
    cJSON_Delete(raiz);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_configurar_red = {
    .uri = "/configurar_red", .method = HTTP_POST, .handler = configurar_red_handler,
};

// POST /configurar_climatizacion -- body JSON con los mismos campos que
// config_climatizacion_t (ver config_braindlab.h): {"cantidad_aires":N,
// "temp_min":N,"temp_max":N,"temp_at":N,"temp_bypass":N,
// "fails_max_bypass":N,"rotar_reserva":bool}. Equivalente de
// /configurar_equipo en controlador_labgeo, adaptado a los parametros de
// climatizacion en vez de los de la celda de carga.
static esp_err_t configurar_climatizacion_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }

    char body[256];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body vacio o invalido");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *raiz = cJSON_Parse(body);
    const cJSON *cantidad_item = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "cantidad_aires") : NULL;
    const cJSON *tmin_item     = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "temp_min") : NULL;
    const cJSON *tmax_item     = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "temp_max") : NULL;
    const cJSON *tat_item      = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "temp_at") : NULL;
    const cJSON *tbypass_item  = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "temp_bypass") : NULL;
    const cJSON *fails_item    = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "fails_max_bypass") : NULL;
    const cJSON *rotar_item    = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "rotar_reserva") : NULL;
    const cJSON *alternar_item = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "aire_alternar") : NULL; // opcional

    if (!cJSON_IsNumber(cantidad_item) || !cJSON_IsNumber(tmin_item) || !cJSON_IsNumber(tmax_item) ||
        !cJSON_IsNumber(tat_item) || !cJSON_IsNumber(tbypass_item) || !cJSON_IsNumber(fails_item) ||
        !cJSON_IsBool(rotar_item)) {
        cJSON_Delete(raiz);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "faltan campos numericos (cantidad_aires/temp_min/temp_max/temp_at/temp_bypass/"
                             "fails_max_bypass) o \"rotar_reserva\" (bool) en el body");
        return ESP_FAIL;
    }

    if (s_callbacks.on_configurar_climatizacion) {
        s_callbacks.on_configurar_climatizacion((uint8_t)cantidad_item->valuedouble,
                                                 (float)tmin_item->valuedouble, (float)tmax_item->valuedouble,
                                                 (float)tat_item->valuedouble, (float)tbypass_item->valuedouble,
                                                 (uint8_t)fails_item->valuedouble, cJSON_IsTrue(rotar_item),
                                                 cJSON_IsNumber(alternar_item) ? (uint8_t)alternar_item->valuedouble : 0);
    }
    eventos_registrar(EVENTO_INFO, EVENTO_CONFIG, origen_web(req),
                      "Climatizacion guardada: %u aires; min %.1f max %.1f AT %.1f bypass %.1f C; rotacion %s "
                      "(se aplica al reiniciar)",
                      (unsigned)cantidad_item->valuedouble, tmin_item->valuedouble, tmax_item->valuedouble,
                      tat_item->valuedouble, tbypass_item->valuedouble, cJSON_IsTrue(rotar_item) ? "si" : "no");
    cJSON_Delete(raiz);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_configurar_climatizacion = {
    .uri = "/configurar_climatizacion", .method = HTTP_POST, .handler = configurar_climatizacion_handler,
};

// POST /configurar_snmp -- body JSON {"community_lectura","community_escritura",
// "community_trap","trap1_ip","trap1_habilitado","trap2_ip","trap2_habilitado",
// "nombre","ubicacion","contacto","escritura_habilitada"}. community_escritura
// vacia = no cambiar.
// Destinos y textos se aplican en caliente; las communities al reiniciar
// (respuesta {"ok":true,"reiniciar":true}).

// Community: 1..31 caracteres ASCII visibles, sin espacios.
static bool community_valida(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || len > 31) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (s[i] <= ' ' || s[i] > '~') {
            return false;
        }
    }
    return true;
}

// Copia un string opcional del JSON; false si es demasiado largo.
static bool copiar_texto_json(const cJSON *raiz, const char *clave, char *destino, size_t len)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(raiz, clave);
    if (!cJSON_IsString(item)) {
        return true; // ausente: se deja el valor actual
    }
    if (strlen(item->valuestring) >= len) {
        return false;
    }
    strcpy(destino, item->valuestring);
    return true;
}

static esp_err_t configurar_snmp_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }

    char body[512];
    if (req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body demasiado grande");
        return ESP_FAIL;
    }
    int recibidos = 0;
    while (recibidos < (int)req->content_len) {
        int len = httpd_req_recv(req, body + recibidos, req->content_len - recibidos);
        if (len <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body vacio o invalido");
            return ESP_FAIL;
        }
        recibidos += len;
    }
    body[recibidos] = '\0';

    cJSON *raiz = cJSON_Parse(body);
    if (!cJSON_IsObject(raiz)) {
        cJSON_Delete(raiz);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON invalido");
        return ESP_FAIL;
    }

    config_snmp_t cfg;
    config_braindlab_cargar_snmp(&cfg); // base: lo vigente (campos ausentes no cambian)
    const char *error = NULL;

    static const char *const claves_ip[CONFIG_SNMP_NUM_DESTINOS] = { "trap1_ip", "trap2_ip" };
    static const char *const claves_hab[CONFIG_SNMP_NUM_DESTINOS] = { "trap1_habilitado", "trap2_habilitado" };
    for (int i = 0; i < CONFIG_SNMP_NUM_DESTINOS && !error; i++) {
        const cJSON *ip = cJSON_GetObjectItemCaseSensitive(raiz, claves_ip[i]);
        const cJSON *hab = cJSON_GetObjectItemCaseSensitive(raiz, claves_hab[i]);
        if (cJSON_IsString(ip)) {
            struct in_addr a;
            const char *texto = ip->valuestring[0] ? ip->valuestring : "0.0.0.0";
            if (inet_aton(texto, &a) == 0) {
                error = "IP de gestor invalida";
                break;
            }
            inet_ntoa_r(a, cfg.trap_ip[i], sizeof(cfg.trap_ip[i]));
        }
        if (cJSON_IsBool(hab)) {
            cfg.trap_habilitado[i] = cJSON_IsTrue(hab);
        }
        if (cfg.trap_habilitado[i] && strcmp(cfg.trap_ip[i], "0.0.0.0") == 0) {
            error = "un gestor habilitado necesita IP";
        }
    }

    const cJSON *permitir_set = cJSON_GetObjectItemCaseSensitive(raiz, "escritura_habilitada");
    if (cJSON_IsBool(permitir_set)) {
        cfg.escritura_habilitada = cJSON_IsTrue(permitir_set);
    }

    if (!error) {
        const cJSON *escritura = cJSON_GetObjectItemCaseSensitive(raiz, "community_escritura");
        bool escritura_nueva = cJSON_IsString(escritura) && escritura->valuestring[0];
        if (!copiar_texto_json(raiz, "community_lectura", cfg.community_lectura, sizeof(cfg.community_lectura)) ||
            !copiar_texto_json(raiz, "community_trap", cfg.community_trap, sizeof(cfg.community_trap)) ||
            (escritura_nueva && !copiar_texto_json(raiz, "community_escritura", cfg.community_escritura,
                                                   sizeof(cfg.community_escritura)))) {
            error = "community demasiado larga (max. 31)";
        } else if (!community_valida(cfg.community_lectura) || !community_valida(cfg.community_escritura) ||
                   !community_valida(cfg.community_trap)) {
            error = "community invalida (1-31 caracteres, sin espacios)";
        } else if (strcmp(cfg.community_lectura, cfg.community_escritura) == 0) {
            error = "la community de escritura no puede ser igual a la de lectura";
        } else if (!copiar_texto_json(raiz, "nombre", cfg.nombre, sizeof(cfg.nombre)) ||
                   !copiar_texto_json(raiz, "ubicacion", cfg.ubicacion, sizeof(cfg.ubicacion)) ||
                   !copiar_texto_json(raiz, "contacto", cfg.contacto, sizeof(cfg.contacto))) {
            error = "nombre (max. 31), ubicacion o contacto (max. 47) demasiado largo";
        }
    }
    cJSON_Delete(raiz);

    if (error) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
        return ESP_FAIL;
    }

    bool reiniciar = false;
    if (snmp_braindlab_configurar(&cfg, &reiniciar) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no se pudo guardar config.json");
        return ESP_FAIL;
    }
    eventos_registrar(EVENTO_INFO, EVENTO_CONFIG, origen_web(req), "Configuracion SNMP guardada (control por SET: %s)%s",
                      cfg.escritura_habilitada ? "habilitado" : "solo lectura",
                      reiniciar ? "; communities nuevas al reiniciar" : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, reiniciar ? "{\"ok\":true,\"reiniciar\":true}" : "{\"ok\":true,\"reiniciar\":false}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_configurar_snmp = {
    .uri = "/configurar_snmp", .method = HTTP_POST, .handler = configurar_snmp_handler,
};

// Nombre del sitio: 1..31 caracteres ASCII visibles (SNMP DisplayString no
// admite tildes ni ñ). Vacio = 'por_defecto'. NULL si es invalido.
static const char *nombre_sitio(const cJSON *item, const char *por_defecto, const char **error)
{
    if (!cJSON_IsString(item)) {
        *error = "falta un nombre (string)";
        return NULL;
    }
    const char *nombre = item->valuestring[0] ? item->valuestring : por_defecto;
    size_t largo = strlen(nombre);
    if (largo > 31) {
        *error = "nombre demasiado largo (max. 31)";
        return NULL;
    }
    for (size_t j = 0; j < largo; j++) {
        if (nombre[j] < ' ' || nombre[j] > '~') {
            *error = "nombre invalido: sin tildes ni enie (SNMP solo admite ASCII)";
            return NULL;
        }
    }
    return nombre;
}

// POST /configurar_sitio -- body JSON {"equipo":"...","aires":["..." x 4],
// "sensores":[{"nombre":"...","instalado":bool} x 6]}. "equipo" es el
// sysName de SNMP y el titulo del panel.
static esp_err_t configurar_sitio_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }

    char body[1024];
    if (req->content_len >= sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body demasiado grande");
        return ESP_FAIL;
    }
    int recibidos = 0;
    while (recibidos < (int)req->content_len) {
        int len = httpd_req_recv(req, body + recibidos, req->content_len - recibidos);
        if (len <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body vacio o invalido");
            return ESP_FAIL;
        }
        recibidos += len;
    }
    body[recibidos] = '\0';

    cJSON *raiz = cJSON_Parse(body);
    const cJSON *lista = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "sensores") : NULL;
    const cJSON *aires = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "aires") : NULL;
    if (!cJSON_IsArray(lista) || cJSON_GetArraySize(lista) != 6 ||
        !cJSON_IsArray(aires) || cJSON_GetArraySize(aires) != 4) {
        cJSON_Delete(raiz);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "faltan \"sensores\" (6) o \"aires\" (4)");
        return ESP_FAIL;
    }

    static const char *const sensores_defecto[6] = { "T. Operacion 1", "T. Operacion 2", "T. Inyeccion 1",
                                                      "T. Inyeccion 2", "Gestor", "Sensor 6" };
    static const char *const aires_defecto[4] = { "AA1", "AA2", "AA3", "AA4" };
    const char *error = NULL;
    const char *equipo = nombre_sitio(cJSON_GetObjectItemCaseSensitive(raiz, "equipo"), "controlador_braindlab",
                                      &error);
    const char *nombres_aires[4];
    for (int i = 0; i < 4 && !error; i++) {
        nombres_aires[i] = nombre_sitio(cJSON_GetArrayItem(aires, i), aires_defecto[i], &error);
    }
    const char *nombres[6];
    bool instalado[6];
    for (int i = 0; i < 6 && !error; i++) {
        const cJSON *item = cJSON_GetArrayItem(lista, i);
        const cJSON *inst = cJSON_GetObjectItemCaseSensitive(item, "instalado");
        nombres[i] = nombre_sitio(cJSON_GetObjectItemCaseSensitive(item, "nombre"), sensores_defecto[i], &error);
        if (!error && !cJSON_IsBool(inst)) {
            error = "cada sensor necesita \"instalado\" (bool)";
        }
        instalado[i] = cJSON_IsTrue(inst);
    }

    esp_err_t err = ESP_FAIL;
    if (!error && s_callbacks.on_configurar_sitio) {
        err = s_callbacks.on_configurar_sitio(equipo, nombres_aires, nombres, instalado);
    }
    cJSON_Delete(raiz); // despues del callback: los nombres apuntan adentro del JSON

    if (error) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, error);
        return ESP_FAIL;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no se pudo guardar config.json");
        return ESP_FAIL;
    }
    eventos_registrar(EVENTO_INFO, EVENTO_CONFIG, origen_web(req), "Sitio guardado: nombres y sensores instalados");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_configurar_sitio = {
    .uri = "/configurar_sitio", .method = HTTP_POST, .handler = configurar_sitio_handler,
};

// POST /snmp_prueba -- encola el trap de prueba hacia los gestores habilitados.
static esp_err_t snmp_prueba_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }
    esp_err_t err = snmp_braindlab_trap_prueba();
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"mensaje\":\"no hay ningun gestor habilitado (guardar primero)\"}");
    } else {
        httpd_resp_sendstr(req, "{\"ok\":false,\"mensaje\":\"agente SNMP sin arrancar o sin cable de red\"}");
    }
    return ESP_OK;
}

static const httpd_uri_t s_uri_snmp_prueba = {
    .uri = "/snmp_prueba", .method = HTTP_POST, .handler = snmp_prueba_handler,
};

// POST /control_aire -- body JSON {"indice":0-3,"encendido":bool}. Fuerza a
// mano la salida AA(indice+1) hasta que la automatica cruce a otro ciclo
// (ver comentario de servidor_web_cb_control_aire_t).
static esp_err_t control_aire_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }

    char body[96];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body vacio o invalido");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *raiz = cJSON_Parse(body);
    const cJSON *indice_item     = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "indice") : NULL;
    const cJSON *encendido_item  = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "encendido") : NULL;

    if (!cJSON_IsNumber(indice_item) || !cJSON_IsBool(encendido_item) ||
        indice_item->valuedouble < 0 || indice_item->valuedouble > 3) {
        cJSON_Delete(raiz);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "falta \"indice\" (0-3) o \"encendido\" (bool) en el body");
        return ESP_FAIL;
    }

    if (s_callbacks.on_control_aire) {
        s_callbacks.on_control_aire((uint8_t)indice_item->valuedouble, cJSON_IsTrue(encendido_item));
    }
    eventos_manual_aire(origen_web(req), (uint8_t)indice_item->valuedouble, cJSON_IsTrue(encendido_item));
    cJSON_Delete(raiz);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_control_aire = {
    .uri = "/control_aire", .method = HTTP_POST, .handler = control_aire_handler,
};

// POST /control_bypass -- body JSON {"solicitado":bool}. Fuerza a mano
// bypass_solicitado (BP_S) hasta que la automatica cruce a otro ciclo.
static esp_err_t control_bypass_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }

    char body[64];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body vacio o invalido");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *raiz = cJSON_Parse(body);
    const cJSON *solicitado_item = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "solicitado") : NULL;

    if (!cJSON_IsBool(solicitado_item)) {
        cJSON_Delete(raiz);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "falta \"solicitado\" (bool) en el body");
        return ESP_FAIL;
    }

    if (s_callbacks.on_control_bypass) {
        s_callbacks.on_control_bypass(cJSON_IsTrue(solicitado_item));
    }
    eventos_manual_bypass(origen_web(req), cJSON_IsTrue(solicitado_item));
    cJSON_Delete(raiz);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_control_bypass = {
    .uri = "/control_bypass", .method = HTTP_POST, .handler = control_bypass_handler,
};

// POST /configurar_rtc -- body JSON {"fecha_hora":"YYYY-MM-DDTHH:MM[:SS]"},
// el formato que manda tal cual un <input type="datetime-local"> de HTML.
// Se parsea a mano con sscanf en vez de traer una libreria de fechas --
// formato fijo, no hace falta mas.
static esp_err_t configurar_rtc_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }

    char body[64];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body vacio o invalido");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *raiz = cJSON_Parse(body);
    const cJSON *fecha_hora_item = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "fecha_hora") : NULL;

    if (!cJSON_IsString(fecha_hora_item)) {
        cJSON_Delete(raiz);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "falta \"fecha_hora\" (string) en el body");
        return ESP_FAIL;
    }

    int anio, mes, dia, hora, minuto, segundo = 0;
    int leidos = sscanf(fecha_hora_item->valuestring, "%d-%d-%dT%d:%d:%d",
                         &anio, &mes, &dia, &hora, &minuto, &segundo);

    if (leidos < 5 || anio < 2000 || anio > 2099 || mes < 1 || mes > 12 || dia < 1 || dia > 31 ||
        hora < 0 || hora > 23 || minuto < 0 || minuto > 59 || segundo < 0 || segundo > 59) {
        cJSON_Delete(raiz);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "\"fecha_hora\" invalida (esperado YYYY-MM-DDTHH:MM[:SS])");
        return ESP_FAIL;
    }

    if (s_callbacks.on_configurar_rtc) {
        s_callbacks.on_configurar_rtc((uint16_t)anio, (uint8_t)mes, (uint8_t)dia,
                                       (uint8_t)hora, (uint8_t)minuto, (uint8_t)segundo);
    }
    eventos_registrar(EVENTO_INFO, EVENTO_CONFIG, origen_web(req), "Hora del equipo ajustada a %04d-%02d-%02d %02d:%02d",
                      anio, mes, dia, hora, minuto);
    cJSON_Delete(raiz);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_configurar_rtc = {
    .uri = "/configurar_rtc", .method = HTTP_POST, .handler = configurar_rtc_handler,
};

// POST /control_at -- body JSON {"activa":bool}. Fuerza a mano OUT_AT hasta
// que la automatica cruce a otro ciclo.
static esp_err_t control_at_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }

    char body[64];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body vacio o invalido");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *raiz = cJSON_Parse(body);
    const cJSON *activa_item = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "activa") : NULL;

    if (!cJSON_IsBool(activa_item)) {
        cJSON_Delete(raiz);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "falta \"activa\" (bool) en el body");
        return ESP_FAIL;
    }

    if (s_callbacks.on_control_at) {
        s_callbacks.on_control_at(cJSON_IsTrue(activa_item));
    }
    eventos_manual_at(origen_web(req), cJSON_IsTrue(activa_item));
    cJSON_Delete(raiz);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_control_at = {
    .uri = "/control_at", .method = HTTP_POST, .handler = control_at_handler,
};

// POST /control_automatico -- sin body. Cancela cualquier override manual
// de AA1-4/bypass, la automatica retoma el control de todo.
static esp_err_t control_automatico_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }

    if (s_callbacks.on_control_automatico) {
        s_callbacks.on_control_automatico();
    }
    eventos_manual_automatico(origen_web(req));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_control_automatico = {
    .uri = "/control_automatico", .method = HTTP_POST, .handler = control_automatico_handler,
};

// POST /calibrar_sensor -- body JSON {"sensor":"t1|t2|t3|t4|temp_gestor|
// humedad_gestor","valor_referencia":N}. Ver servidor_web_cb_calibrar_sensor_t.
static esp_err_t calibrar_sensor_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }

    char body[96];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body vacio o invalido");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *raiz = cJSON_Parse(body);
    const cJSON *sensor_item     = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "sensor") : NULL;
    const cJSON *referencia_item = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "valor_referencia") : NULL;

    if (!cJSON_IsString(sensor_item) || !cJSON_IsNumber(referencia_item)) {
        cJSON_Delete(raiz);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                             "faltan \"sensor\" (string) o \"valor_referencia\" (numero) en el body");
        return ESP_FAIL;
    }

    if (s_callbacks.on_calibrar_sensor) {
        s_callbacks.on_calibrar_sensor(sensor_item->valuestring, (float)referencia_item->valuedouble);
    }
    eventos_registrar(EVENTO_INFO, EVENTO_CONFIG, origen_web(req), "Calibracion de %.15s: referencia %.2f",
                      sensor_item->valuestring, referencia_item->valuedouble);
    cJSON_Delete(raiz);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_calibrar_sensor = {
    .uri = "/calibrar_sensor", .method = HTTP_POST, .handler = calibrar_sensor_handler,
};

// POST /sd_formatear -- body JSON {"confirmacion":"FORMATEAR"}. Formatea la
// MicroSD (borra todo el historico). La palabra de confirmacion se valida
// ACA tambien, no solo en el navegador: asi un POST suelto (un script, un
// curl equivocado) no borra nada. Bloquea este task del servidor mientras
// formatea (unos segundos) -- es una operacion rara y explicita.
static esp_err_t sd_formatear_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }

    char body[64];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body vacio o invalido");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *raiz = cJSON_Parse(body);
    const cJSON *conf = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "confirmacion") : NULL;
    bool confirmado = cJSON_IsString(conf) && strcmp(conf->valuestring, "FORMATEAR") == 0;
    cJSON_Delete(raiz);
    if (!confirmado) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "falta la confirmacion: {\"confirmacion\":\"FORMATEAR\"}");
        return ESP_FAIL;
    }
    if (!s_callbacks.on_formatear_sd) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "formateo no disponible");
        return ESP_FAIL;
    }

    esp_err_t err = s_callbacks.on_formatear_sd();
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        eventos_registrar(EVENTO_AVISO, EVENTO_CONFIG, origen_web(req),
                          "MicroSD formateada: se borro el historico y los eventos anteriores");
        httpd_resp_sendstr(req, "{\"ok\":true,\"mensaje\":\"SD formateada\"}");
        return ESP_OK;
    }
    const char *motivo =
        err == ESP_ERR_NOT_FOUND     ? "no hay tarjeta SD (o no responde)" :
        err == ESP_ERR_INVALID_STATE ? "ya hay un formateo en curso" :
        err == ESP_ERR_TIMEOUT       ? "el formateo tarda demasiado, revisar el estado en unos segundos" :
                                       "fallo el formateo (tarjeta dañada o protegida?)";
    char resp[160];
    snprintf(resp, sizeof(resp), "{\"ok\":false,\"mensaje\":\"%s\"}", motivo);
    httpd_resp_set_status(req, err == ESP_ERR_NOT_FOUND ? "404 Not Found" : "500 Internal Server Error");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static const httpd_uri_t s_uri_sd_formatear = {
    .uri = "/sd_formatear", .method = HTTP_POST, .handler = sd_formatear_handler,
};

// ---------- Historico (MicroSD, ver registro_sd.h) ----------

// Lee un parametro entero de la query (?mes=2026-10 -> se parsea aparte).
static bool leer_query(httpd_req_t *req, const char *clave, char *valor, size_t largo)
{
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, clave, valor, largo) == ESP_OK;
}

// GET /api/historico/dias?mes=2026-10 -- dias con datos de ese mes, para
// el calendario de historico.html:
// {"mes":"2026-10","sd_ok":true,"dias":[{"dia":1,"kb":170},{"dia":2,"kb":85}]}
// sd_ok=false si la SD no esta montada (la pagina avisa en vez de mostrar
// un mes vacio que parezca "no hay datos").
static esp_err_t historico_dias_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }

    char mes_txt[12];
    int anio = 0, mes = 0;
    if (!leer_query(req, "mes", mes_txt, sizeof(mes_txt)) || sscanf(mes_txt, "%d-%d", &anio, &mes) != 2) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "falta ?mes=AAAA-MM");
        return ESP_FAIL;
    }

    uint8_t dias[31];
    uint32_t kb[31];
    int n = registro_sd_dias_con_datos(anio, mes, dias, kb);

    // 31 dias x ~20 bytes + cabecera, entra de sobra.
    char resp[768];
    int pos = snprintf(resp, sizeof(resp), "{\"mes\":\"%04d-%02d\",\"sd_ok\":%s,\"dias\":[",
                       anio, mes, n >= 0 ? "true" : "false");
    for (int i = 0; i < n; i++) {
        pos += snprintf(resp + pos, sizeof(resp) - pos, "%s{\"dia\":%u,\"kb\":%lu}",
                        i ? "," : "", dias[i], (unsigned long)kb[i]);
    }
    snprintf(resp + pos, sizeof(resp) - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static const httpd_uri_t s_uri_historico_dias = {
    .uri = "/api/historico/dias", .method = HTTP_GET, .handler = historico_dias_handler,
};

// GET /api/historico/dia?fecha=2026-10-01 -- el CSV de ese dia tal cual
// esta en la SD, en pedazos (no se carga entero en RAM, ~170 KB). El
// navegador lo parsea y grafica: el ESP32 solo copia bytes. Con
// &descargar=1 el navegador lo guarda como archivo en vez de mostrarlo.
static esp_err_t historico_dia_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }

    char fecha[16];
    int anio = 0, mes = 0, dia = 0;
    if (!leer_query(req, "fecha", fecha, sizeof(fecha)) || sscanf(fecha, "%d-%d-%d", &anio, &mes, &dia) != 3) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "falta ?fecha=AAAA-MM-DD");
        return ESP_FAIL;
    }

    char ruta[48];
    if (!registro_sd_ruta_dia(anio, mes, dia, ruta, sizeof(ruta))) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "SD no disponible o fecha invalida");
        return ESP_FAIL;
    }
    FILE *f = fopen(ruta, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no hay datos de ese dia");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/csv");
    char descargar[4];
    if (leer_query(req, "descargar", descargar, sizeof(descargar)) && descargar[0] == '1') {
        char disp[64];
        snprintf(disp, sizeof(disp), "attachment; filename=\"braindlab_%04d-%02d-%02d.csv\"", anio, mes, dia);
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
    }

    char buf[1024];
    size_t leidos;
    esp_err_t res = ESP_OK;
    while ((leidos = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, leidos) != ESP_OK) {
            res = ESP_FAIL;
            break;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return res;
}

static const httpd_uri_t s_uri_historico_dia = {
    .uri = "/api/historico/dia", .method = HTTP_GET, .handler = historico_dia_handler,
};

// ---------- Registro de eventos (MicroSD, ver eventos.h) ----------

// GET /api/eventos/meses -- meses con eventos, del mas nuevo al mas viejo:
// {"sd_ok":true,"meses":[{"mes":"2026-10","kb":3},{"mes":"2026-09","kb":5}]}
static esp_err_t eventos_meses_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }
    // Hasta 36 meses (3 anios) en la lista: alcanza de sobra para la vista.
    static char meses[36][8]; // static: no carga la pila del task HTTP
    static uint32_t kb[36];
    int n = eventos_meses_con_datos(meses, kb, 36);

    char resp[1300]; // 36 x ~30 bytes + cabecera
    int pos = snprintf(resp, sizeof(resp), "{\"sd_ok\":%s,\"meses\":[", n >= 0 ? "true" : "false");
    for (int i = 0; i < n; i++) {
        pos += snprintf(resp + pos, sizeof(resp) - pos, "%s{\"mes\":\"%s\",\"kb\":%lu}",
                        i ? "," : "", meses[i], (unsigned long)kb[i]);
    }
    snprintf(resp + pos, sizeof(resp) - pos, "]}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    return ESP_OK;
}

static const httpd_uri_t s_uri_eventos_meses = {
    .uri = "/api/eventos/meses", .method = HTTP_GET, .handler = eventos_meses_handler,
};

// GET /api/eventos?mes=2026-10 -- el CSV del mes tal cual esta en la SD, en
// pedazos (fecha_hora,nivel,categoria,origen,detalle). Con &descargar=1 el
// navegador lo guarda como archivo.
static esp_err_t eventos_mes_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }
    char mes_txt[12];
    int anio = 0, mes = 0;
    if (!leer_query(req, "mes", mes_txt, sizeof(mes_txt)) || sscanf(mes_txt, "%d-%d", &anio, &mes) != 2) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "falta ?mes=AAAA-MM");
        return ESP_FAIL;
    }
    char ruta[48];
    if (!eventos_ruta_mes(anio, mes, ruta, sizeof(ruta))) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "SD no disponible o mes invalido");
        return ESP_FAIL;
    }
    FILE *f = fopen(ruta, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no hay eventos de ese mes");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/csv");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    char descargar[4];
    if (leer_query(req, "descargar", descargar, sizeof(descargar)) && descargar[0] == '1') {
        char disp[64];
        snprintf(disp, sizeof(disp), "attachment; filename=\"eventos_%04d-%02d.csv\"", anio, mes);
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
    }

    char buf[1024];
    size_t leidos;
    esp_err_t res = ESP_OK;
    while ((leidos = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, leidos) != ESP_OK) {
            res = ESP_FAIL;
            break;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return res;
}

static const httpd_uri_t s_uri_eventos_mes = {
    .uri = "/api/eventos", .method = HTTP_GET, .handler = eventos_mes_handler,
};

// POST /reiniciar -- sin body. Reinicia el equipo ya (esp_restart()), para
// que tome efecto cualquier config que solo se aplica al arrancar (red,
// climatizacion, etc -- ver /configurar_red, /configurar_climatizacion). No
// necesita callback a app_main.c, es infraestructura pura -- mismo
// esp_restart() que ya usa /ota al final de un firmware aplicado con exito.
static esp_err_t reiniciar_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir(req)) {
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"mensaje\":\"reiniciando\"}");
    eventos_motivo_reinicio(auth_web_nivel(req) == AUTH_NIVEL_ROOT ? "pedido desde la web (root)"
                                                                   : "pedido desde la web (admin)");

    vTaskDelay(pdMS_TO_TICKS(500)); // le da tiempo a la respuesta de salir antes de reiniciar, mismo criterio que /ota
    esp_restart();
    return ESP_OK; // no se llega aca
}

static const httpd_uri_t s_uri_reiniciar = {
    .uri = "/reiniciar", .method = HTTP_POST, .handler = reiniciar_handler,
};

// POST /ota -- sube un firmware nuevo (el .bin que genera "idf.py build") y
// lo escribe en la particion OTA que NO esta corriendo ahora (ota_0/ota_1,
// ver partitions.csv), streameado en pedazos con esp_ota_write(). Si
// esp_ota_end() valida bien la imagen (checksum/firma), la marca para
// bootear y reinicia -- si el firmware nuevo no llega a confirmarse con
// esp_ota_mark_app_valid_cancel_rollback() (ver app_main.c, se llama al
// final de app_main() si todo arranco bien), el bootloader vuelve solo al
// firmware anterior en el proximo reset (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y,
// ver sdkconfig.defaults). Mismo patron que /ota en controlador_labgeo.
#define OTA_BUF_LEN 1024

static esp_err_t ota_handler(httpd_req_t *req)
{
    log_peticion(req);
    if (!auth_web_requerir_root(req)) {
        return ESP_OK;
    }

    if (req->content_len == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body vacio -- mandar el .bin como body crudo");
        return ESP_FAIL;
    }

    const esp_partition_t *particion_destino = esp_ota_get_next_update_partition(NULL);
    if (!particion_destino) {
        ESP_LOGE(TAG, "ota: no se encontro particion OTA destino (revisar partitions.csv)");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no hay particion OTA disponible");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(particion_destino, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota: esp_ota_begin() fallo (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no se pudo empezar el OTA");
        return ESP_FAIL;
    }

    char buf[OTA_BUF_LEN];
    size_t restantes = req->content_len;
    bool error = false;
    while (restantes > 0) {
        size_t a_leer = restantes < sizeof(buf) ? restantes : sizeof(buf);
        int leidos = httpd_req_recv(req, buf, a_leer);
        if (leidos == HTTPD_SOCK_ERR_TIMEOUT) {
            continue; // reintenta -- mismo criterio que en controlador_labgeo
        }
        if (leidos <= 0 || esp_ota_write(ota_handle, buf, leidos) != ESP_OK) {
            error = true;
            break;
        }
        restantes -= (size_t)leidos;
    }

    if (error) {
        ESP_LOGE(TAG, "ota: fallo recibiendo/escribiendo el firmware");
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "error recibiendo el firmware");
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        // Imagen invalida (checksum/firma mal, o no llego el header
        // completo) -- esp_ota_end() ya se aseguro de no dejar nada
        // aplicado, el firmware que esta corriendo ahora no se toco.
        ESP_LOGE(TAG, "ota: esp_ota_end() rechazo la imagen (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "imagen de firmware invalida, no se aplico nada");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(particion_destino);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota: esp_ota_set_boot_partition() fallo (%s)", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no se pudo marcar la particion para bootear");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ota: firmware nuevo OK en '%s' (%u bytes) -- reiniciando",
             particion_destino->label, (unsigned)req->content_len);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"mensaje\":\"firmware aplicado, reiniciando\"}");
    eventos_motivo_reinicio("actualizacion de firmware (OTA) desde la web");

    vTaskDelay(pdMS_TO_TICKS(500)); // le da tiempo a la respuesta de salir antes de reiniciar
    esp_restart();
    return ESP_OK; // no se llega aca
}

static const httpd_uri_t s_uri_ota = {
    .uri = "/ota", .method = HTTP_POST, .handler = ota_handler,
};

// Handshake / frames entrantes del WebSocket. El canal es solo
// controlador -> navegador, asi que alcanza con aceptar el handshake y
// descartar cualquier frame que llegue.
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        log_peticion(req);
        ESP_LOGI(TAG, "Cliente WS conectado (fd=%d)", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    httpd_ws_recv_frame(req, &frame, 0);
    return ESP_OK;
}

// Antes del handshake: sin sesion no se abre el WebSocket (si no,
// cualquiera en la red veria el estado en vivo sin loguearse). Necesita
// CONFIG_HTTPD_WS_PRE_HANDSHAKE_CB_SUPPORT=y (sdkconfig.defaults).
static esp_err_t ws_pre_handshake(httpd_req_t *req)
{
    if (!auth_web_sesion_valida(req)) {
        ESP_LOGW(TAG, "WS rechazado: sin sesion (fd=%d)", httpd_req_to_sockfd(req));
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_send(req, NULL, 0);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const httpd_uri_t s_uri_ws = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = ws_handler,
    .is_websocket = true,
    .ws_pre_handshake_cb = ws_pre_handshake,
};

esp_err_t servidor_web_init(void)
{
    // Si el sitio no monta (por ejemplo, nunca se flasheo la imagen de
    // ../web), seguimos igual: el WebSocket funciona sin el sitio estatico,
    // solo que "/" va a devolver 404.
    montar_www();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = HTTPD_MAX_SOCKETS; // 12 total, margen para la pagina de archivos cargando varios recursos
    config.lru_purge_enable = true;
    // Keep-alive TCP: detecta y libera sockets de pestanas cerradas sin
    // aviso (WS que no mandaron el close, PC que se desconecto) en ~15 s,
    // en vez de dejarlos ocupando lugar hasta que el LRU los purgue.
    config.keep_alive_enable = true;
    config.keep_alive_idle = 5;
    config.keep_alive_interval = 5;
    config.keep_alive_count = 3;
    // servidor_web_enviar_ws() corre en tarea_climatizacion: un cliente
    // que no recibe no debe trabarla los 5 s del default por cada envio.
    config.send_wait_timeout = 2;
    config.max_uri_handlers = 44; // 40 rutas registradas (33 + eventos.html y sus 2 API + 4 de auth_web.c), con margen
    // 6144: los handlers arman respuestas/eventos con snprintf en la pila.
    config.stack_size = 6144;

    esp_err_t err = httpd_start(&s_servidor, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar el servidor HTTP (%s)", esp_err_to_name(err));
        return err;
    }

    httpd_register_uri_handler(s_servidor, &s_uri_raiz);
    httpd_register_uri_handler(s_servidor, &s_uri_index);
    httpd_register_uri_handler(s_servidor, &s_uri_style);
    httpd_register_uri_handler(s_servidor, &s_uri_script);
    httpd_register_uri_handler(s_servidor, &s_uri_ota_html);
    httpd_register_uri_handler(s_servidor, &s_uri_configurar_html);
    httpd_register_uri_handler(s_servidor, &s_uri_logo);
    httpd_register_uri_handler(s_servidor, &s_uri_files_html);
    httpd_register_uri_handler(s_servidor, &s_uri_listado);
    httpd_register_uri_handler(s_servidor, &s_uri_upload);
    httpd_register_uri_handler(s_servidor, &s_uri_descargar);
    httpd_register_uri_handler(s_servidor, &s_uri_eliminar);
    httpd_register_uri_handler(s_servidor, &s_uri_configurar_red);
    httpd_register_uri_handler(s_servidor, &s_uri_configurar_climatizacion);
    httpd_register_uri_handler(s_servidor, &s_uri_configurar_snmp);
    httpd_register_uri_handler(s_servidor, &s_uri_snmp_prueba);
    httpd_register_uri_handler(s_servidor, &s_uri_configurar_sitio);
    httpd_register_uri_handler(s_servidor, &s_uri_control_aire);
    httpd_register_uri_handler(s_servidor, &s_uri_control_bypass);
    httpd_register_uri_handler(s_servidor, &s_uri_control_at);
    httpd_register_uri_handler(s_servidor, &s_uri_configurar_rtc);
    httpd_register_uri_handler(s_servidor, &s_uri_control_automatico);
    httpd_register_uri_handler(s_servidor, &s_uri_calibrar_sensor);
    httpd_register_uri_handler(s_servidor, &s_uri_reiniciar);
    httpd_register_uri_handler(s_servidor, &s_uri_sd_formatear);
    httpd_register_uri_handler(s_servidor, &s_uri_historico_html);
    httpd_register_uri_handler(s_servidor, &s_uri_historico_dias);
    httpd_register_uri_handler(s_servidor, &s_uri_historico_dia);
    httpd_register_uri_handler(s_servidor, &s_uri_eventos_html);
    httpd_register_uri_handler(s_servidor, &s_uri_eventos_meses);
    httpd_register_uri_handler(s_servidor, &s_uri_eventos_mes);
    httpd_register_uri_handler(s_servidor, &s_uri_ota);
    httpd_register_uri_handler(s_servidor, &s_uri_ws);
    httpd_register_uri_handler(s_servidor, &s_uri_login_html);
    httpd_register_uri_handler(s_servidor, &s_uri_comun_js);
    httpd_register_uri_handler(s_servidor, &s_uri_root_html);
    auth_web_registrar_handlers(s_servidor);

    ESP_LOGI(TAG, "Servidor HTTP + WS listo (puerto %d)", config.server_port);
    return ESP_OK;
}

// Recorre todos los sockets abiertos del servidor, se queda solo con los que
// son WebSockets activos, y les manda el mismo frame de texto a cada uno.
// httpd_ws_send_frame_async() se puede llamar desde cualquier tarea, por eso
// tarea_climatizacion (app_main.c) puede invocar esta funcion directamente.
void servidor_web_enviar_ws(const char *json)
{
    if (!s_servidor) {
        return; // el servidor no llego a arrancar (por ejemplo, sin Ethernet)
    }

    // OJO: httpd_get_client_list() devuelve TODOS los sockets abiertos
    // (HTTP + WS), y falla si hay mas que el tamanio del arreglo -- antes
    // era de WS_MAX_CLIENTES (4) y con 5+ conexiones abiertas (el navegador
    // abre varias en paralelo al cargar la pagina) no se enviaba nada a
    // nadie. Tiene que ser del tamanio de max_open_sockets.
    size_t fds = HTTPD_MAX_SOCKETS;
    int client_fds[HTTPD_MAX_SOCKETS];
    if (httpd_get_client_list(s_servidor, &fds, client_fds) != ESP_OK) {
        ESP_LOGW(TAG, "WS: no se pudo obtener la lista de clientes");
        return;
    }

    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = strlen(json),
    };

    for (size_t i = 0; i < fds; i++) {
        int fd = client_fds[i];
        if (httpd_ws_get_fd_info(s_servidor, fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
            if (httpd_ws_send_frame_async(s_servidor, fd, &frame) != ESP_OK) {
                // Cliente muerto (pestana cerrada sin aviso): se cierra para
                // liberar el socket en vez de reintentarle cada 2 s.
                ESP_LOGW(TAG, "WS: fallo el envio a fd=%d, cerrando la sesion", fd);
                httpd_sess_trigger_close(s_servidor, fd);
            }
        }
    }
}

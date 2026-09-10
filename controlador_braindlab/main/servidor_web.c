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
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "servidor_web.h"
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

typedef struct {
    const char *ruta_spiffs;
    const char *content_type;
} archivo_estatico_t;

// Sirve un archivo de /www en pedazos (httpd_resp_send_chunk), sin cargarlo
// entero en RAM.
static esp_err_t archivo_estatico_handler(httpd_req_t *req)
{
    log_peticion(req);

    const archivo_estatico_t *info = (const archivo_estatico_t *)req->user_ctx;

    FILE *f = fopen(info->ruta_spiffs, "r");
    if (!f) {
        ESP_LOGW(TAG, "No se encontro %s (se compilo con 'idf.py flash' despues de agregar web/?)",
                 info->ruta_spiffs);
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, info->content_type);

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

static const archivo_estatico_t s_archivo_index       = { "/www/index.html", "text/html" };
static const archivo_estatico_t s_archivo_style       = { "/www/style.css", "text/css" };
static const archivo_estatico_t s_archivo_script      = { "/www/script.js", "application/javascript" };
static const archivo_estatico_t s_archivo_ota_html     = { "/www/ota.html", "text/html" };
static const archivo_estatico_t s_archivo_configurar_html = { "/www/configurar.html", "text/html" };
static const archivo_estatico_t s_archivo_logo         = { "/www/logo.png", "image/png" };
static const archivo_estatico_t s_archivo_files_html   = { "/www/files.html", "text/html" };

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
static const httpd_uri_t s_uri_files_html = {
    .uri = "/files.html", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_files_html,
};

// Lista lo que hay grabado en la particion SPIFFS "www" -- mismo formato
// que controlador_labgeo: {"total_bytes":N,"usados_bytes":N,"archivos":
// [{"nombre":"index.html","bytes":1234},...]}
static esp_err_t listado_www_handler(httpd_req_t *req)
{
    log_peticion(req);

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

// GET /descargar?nombre=xxx -- para ver/bajar un archivo de /www que no
// tenga ya su propia ruta fija arriba. Las rutas fijas siguen siendo la
// forma normal de servir index.html/style.css/etc, esto es solo el
// complemento para lo demas (por ejemplo, algo subido por /upload).
static esp_err_t descargar_handler(httpd_req_t *req)
{
    log_peticion(req);

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
                                                 (uint8_t)fails_item->valuedouble, cJSON_IsTrue(rotar_item));
    }
    cJSON_Delete(raiz);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_configurar_climatizacion = {
    .uri = "/configurar_climatizacion", .method = HTTP_POST, .handler = configurar_climatizacion_handler,
};

// POST /control_aire -- body JSON {"indice":0-3,"encendido":bool}. Fuerza a
// mano la salida AA(indice+1) hasta que la automatica cruce a otro ciclo
// (ver comentario de servidor_web_cb_control_aire_t).
static esp_err_t control_aire_handler(httpd_req_t *req)
{
    log_peticion(req);

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

    if (s_callbacks.on_control_automatico) {
        s_callbacks.on_control_automatico();
    }

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
    cJSON_Delete(raiz);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_calibrar_sensor = {
    .uri = "/calibrar_sensor", .method = HTTP_POST, .handler = calibrar_sensor_handler,
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

static const httpd_uri_t s_uri_ws = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = ws_handler,
    .is_websocket = true,
};

esp_err_t servidor_web_init(void)
{
    // Si el sitio no monta (por ejemplo, nunca se flasheo la imagen de
    // ../web), seguimos igual: el WebSocket funciona sin el sitio estatico,
    // solo que "/" va a devolver 404.
    montar_www();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = WS_MAX_CLIENTES + 8; // 12 total -- max permitido es 13 (16-3), margen para la pagina de archivos cargando varios recursos
    config.lru_purge_enable = true;
    config.max_uri_handlers = 24; // 22 rutas registradas, con un poco de margen

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
    httpd_register_uri_handler(s_servidor, &s_uri_control_aire);
    httpd_register_uri_handler(s_servidor, &s_uri_control_bypass);
    httpd_register_uri_handler(s_servidor, &s_uri_control_at);
    httpd_register_uri_handler(s_servidor, &s_uri_configurar_rtc);
    httpd_register_uri_handler(s_servidor, &s_uri_control_automatico);
    httpd_register_uri_handler(s_servidor, &s_uri_calibrar_sensor);
    httpd_register_uri_handler(s_servidor, &s_uri_ota);
    httpd_register_uri_handler(s_servidor, &s_uri_ws);

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

    size_t fds = WS_MAX_CLIENTES;
    int client_fds[WS_MAX_CLIENTES];
    if (httpd_get_client_list(s_servidor, &fds, client_fds) != ESP_OK) {
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
            httpd_ws_send_frame_async(s_servidor, fd, &frame);
        }
    }
}

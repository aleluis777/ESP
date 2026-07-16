// Servidor HTTP (esp_http_server): sirve el sitio estatico (index.html,
// style.css, script.js -- ver carpeta ../web) desde la particion SPIFFS
// "www", y tiene el WebSocket en /ws que empuja los datos en vivo.
//
// El sitio se graba en flash al compilar (spiffs_create_partition_image en
// main/CMakeLists.txt), en una particion separada de "storage" (donde vive
// el historial de las corridas) para que actualizar el sitio no borre datos
// guardados, y viceversa.

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include "servidor_web.h"
#include "esp_http_server.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "cJSON.h"

static const char *TAG = "SERVIDOR_WEB";
// CONFIG_LWIP_MAX_SOCKETS=16 en este proyecto (ver sdkconfig.defaults) --
// esp_http_server usa 3 sockets para si mismo, asi que max_open_sockets no
// puede pasar de 13 (16-3, el propio httpd_start() lo rechaza con
// ESP_ERR_INVALID_ARG si te pasas -- confirmado, ver historial). Antes esto
// estaba en 10 (default de ESP-IDF) y una carga de pagina normal
// (index.html + style.css + equipo.png, sin keep-alive, cada uno con su
// propia conexion) mas el WS llegaban a llenar el limite disponible, dejando
// el WS sin socket. Con 16 sobra margen de verdad.
#define WS_MAX_CLIENTES 4

static httpd_handle_t s_servidor = NULL;
static servidor_web_callbacks_t s_callbacks;

// Monta la particion SPIFFS "www" (la que arma spiffs_create_partition_image
// a partir de ../web) en /www. Idempotente: config_labgeo_init() ya la monta
// antes (necesita el config.json ahi para calibrar los sensores, que no
// dependen de la red) -- si para cuando arranca el servidor web ya esta
// montada, no hace nada. format_if_mount_failed=false a proposito: si el
// mount falla, preferimos ver el error en el log antes que formatear y
// perder el sitio que se grabo al compilar.
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

// Se llama antes de servidor_web_init() para que app_main.c decida que
// hacer cuando llega un POST de calibracion -- este archivo no sabe nada de
// HX711 ni de como se calcula pendiente/offset, solo avisa que llego el
// pedido (mismo patron que uart_link_set_callbacks()).
void servidor_web_set_callbacks(servidor_web_callbacks_t callbacks)
{
    s_callbacks = callbacks;
}

// Loguea cada peticion HTTP que entra (metodo, URI, IP del cliente) --
// se llama al principio de cada handler. httpd_req_to_sockfd() da el fd del
// socket ya aceptado por esp_http_server; getpeername() sobre ese fd es lo
// mismo que usariamos para ver quien nos pego del otro lado en un socket
// cualquiera. Con IPv6 habilitado (CONFIG_LWIP_IPV6=y) el servidor escucha
// dual-stack, por eso el sockaddr es de tipo in6 y extraemos la direccion
// IPv4 mapeada si corresponde.
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

// Info que necesita el handler generico de archivos estaticos: de donde
// leerlo en SPIFFS y que Content-Type mandar.
typedef struct {
    const char *ruta_spiffs;
    const char *content_type;
} archivo_estatico_t;

// Sirve un archivo de /www en pedazos (httpd_resp_send_chunk), asi no hace
// falta cargarlo entero en RAM -- funciona igual de bien para un archivo de
// 1KB que para uno de 100KB.
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
    httpd_resp_send_chunk(req, NULL, 0); // cierra la respuesta chunked
    return ESP_OK;
}

static const archivo_estatico_t s_archivo_index      = { "/www/index.html", "text/html" };
static const archivo_estatico_t s_archivo_style      = { "/www/style.css", "text/css" };
static const archivo_estatico_t s_archivo_script     = { "/www/script.js", "application/javascript" };
static const archivo_estatico_t s_archivo_files_html = { "/www/files.html", "text/html" };
// Grafica del historial de corridas (web/grafica.html), lee /corridas.csv
// por su cuenta con fetch() -- no necesita nada especial del backend.
static const archivo_estatico_t s_archivo_grafica_html = { "/www/grafica.html", "text/html" };
static const archivo_estatico_t s_archivo_equipo_png = { "/www/equipo.png", "image/png" };
// Sirven calibracion.json/sistema.json tal cual estan en SPIFFS -- dan 404
// hasta el primer guardado de cada uno (config_labgeo_guardar()/
// config_labgeo_guardar_red() recien los crean ahi), es esperado.
static const archivo_estatico_t s_archivo_calibracion_json = { "/www/calibracion.json", "application/json" };
static const archivo_estatico_t s_archivo_sistema_json     = { "/www/sistema.json", "application/json" };
// Historial de TODAS las corridas (almacenamiento.c), append-only -- 404
// hasta que se inicie la primera corrida.
static const archivo_estatico_t s_archivo_corridas_csv = { "/www/corridas.csv", "text/csv" };

// "/" y "/index.html" apuntan al mismo archivo -- asi entrar directo a la
// IP del controlador ya muestra la pagina, sin tener que escribir la ruta.
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
// Vista humana del listado de /files (tabla en vez de JSON crudo) -- el
// contenido esta en web/files.html, se sirve igual que index.html/style.css.
static const httpd_uri_t s_uri_files_html = {
    .uri = "/files.html", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_files_html,
};
static const httpd_uri_t s_uri_grafica_html = {
    .uri = "/grafica.html", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_grafica_html,
};
static const httpd_uri_t s_uri_equipo_png = {
    .uri = "/equipo.png", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_equipo_png,
};
static const httpd_uri_t s_uri_calibracion_json = {
    .uri = "/calibracion.json", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_calibracion_json,
};
static const httpd_uri_t s_uri_sistema_json = {
    .uri = "/sistema.json", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_sistema_json,
};
static const httpd_uri_t s_uri_corridas_csv = {
    .uri = "/corridas.csv", .method = HTTP_GET,
    .handler = archivo_estatico_handler, .user_ctx = (void *)&s_archivo_corridas_csv,
};

// Lista lo que realmente hay grabado en la particion SPIFFS "www" -- util
// para confirmar que un "idf.py flash" nuevo grabo bien la carpeta ../web
// (spiffs_create_partition_image en main/CMakeLists.txt), sin tener que
// adivinar por lo que se ve en el navegador. Devuelve JSON:
// {"total_bytes":N,"usados_bytes":N,"archivos":[{"nombre":"index.html","bytes":1234},...]}
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
    httpd_resp_sendstr_chunk(req, NULL); // cierra la respuesta chunked
    return ESP_OK;
}

static const httpd_uri_t s_uri_listado = {
    .uri = "/files", .method = HTTP_GET,
    .handler = listado_www_handler,
};

// POST /calibrar_cero -- sin body. Le avisa a app_main.c que ahora mismo la
// celda esta sin peso (tara): el callback lee el crudo actual y lo guarda
// como offset. Tiene que llamarse ANTES que /calibrar_maximo (la pendiente
// se calcula a partir de este offset).
static esp_err_t calibrar_cero_handler(httpd_req_t *req)
{
    log_peticion(req);

    if (!s_callbacks.on_calibrar_cero) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    s_callbacks.on_calibrar_cero();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// POST /calibrar_maximo -- body JSON {"peso_n": 123.4} con el peso de
// referencia ya puesto sobre la celda (en Newtons, misma unidad que
// "peso_N" en el resto del firmware). El callback lee el crudo actual,
// calcula la pendiente contra el offset ya guardado por /calibrar_cero, y
// persiste todo en config.json.
static esp_err_t calibrar_maximo_handler(httpd_req_t *req)
{
    log_peticion(req);

    char body[128];
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body vacio o invalido");
        return ESP_FAIL;
    }
    body[len] = '\0';

    cJSON *raiz = cJSON_Parse(body);
    const cJSON *peso_item = raiz ? cJSON_GetObjectItemCaseSensitive(raiz, "peso_n") : NULL;
    if (!cJSON_IsNumber(peso_item)) {
        cJSON_Delete(raiz);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "falta \"peso_n\" numerico en el body");
        return ESP_FAIL;
    }
    float peso_n = (float)peso_item->valuedouble;
    cJSON_Delete(raiz);

    if (!s_callbacks.on_calibrar_maximo) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    s_callbacks.on_calibrar_maximo(peso_n);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_calibrar_cero = {
    .uri = "/calibrar_cero", .method = HTTP_POST, .handler = calibrar_cero_handler,
};
static const httpd_uri_t s_uri_calibrar_maximo = {
    .uri = "/calibrar_maximo", .method = HTTP_POST, .handler = calibrar_maximo_handler,
};

// POST /configurar_red -- body JSON {"ip":"...","gateway":"...","mascara":"..."}.
// Por ahora solo guarda en config.json (via el callback de app_main.c);
// aplicarlo de verdad al W5500 es un paso pendiente, todavia no implementado.
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

// POST /avanzar_ensayo -- sin body. Un solo boton en la web llama siempre a
// este mismo endpoint; app_main.c es quien sabe en que estado (0..4) esta el
// ensayo y decide que transicion corresponde. Este archivo no sabe nada del
// significado de los estados, solo avisa "tocaron el boton".
static esp_err_t avanzar_ensayo_handler(httpd_req_t *req)
{
    log_peticion(req);

    if (!s_callbacks.on_avanzar_ensayo) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    s_callbacks.on_avanzar_ensayo();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static const httpd_uri_t s_uri_avanzar_ensayo = {
    .uri = "/avanzar_ensayo", .method = HTTP_POST, .handler = avanzar_ensayo_handler,
};

// Handshake / frames entrantes del WebSocket. No esperamos nada del cliente
// por ahora (el canal es solo controlador -> navegador), asi que alcanza con
// aceptar el handshake y descartar cualquier frame que llegue.
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // Esta es la peticion HTTP real (el handshake "GET /ws" con
        // Upgrade: websocket) -- las llamadas siguientes a este handler son
        // frames del WS ya establecido, no peticiones HTTP nuevas, por eso
        // el log de peticion va aca y no mas abajo.
        log_peticion(req);
        ESP_LOGI(TAG, "Cliente WS conectado (fd=%d)", httpd_req_to_sockfd(req));
        return ESP_OK;
    }

    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    httpd_ws_recv_frame(req, &frame, 0); // solo para vaciar el frame entrante, no nos interesa el contenido
    return ESP_OK;
}

static const httpd_uri_t s_uri_ws = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = ws_handler,
    .is_websocket = true,
};

// Monta el sitio, arranca el servidor HTTP y registra todas las rutas.
// lru_purge_enable hace que si se llenan los sockets disponibles, se cierre
// el cliente mas viejo/inactivo en vez de rechazar la conexion nueva.
esp_err_t servidor_web_init(void)
{
    // Si el sitio no monta (por ejemplo, nunca se flasheo la imagen de
    // ../web), seguimos igual: el WebSocket funciona sin el sitio estatico,
    // solo que "/" va a devolver 404 en vez de la pagina.
    montar_www();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = WS_MAX_CLIENTES + 8; // 12 total -- max permitido es 13 (16-3), dejamos 1 de margen
    config.lru_purge_enable = true;
    config.max_uri_handlers = 17; // 16 rutas registradas, con un poco de margen

    esp_err_t err = httpd_start(&s_servidor, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar el servidor HTTP (%s)", esp_err_to_name(err));
        return err;
    }

    httpd_register_uri_handler(s_servidor, &s_uri_raiz);
    httpd_register_uri_handler(s_servidor, &s_uri_index);
    httpd_register_uri_handler(s_servidor, &s_uri_style);
    httpd_register_uri_handler(s_servidor, &s_uri_script);
    httpd_register_uri_handler(s_servidor, &s_uri_files_html);
    httpd_register_uri_handler(s_servidor, &s_uri_grafica_html);
    httpd_register_uri_handler(s_servidor, &s_uri_equipo_png);
    httpd_register_uri_handler(s_servidor, &s_uri_calibracion_json);
    httpd_register_uri_handler(s_servidor, &s_uri_sistema_json);
    httpd_register_uri_handler(s_servidor, &s_uri_corridas_csv);
    httpd_register_uri_handler(s_servidor, &s_uri_listado);
    httpd_register_uri_handler(s_servidor, &s_uri_calibrar_cero);
    httpd_register_uri_handler(s_servidor, &s_uri_calibrar_maximo);
    httpd_register_uri_handler(s_servidor, &s_uri_configurar_red);
    httpd_register_uri_handler(s_servidor, &s_uri_avanzar_ensayo);
    httpd_register_uri_handler(s_servidor, &s_uri_ws);

    ESP_LOGI(TAG, "Servidor HTTP + WS listo (puerto %d)", config.server_port);
    return ESP_OK;
}

// Recorre todos los sockets abiertos del servidor, se queda solo con los
// que son WebSockets activos (podria haber tambien conexiones HTTP normales
// a "/"), y les manda el mismo frame de texto a cada uno.
// httpd_ws_send_frame_async() se puede llamar desde cualquier tarea (no
// hace falta estar "dentro" de un handler de esp_http_server), por eso
// tarea_sensores puede invocar esta funcion directamente.
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

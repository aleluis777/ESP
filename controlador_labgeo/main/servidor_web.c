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

static const char *TAG = "SERVIDOR_WEB";
// LWIP_MAX_SOCKETS (default de este proyecto) solo da margen para 7 sockets
// abiertos en simultaneo, y esp_http_server ya usa 3 para si mismo -- por
// eso el limite real de clientes WS es bajo. Si hace falta mas clientes a
// la vez, subir CONFIG_LWIP_MAX_SOCKETS en sdkconfig.defaults.
#define WS_MAX_CLIENTES 4

static httpd_handle_t s_servidor = NULL;

// Monta la particion SPIFFS "www" (la que arma spiffs_create_partition_image
// a partir de ../web) en /www. format_if_mount_failed=false a proposito: si
// el mount falla, preferimos ver el error en el log antes que formatear y
// perder el sitio que se grabo al compilar.
static esp_err_t montar_www(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/www",
        .partition_label = "www",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo montar la particion 'www' (%s)", esp_err_to_name(err));
    }
    return err;
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
    config.max_open_sockets = WS_MAX_CLIENTES + 2; // + margen para pedidos HTTP normales
    config.lru_purge_enable = true;
    config.max_uri_handlers = 8; // default (8) alcanza para las 7 rutas, queda explicito por claridad

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
    httpd_register_uri_handler(s_servidor, &s_uri_listado);
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

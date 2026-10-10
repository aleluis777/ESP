#pragma once

// Login de la interfaz web con dos usuarios:
//
//   "admin" -- el cliente. Panel, historico, configuracion y controles.
//              Contrasena en NVS; de fabrica AUTH_PASSWORD_DEFAULT.
//   "root"  -- el fabricante. Todo lo anterior + files/ota/root.html, y es
//              el UNICO que cambia contrasenas (la suya y la del admin).
//              De fabrica: el hash de auth_root_fabrica.h (generado por
//              tools/generar_hash_root.py -- la contrasena no esta en el
//              codigo). Si se cambia desde root.html, la nueva queda en NVS.
//
// En NVS solo se guardan hashes con salt, nunca contrasenas. Las dos
// sobreviven a "idf.py flash" y a OTA; borrando la NVS (o erase-flash)
// vuelven a las de fabrica.
//
// Sesiones: cookie "sesion" con un token aleatorio que vive solo en RAM (un
// reinicio cierra todas). Expira tras AUTH_SESION_INACTIVA_S sin actividad.
//
// Todas las funciones que reciben httpd_req_t se llaman desde la tarea del
// servidor HTTP (una sola), asi que no hay concurrencia entre ellas.

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUTH_USUARIO_ADMIN      "admin"
#define AUTH_USUARIO_ROOT       "root"
#define AUTH_PASSWORD_DEFAULT   "braindlab" // admin de fabrica
#define AUTH_PASSWORD_MIN       6
#define AUTH_PASSWORD_ROOT_MIN  12
#define AUTH_PASSWORD_MAX       64
#define AUTH_SESION_INACTIVA_S  (30 * 60)

typedef enum {
    AUTH_NIVEL_NINGUNO = 0, // sin sesion
    AUTH_NIVEL_ADMIN = 1,
    AUTH_NIVEL_ROOT = 2,
} auth_nivel_t;

// Inicializa NVS (si nadie lo hizo) y carga los hashes. Si no hay
// contrasena de admin guardada, graba la de fabrica.
esp_err_t auth_web_init(void);

// Nivel de la sesion del request (y le renueva la expiracion).
auth_nivel_t auth_web_nivel(httpd_req_t *req);

// true si el request trae cualquier sesion valida.
bool auth_web_sesion_valida(httpd_req_t *req);

// Para el inicio de cada handler protegido: si no alcanza el nivel
// responde 401 (sin sesion) o 403 (admin pidiendo algo de root) y devuelve
// false -- el handler debe salir con ESP_OK sin hacer nada mas.
bool auth_web_requerir(httpd_req_t *req);
bool auth_web_requerir_root(httpd_req_t *req);

// Registra POST /login, POST /logout, POST /cambiar_password y GET
// /api/sesion (login.html y root.html los sirve servidor_web.c).
void auth_web_registrar_handlers(httpd_handle_t servidor);

#ifdef __cplusplus
}
#endif

#pragma once

// Login de la interfaz web: un solo usuario ("admin"), contrasena guardada
// en NVS (sobrevive a "idf.py flash" y a OTA -- solo "idf.py erase-flash"
// la vuelve al valor de fabrica). En NVS se guarda solo un hash con salt,
// nunca la contrasena en texto plano.
//
// Sesiones: al hacer login se entrega una cookie "sesion" con un token
// aleatorio que vive solo en RAM (un reinicio cierra todas las sesiones).
// Expira tras AUTH_SESION_INACTIVA_S sin actividad.
//
// Todas las funciones que reciben httpd_req_t se llaman desde la tarea del
// servidor HTTP (una sola), asi que no hay concurrencia entre ellas.

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUTH_USUARIO            "admin"
#define AUTH_PASSWORD_DEFAULT   "braindlab"
#define AUTH_PASSWORD_MIN       6
#define AUTH_PASSWORD_MAX       64
#define AUTH_SESION_INACTIVA_S  (30 * 60)

// Inicializa NVS (si nadie lo hizo) y carga el hash. Si no hay contrasena
// guardada, graba la de fabrica (AUTH_PASSWORD_DEFAULT).
esp_err_t auth_web_init(void);

// true si el request trae una cookie de sesion valida (y le renueva la
// expiracion).
bool auth_web_sesion_valida(httpd_req_t *req);

// Para el inicio de cada handler protegido: si no hay sesion responde
// 401 (APIs) y devuelve false -- el handler debe salir con ESP_OK sin
// hacer nada mas.
bool auth_web_requerir(httpd_req_t *req);

// true si la contrasena sigue siendo la de fabrica (la web muestra aviso).
bool auth_web_password_por_defecto(void);

// Registra POST /login, POST /logout, POST /cambiar_password y GET
// /api/sesion (login.html lo sirve servidor_web.c como archivo publico).
void auth_web_registrar_handlers(httpd_handle_t servidor);

#ifdef __cplusplus
}
#endif

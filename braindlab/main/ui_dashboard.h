#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Numero de unidades de aire acondicionado y de sensores ambientales que
// muestra el dashboard. Si el hardware real termina teniendo otra cantidad,
// cambiar aca y en ui_dashboard.c (crear_columna_izquierda/crear_columna_derecha).
#define UI_DASH_NUM_AIRES   4
#define UI_DASH_NUM_SENSORES 4

// Franjas de aire que se animan (parpadeo de opacidad, en cascada) sobre el
// icono del equipo mientras esta encendido, para simular el flujo de aire.
#define UI_DASH_NUM_VIENTO 3

// Tarjeta de una unidad de aire acondicionado (ver "AIRES ACONDICIONADOS" en
// el mockup). Todo esto es solo vista: la logica que decide temperatura,
// velocidad o estado real vive afuera (todavia no existe, ver braindlab.c).
// El encendido/apagado (boton ON/OFF) si se maneja aca mismo porque es un
// efecto puramente visual (animar el "viento"), sin dato real detras todavia.
typedef struct {
    lv_obj_t *led_estado;       // punto verde/gris: online/offline
    lv_obj_t *lbl_nombre;       // "Oficina 1", etc.
    lv_obj_t *img_unidad;       // icono del equipo (air-conditioner.png)
    lv_obj_t *viento[UI_DASH_NUM_VIENTO]; // franjas animadas de "aire fluyendo"
    lv_obj_t *lbl_temp_actual;  // "24.0" (numero grande)
    lv_obj_t *lbl_set;          // "Set: 24.0°C"
    lv_obj_t *lbl_velocidad;    // "Auto" / "1" / "2" / "3"
    lv_obj_t *btn_power;        // boton ON/OFF
    lv_obj_t *lbl_btn_power;    // texto del boton ("ON"/"OFF")
    bool      encendido;
} ui_dash_aire_t;

// Tarjeta de un sensor ambiental (T1..T4).
typedef struct {
    lv_obj_t *lbl_nombre;  // "Oficina 1"
    lv_obj_t *lbl_valor;   // "23.6°C"
} ui_dash_sensor_t;

typedef struct {
    lv_obj_t *cont;  // Contenedor raiz de esta vista (para mostrarla/ocultarla completa)

    // ---- Barra superior ----
    lv_obj_t *led_estado_sistema;  // punto verde: "Sistema OK"
    lv_obj_t *lbl_estado_sistema;
    lv_obj_t *lbl_hora;
    lv_obj_t *lbl_fecha;

    // ---- Aires acondicionados ----
    ui_dash_aire_t aires[UI_DASH_NUM_AIRES];

    // ---- Sensores ambientales ----
    ui_dash_sensor_t sensores[UI_DASH_NUM_SENSORES];
    lv_obj_t *lbl_humedad;      // "55 %RH"
    lv_obj_t *lbl_temp_gestor;  // "28.0°C" -- temperatura del gabinete (AM2301A)

    // ---- Alarmas activas ----
    lv_obj_t *cont_alarmas;    // contenedor donde se listan
    lv_obj_t *fila_alarma_at;  // fila "AT" -- se muestra/oculta segun alarma_at real
    lv_obj_t *fila_alarma_bps; // fila "BPS" -- se muestra/oculta segun bypass_activo real

    // ---- Controles Bypass / AT ----
    // Mismo patron que los botones de aire (ui_dash_aire_t.btn_power): el
    // toggle visual es inmediato al toque (ver toggle_bypass_cb/toggle_at_cb
    // en ui_dashboard.c), el pedido real se manda por UART aparte (ver
    // braindlab.c) y uart_braindlab.c corrige la vista con el estado real
    // que llega en el proximo ESTADO_UPDATE (bypass_solicitado/alarma_at,
    // los mismos campos "ya mezclados con lo automatico" que usa
    // salida_aire para AA1-4).
    lv_obj_t *btn_bypass;
    lv_obj_t *lbl_btn_bypass;
    bool      bypass_activo;
    lv_obj_t *btn_at;
    lv_obj_t *lbl_btn_at;
    bool      at_activo;

    // ---- Barra inferior de navegacion ----
    // Inicio / Graficas / Ajustes / Red / Fecha-Hora. "Graficas" abre la
    // vista de Parametros Electricos (ver ui_energia.c: se saco de aca
    // porque las 6 tarjetas no entraban con texto legible). Los otros 3
    // botones quedan creados y con su callback enganchable, pero sin vista
    // propia todavia (cada una se va a agregar como su propio ui_*.c, igual
    // que ui_grafica en el proyecto blink, para no bloquear el trabajo de
    // las demas).
    lv_obj_t *btn_nav_inicio;
    lv_obj_t *btn_nav_graficas;
    lv_obj_t *btn_nav_ajustes;
    lv_obj_t *btn_nav_red;
    lv_obj_t *btn_nav_fecha_hora;
} ui_dashboard_t;

// Construye la interfaz completa dentro de 'parent' (normalmente lv_screen_active()).
// Debe llamarse dentro de lvgl_port_lock()/lvgl_port_unlock().
// El puntero devuelto es estatico (vive mientras dure el programa), no hay que liberarlo.
ui_dashboard_t *ui_dashboard_create(lv_obj_t *parent);

// Aplica el estado ON/OFF real a una tarjeta de aire (colores, icono, viento
// animado) -- misma logica visual que usaba el toggle interno del boton de
// encendido, expuesta para que uart_braindlab.c la pueda aplicar cuando
// llega el estado real por UART (ver protocolo_braindlab.h).
void ui_dashboard_aire_set_encendido(ui_dash_aire_t *aire, bool encendido);

// Mismo criterio que ui_dashboard_aire_set_encendido() pero para los
// botones de Bypass y AT (un solo boton cada uno, no hace falta indice).
void ui_dashboard_bypass_set_activo(ui_dashboard_t *ui, bool activo);
void ui_dashboard_at_set_activo(ui_dashboard_t *ui, bool activo);

#ifdef __cplusplus
}
#endif

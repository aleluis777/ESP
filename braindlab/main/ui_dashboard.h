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
// el mockup). Todo lo que muestra llega por UART en ESTADO_UPDATE (ver
// uart_braindlab.c): encendido, si es la de reserva de la semana y si esta
// forzada a mano. El controlador no sabe temperatura ni setpoint de cada
// equipo (solo lo prende/apaga por rele), por eso no se muestran.
typedef struct {
    lv_obj_t *card;             // tarjeta completa (se oculta si el aire no se usa)
    lv_obj_t *led_estado;       // punto verde/gris: encendido/apagado
    lv_obj_t *lbl_nombre;       // nombre del sitio ("AA1" por defecto)
    lv_obj_t *img_unidad;       // icono del equipo (air-conditioner.png)
    lv_obj_t *viento[UI_DASH_NUM_VIENTO]; // franjas animadas de "aire fluyendo"
    lv_obj_t *lbl_rol;          // "Principal" / "Reserva"
    lv_obj_t *lbl_modo;         // "Automatico" / "Manual"
    lv_obj_t *btn_power;        // boton ON/OFF
    lv_obj_t *lbl_btn_power;    // texto del boton ("ON"/"OFF")
    bool      encendido;
} ui_dash_aire_t;

// Tarjeta de un sensor ambiental (T1..T4).
typedef struct {
    lv_obj_t *card;        // tarjeta completa (se oculta si no esta instalado)
    lv_obj_t *lbl_nombre;  // nombre del sitio ("T1" por defecto)
    lv_obj_t *lbl_valor;   // "23.6°C", o "--" en rojo si no hay lectura
} ui_dash_sensor_t;

typedef struct {
    lv_obj_t *cont;  // Contenedor raiz de esta vista (para mostrarla/ocultarla completa)

    // ---- Barra superior ----
    lv_obj_t *lbl_titulo;          // nombre del equipo ("SISTEMA DE MONITOREO" por defecto)
    lv_obj_t *led_estado_sistema;  // punto verde: "Sistema OK"
    lv_obj_t *lbl_estado_sistema;
    lv_obj_t *lbl_hora;
    lv_obj_t *lbl_fecha;

    // ---- Aires acondicionados ----
    lv_obj_t *lbl_ciclo;        // ciclo de la automatica, al lado del titulo de la seccion
    ui_dash_aire_t aires[UI_DASH_NUM_AIRES];

    // ---- Sensores ambientales ----
    lv_obj_t *titulo_sensores;  // titulo + grilla: se ocultan si no hay ningun NTC instalado
    lv_obj_t *grid_sensores;
    ui_dash_sensor_t sensores[UI_DASH_NUM_SENSORES];
    lv_obj_t *titulo_gabinete;  // titulo (= nombre del sensor 5) + tarjeta del AM2301A
    lv_obj_t *card_gabinete;
    lv_obj_t *lbl_humedad;      // "55 %RH"
    lv_obj_t *lbl_temp_gestor;  // "28.0°C" -- temperatura del gabinete (AM2301A)

    // ---- Alarmas activas ----
    lv_obj_t *cont_alarmas;    // contenedor donde se listan
    lv_obj_t *fila_alarma_at;  // fila "AT" -- se muestra/oculta segun alarma_at real
    lv_obj_t *fila_alarma_bps; // fila "BPS" -- se muestra/oculta segun bypass_activo real
    lv_obj_t *fila_alarma_sd;  // fila "SD" -- se muestra si la MicroSD del controlador falla
    lv_obj_t *lbl_alarma_sd;   // descripcion de esa fila (sin tarjeta / sin formato / ...)
    lv_obj_t *fila_alarma_mod; // fila "MOD" -- modulos del controlador con falla (RTC, ADC, ...)
    lv_obj_t *lbl_alarma_mod;  // descripcion de esa fila (lista de modulos)
    lv_obj_t *fila_alarma_com; // fila "COM" -- esta pantalla no recibe tramas del controlador

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
    // Inicio / Energia. "Energia" abre la vista de Parametros Electricos
    // (ver ui_energia.c: se saco de aca porque las 6 tarjetas no entraban
    // con texto legible).
    lv_obj_t *btn_nav_inicio;
    lv_obj_t *btn_nav_energia;
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

// Rol del aire esta semana y si esta forzado a mano (ESTADO_UPDATE:
// indice_reserva y aire_manual). 'hay_reserva'=false con 1 solo aire.
void ui_dashboard_aire_set_rol(ui_dash_aire_t *aire, bool es_reserva, bool hay_reserva, bool manual);

// Nombre del sitio recibido por UART (BRAINDLAB_CMD_NOMBRE): 'tipo' es
// BRAINDLAB_NOMBRE_EQUIPO/_AIRE/_SENSOR de protocolo_braindlab.h. Texto
// vacio = nombre por defecto. Sensores 0..3 = T1..T4, 4 = gabinete.
void ui_dashboard_set_nombre(ui_dashboard_t *ui, uint8_t tipo, uint8_t indice, const char *texto);

// Oculta los sensores no instalados ('sens_inst': bit i = sensor i+1) y los
// aires por encima de 'n_aires'. Solo toca LVGL si algo cambio.
void ui_dashboard_set_instalacion(ui_dashboard_t *ui, uint8_t sens_inst, uint8_t n_aires);

#ifdef __cplusplus
}
#endif

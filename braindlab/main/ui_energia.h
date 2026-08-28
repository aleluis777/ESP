#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Vista de parametros electricos. Se saco del dashboard de Inicio porque ahi
// las 6 tarjetas (Voltaje, Corriente, Potencia, Energia, Frecuencia, Factor
// Potencia) no entraban con texto legible en una sola fila de 78px. Vista
// independiente, igual que ui_grafica en el proyecto blink.
typedef struct {
    lv_obj_t *cont;        // Contenedor raiz de esta vista (mostrar/ocultar completa)
    lv_obj_t *btn_volver;  // Vuelve al dashboard (ui_dashboard)

    lv_obj_t *lbl_voltaje;
    lv_obj_t *lbl_corriente;
    lv_obj_t *lbl_potencia_activa;
    lv_obj_t *lbl_energia_hoy;
    lv_obj_t *lbl_frecuencia;
    lv_obj_t *lbl_factor_potencia;
} ui_energia_t;

// Construye la vista dentro de 'parent' (normalmente lv_screen_active()).
// Debe llamarse dentro de lvgl_port_lock()/lvgl_port_unlock().
// El puntero devuelto es estatico (vive mientras dure el programa), no hay que liberarlo.
ui_energia_t *ui_energia_create(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Vista de parametros electricos: voltaje y corriente de las fases R, S, T
// (medidor JSY-MK-333G del controlador, llegan por UART en
// ESTADO_UPDATE -- ver protocolo_braindlab.h). Vista independiente del
// dashboard de Inicio, igual que ui_grafica en el proyecto blink.
#define UI_ENERGIA_NUM_FASES 3

typedef struct {
    lv_obj_t *cont;        // Contenedor raiz de esta vista (mostrar/ocultar completa)
    lv_obj_t *btn_volver;  // Vuelve al dashboard (ui_dashboard)

    lv_obj_t *lbl_voltaje[UI_ENERGIA_NUM_FASES];   // R, S, T
    lv_obj_t *lbl_corriente[UI_ENERGIA_NUM_FASES]; // R, S, T
} ui_energia_t;

// Construye la vista dentro de 'parent' (normalmente lv_screen_active()).
// Debe llamarse dentro de lvgl_port_lock()/lvgl_port_unlock().
// El puntero devuelto es estatico (vive mientras dure el programa), no hay que liberarlo.
ui_energia_t *ui_energia_create(lv_obj_t *parent);

// Pinta voltajes (decimas de V) y corrientes (centesimas de A) de R, S, T,
// tal cual llegan por UART. ok=false -> muestra "--" (medidor sin
// respuesta). Llamar con el lock de LVGL tomado.
void ui_energia_set_valores(ui_energia_t *ui, bool ok,
                            const uint16_t voltajes_decimas[UI_ENERGIA_NUM_FASES],
                            const uint16_t corrientes_centesimas[UI_ENERGIA_NUM_FASES]);

#ifdef __cplusplus
}
#endif

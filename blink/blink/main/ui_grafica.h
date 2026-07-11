#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Vista de graficas: muestra los datos guardados de las corridas.
// Es una vista independiente de ui_labgeo (su propio archivo/"libreria"),
// para poder agregar mas vistas a futuro sin tocar el dashboard principal.
typedef struct {
    lv_obj_t *cont;         // Contenedor raiz de esta vista (mostrar/ocultar completa)
    lv_obj_t *btn_volver;   // Vuelve al dashboard (ui_labgeo)

    lv_obj_t *btn_corrida1; // Pide/muestra los datos de la Primera Corrida
    lv_obj_t *btn_corrida2; // Pide/muestra los datos de la Segunda Corrida

    lv_obj_t *chart;
    lv_chart_series_t *serie_dial1;
    lv_chart_series_t *serie_peso;
} ui_grafica_t;

// Construye la vista dentro de 'parent' (normalmente lv_screen_active()).
// Debe llamarse dentro de lvgl_port_lock()/lvgl_port_unlock().
// El puntero devuelto es estatico (vive mientras dure el programa), no hay que liberarlo.
ui_grafica_t *ui_grafica_create(lv_obj_t *parent);

// Agrega un punto nuevo a las series (valor de Dial 1 y de Peso), alimentado
// por los datos que llegan del controlador (ver uart_labgeo.c).
void ui_grafica_agregar_punto(ui_grafica_t *ui, int32_t valor_dial1, int32_t valor_peso);

// Vacia el chart (se usa antes de pedir los datos de una corrida nueva).
void ui_grafica_reset(ui_grafica_t *ui);

#ifdef __cplusplus
}
#endif

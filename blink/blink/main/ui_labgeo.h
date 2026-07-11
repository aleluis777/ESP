#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Handles a los widgets de la interfaz LabGeo: 4 sensores (2 diales de
// deformacion, peso, tiempo transcurrido) y el flujo de dos corridas
// consecutivas (Primera Corrida / Segunda Corrida).
typedef struct {
    lv_obj_t *cont;  // Contenedor raiz de esta vista (para mostrarla/ocultarla completa)

    lv_obj_t *lbl_corrida_activa;  // "Corrida activa: Primera/Segunda Corrida"
    lv_obj_t *btn_ver_grafica;     // Cambia a la vista de graficas (ui_grafica)

    // ---- Sensores (solo lectura, la logica/lectura real vive en blink.c) ----
    lv_obj_t *lbl_dial1;   // Sensor de Dial 1 - deformacion/longitud
    lv_obj_t *lbl_dial2;   // Sensor de Dial 2 - deformacion/longitud (2do punto, diametro o promedio)
    lv_obj_t *lbl_peso;    // Sensor de peso/carga (N)
    lv_obj_t *lbl_tiempo;  // Cronometro de la corrida activa

    // ---- Control del flujo de corridas ----
    lv_obj_t *btn_iniciar;      // Inicia/Detiene la corrida activa
    lv_obj_t *lbl_btn_iniciar;  // Texto del boton (se actualiza segun el estado)
    lv_obj_t *btn_siguiente;    // Avanza de la Primera a la Segunda Corrida
} ui_labgeo_t;

// Construye la interfaz completa dentro de 'parent' (normalmente lv_screen_active()).
// Debe llamarse dentro de lvgl_port_lock()/lvgl_port_unlock().
// El puntero devuelto es estatico (vive mientras dure el programa), no hay que liberarlo.
ui_labgeo_t *ui_labgeo_create(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif

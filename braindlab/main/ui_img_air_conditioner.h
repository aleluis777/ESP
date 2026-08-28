#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Imagen del equipo de aire acondicionado (icono de cada tarjeta en
// ui_dashboard.c). Generada con LVGLImage.py a partir de air-conditioner.png
// (achicada a 160x62 antes de convertir, ver air_conditioner_small.png).
extern const lv_image_dsc_t air_conditioner;

#ifdef __cplusplus
}
#endif

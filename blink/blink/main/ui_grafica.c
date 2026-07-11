#include "ui_grafica.h"

#define COLOR_BG_ROOT   lv_color_hex(0xF2F2F2)
#define COLOR_TITULO    lv_color_hex(0x606060)
#define COLOR_DIAL1     lv_color_hex(0x1976D2)
#define COLOR_PESO      lv_color_hex(0xE81010)

#define PUNTOS_CHART 30

static ui_grafica_t ui;

static void crear_barra_superior(lv_obj_t *parent)
{
    lv_obj_t *barra = lv_obj_create(parent);
    lv_obj_remove_style_all(barra);
    lv_obj_set_size(barra, LV_PCT(100), 50);
    lv_obj_set_flex_flow(barra, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(barra, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ui.btn_volver = lv_button_create(barra);
    lv_obj_set_size(ui.btn_volver, 140, 44);
    lv_obj_t *lbl_btn = lv_label_create(ui.btn_volver);
    lv_label_set_text(lbl_btn, LV_SYMBOL_LEFT " Volver");
    lv_obj_center(lbl_btn);

    lv_obj_t *lbl_titulo = lv_label_create(barra);
    lv_label_set_text(lbl_titulo, "Datos Guardados");
    lv_obj_set_style_text_font(lbl_titulo, &lv_font_montserrat_28, 0);

    // Selector de corrida: cada boton pide (via UART) los datos de esa corrida.
    lv_obj_t *fila_corridas = lv_obj_create(barra);
    lv_obj_remove_style_all(fila_corridas);
    lv_obj_set_flex_flow(fila_corridas, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(fila_corridas, 8, 0);
    lv_obj_set_size(fila_corridas, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    ui.btn_corrida1 = lv_button_create(fila_corridas);
    lv_obj_set_size(ui.btn_corrida1, 110, 44);
    lv_obj_t *lbl_c1 = lv_label_create(ui.btn_corrida1);
    lv_label_set_text(lbl_c1, "Corrida 1");
    lv_obj_center(lbl_c1);

    ui.btn_corrida2 = lv_button_create(fila_corridas);
    lv_obj_set_size(ui.btn_corrida2, 110, 44);
    lv_obj_t *lbl_c2 = lv_label_create(ui.btn_corrida2);
    lv_label_set_text(lbl_c2, "Corrida 2");
    lv_obj_center(lbl_c2);
}

static lv_obj_t *crear_item_leyenda(lv_obj_t *parent, lv_color_t color, const char *texto)
{
    lv_obj_t *fila = lv_obj_create(parent);
    lv_obj_remove_style_all(fila);
    lv_obj_set_flex_flow(fila, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fila, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(fila, 6, 0);
    lv_obj_set_size(fila, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    lv_obj_t *punto = lv_obj_create(fila);
    lv_obj_remove_style_all(punto);
    lv_obj_set_size(punto, 14, 14);
    lv_obj_set_style_radius(punto, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(punto, color, 0);
    lv_obj_set_style_bg_opa(punto, LV_OPA_COVER, 0);

    lv_obj_t *lbl = lv_label_create(fila);
    lv_label_set_text(lbl, texto);

    return fila;
}

ui_grafica_t *ui_grafica_create(lv_obj_t *parent)
{
    ui.cont = lv_obj_create(parent);
    lv_obj_remove_style_all(ui.cont);
    lv_obj_set_size(ui.cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui.cont, COLOR_BG_ROOT, 0);
    lv_obj_set_style_bg_opa(ui.cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ui.cont, 20, 0);
    lv_obj_set_style_pad_row(ui.cont, 14, 0);
    lv_obj_clear_flag(ui.cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ui.cont, LV_FLEX_FLOW_COLUMN);

    crear_barra_superior(ui.cont);

    // Leyenda: que color corresponde a cada serie.
    lv_obj_t *leyenda = lv_obj_create(ui.cont);
    lv_obj_remove_style_all(leyenda);
    lv_obj_set_size(leyenda, LV_PCT(100), 24);
    lv_obj_set_flex_flow(leyenda, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(leyenda, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(leyenda, 30, 0);
    crear_item_leyenda(leyenda, COLOR_DIAL1, "Dial 1 (mm)");
    crear_item_leyenda(leyenda, COLOR_PESO, "Peso (N)");

    // Chart (se lleva todo el alto sobrante)
    ui.chart = lv_chart_create(ui.cont);
    lv_obj_set_width(ui.chart, LV_PCT(100));
    lv_obj_set_flex_grow(ui.chart, 1);
    lv_obj_set_style_bg_color(ui.chart, lv_color_white(), 0);
    lv_obj_set_style_radius(ui.chart, 8, 0);
    lv_obj_set_style_border_width(ui.chart, 1, 0);
    lv_obj_set_style_border_color(ui.chart, lv_color_hex(0x999999), 0);

    lv_chart_set_type(ui.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(ui.chart, PUNTOS_CHART);
    // Rangos de ejemplo (mm y N); ajustar al rango real de tus sensores.
    lv_chart_set_axis_range(ui.chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);    // Dial 1, en mm
    lv_chart_set_axis_range(ui.chart, LV_CHART_AXIS_SECONDARY_Y, 0, 5000); // Peso, en N

    ui.serie_dial1 = lv_chart_add_series(ui.chart, COLOR_DIAL1, LV_CHART_AXIS_PRIMARY_Y);
    ui.serie_peso  = lv_chart_add_series(ui.chart, COLOR_PESO, LV_CHART_AXIS_SECONDARY_Y);

    ui_grafica_reset(&ui);

    return &ui;
}

void ui_grafica_agregar_punto(ui_grafica_t *g, int32_t valor_dial1, int32_t valor_peso)
{
    lv_chart_set_next_value(g->chart, g->serie_dial1, valor_dial1);
    lv_chart_set_next_value(g->chart, g->serie_peso, valor_peso);
}

void ui_grafica_reset(ui_grafica_t *g)
{
    lv_chart_set_all_values(g->chart, g->serie_dial1, LV_CHART_POINT_NONE);
    lv_chart_set_all_values(g->chart, g->serie_peso, LV_CHART_POINT_NONE);
}

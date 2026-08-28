#include "ui_energia.h"

// ---------------------------------------------------------------------
// Vista "Parametros electricos": pantalla completa, separada del dashboard
// de Inicio para que cada tarjeta tenga espacio real (ver ui_dashboard.c,
// de donde se saco este bloque). Solo vista -- los valores son de ejemplo.
// ---------------------------------------------------------------------

#define COLOR_BG_ROOT        lv_color_hex(0x0A0E17)
#define COLOR_CARD_BG        lv_color_hex(0x111A2B)
#define COLOR_CARD_BORDER    lv_color_hex(0x1E2A3D)
#define COLOR_TITULO_SECCION lv_color_hex(0x4FA3F7)
#define COLOR_TEXTO_SEC      lv_color_hex(0x8A94A6)
#define COLOR_TEXTO_PRINC    lv_color_white()
#define COLOR_VALOR_AZUL     lv_color_hex(0x3B9EFF)
#define COLOR_VERDE          lv_color_hex(0x22C55E)

static ui_energia_t ui;

static lv_obj_t *crear_tarjeta(lv_obj_t *parent, const char *titulo,
                                const char *valor_inicial, lv_color_t color_valor)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_style_bg_color(card, COLOR_CARD_BG, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, COLOR_CARD_BORDER, 0);
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_height(card, LV_PCT(100));

    lv_obj_t *lbl_titulo = lv_label_create(card);
    lv_label_set_text(lbl_titulo, titulo);
    lv_obj_set_style_text_color(lbl_titulo, COLOR_TEXTO_SEC, 0);
    lv_obj_set_style_text_font(lbl_titulo, &lv_font_montserrat_14, 0);

    lv_obj_t *lbl_valor = lv_label_create(card);
    lv_label_set_text(lbl_valor, valor_inicial);
    lv_obj_set_style_text_color(lbl_valor, color_valor, 0);
    lv_obj_set_style_text_font(lbl_valor, &lv_font_montserrat_24, 0);

    return lbl_valor;
}

static void crear_barra_superior(lv_obj_t *parent)
{
    lv_obj_t *barra = lv_obj_create(parent);
    lv_obj_remove_style_all(barra);
    lv_obj_set_size(barra, LV_PCT(100), 34);
    lv_obj_set_flex_flow(barra, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(barra, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(barra, 14, 0);

    ui.btn_volver = lv_button_create(barra);
    lv_obj_set_size(ui.btn_volver, 90, 34);
    lv_obj_set_style_bg_color(ui.btn_volver, COLOR_CARD_BG, 0);
    lv_obj_set_style_border_width(ui.btn_volver, 1, 0);
    lv_obj_set_style_border_color(ui.btn_volver, COLOR_CARD_BORDER, 0);
    lv_obj_set_style_radius(ui.btn_volver, 6, 0);
    lv_obj_t *lbl_volver = lv_label_create(ui.btn_volver);
    lv_label_set_text(lbl_volver, LV_SYMBOL_LEFT " Volver");
    lv_obj_set_style_text_color(lbl_volver, COLOR_TEXTO_PRINC, 0);
    lv_obj_center(lbl_volver);

    lv_obj_t *lbl_titulo = lv_label_create(barra);
    lv_label_set_text(lbl_titulo, "PARAMETROS ELECTRICOS");
    lv_obj_set_style_text_color(lbl_titulo, COLOR_TEXTO_PRINC, 0);
    lv_obj_set_style_text_font(lbl_titulo, &lv_font_montserrat_16, 0);
}

ui_energia_t *ui_energia_create(lv_obj_t *parent)
{
    ui.cont = lv_obj_create(parent);
    lv_obj_remove_style_all(ui.cont);
    lv_obj_set_size(ui.cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui.cont, COLOR_BG_ROOT, 0);
    lv_obj_set_style_bg_opa(ui.cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ui.cont, 12, 0);
    lv_obj_set_style_pad_row(ui.cont, 12, 0);
    lv_obj_clear_flag(ui.cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ui.cont, LV_FLEX_FLOW_COLUMN);

    crear_barra_superior(ui.cont);

    lv_obj_t *grid = lv_obj_create(ui.cont);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(grid, 14, 0);

    lv_obj_t *fila1 = lv_obj_create(grid);
    lv_obj_remove_style_all(fila1);
    lv_obj_set_width(fila1, LV_PCT(100));
    lv_obj_set_flex_grow(fila1, 1);
    lv_obj_set_flex_flow(fila1, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(fila1, 14, 0);

    lv_obj_t *fila2 = lv_obj_create(grid);
    lv_obj_remove_style_all(fila2);
    lv_obj_set_width(fila2, LV_PCT(100));
    lv_obj_set_flex_grow(fila2, 1);
    lv_obj_set_flex_flow(fila2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(fila2, 14, 0);

    ui.lbl_voltaje         = crear_tarjeta(fila1, "Voltaje L-N", "223.4 V", COLOR_TEXTO_PRINC);
    ui.lbl_corriente       = crear_tarjeta(fila1, "Corriente Total", "12.6 A", COLOR_TEXTO_PRINC);
    ui.lbl_potencia_activa = crear_tarjeta(fila1, "Potencia Activa", "2.81 kW", COLOR_VERDE);

    ui.lbl_energia_hoy     = crear_tarjeta(fila2, "Energia Hoy", "18.6 kWh", COLOR_VALOR_AZUL);
    ui.lbl_frecuencia      = crear_tarjeta(fila2, "Frecuencia", "60.0 Hz", COLOR_TEXTO_PRINC);
    ui.lbl_factor_potencia = crear_tarjeta(fila2, "Factor Potencia", "0.92", COLOR_TEXTO_PRINC);

    return &ui;
}

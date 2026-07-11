#include "ui_labgeo.h"
#include "ui_img_prensa.h"

#define COLOR_BG_ROOT   lv_color_hex(0xF2F2F2)
#define COLOR_CARD_BG   lv_color_white()
#define COLOR_BORDER    lv_color_hex(0x999999)
#define COLOR_TITULO    lv_color_hex(0x606060)

// El tercio derecho de la pantalla queda para la imagen del equipo;
// los dos tercios izquierdos son para los numeros de los sensores.
#define ANCHO_COL_DERECHA 300

static ui_labgeo_t ui;

// Tarjeta blanca generica (mismo estilo que las de sensores), usada tambien
// para encuadrar la imagen del equipo.
static lv_obj_t *crear_tarjeta_base(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_style_bg_color(card, COLOR_CARD_BG, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, COLOR_BORDER, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 4, 0);
    return card;
}

// Tarjeta con titulo pequeno arriba y valor grande + unidad centrado abajo.
// Se usa para los 4 sensores (Dial 1, Dial 2, Peso, Tiempo).
static lv_obj_t *crear_tarjeta_sensor(lv_obj_t *parent, const char *titulo,
                                       const char *valor_inicial, const char *unidad)
{
    lv_obj_t *card = crear_tarjeta_base(parent);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_height(card, LV_PCT(100));

    lv_obj_t *lbl_titulo = lv_label_create(card);
    lv_label_set_text(lbl_titulo, titulo);
    lv_obj_set_style_text_color(lbl_titulo, COLOR_TITULO, 0);

    lv_obj_t *fila_valor = lv_obj_create(card);
    lv_obj_remove_style_all(fila_valor);
    lv_obj_set_flex_flow(fila_valor, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fila_valor, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(fila_valor, 6, 0);
    lv_obj_set_size(fila_valor, LV_SIZE_CONTENT, LV_SIZE_CONTENT);

    lv_obj_t *lbl_valor = lv_label_create(fila_valor);
    lv_label_set_text(lbl_valor, valor_inicial);
    lv_obj_set_style_text_font(lbl_valor, &lv_font_montserrat_28, 0);

    if (unidad[0] != '\0') {
        lv_obj_t *lbl_unidad = lv_label_create(fila_valor);
        lv_label_set_text(lbl_unidad, unidad);
        lv_obj_set_style_text_color(lbl_unidad, COLOR_TITULO, 0);
    }

    return lbl_valor;
}

// Barra superior: titulo de la app + boton para ir a la vista de graficas.
static void crear_barra_superior(lv_obj_t *parent)
{
    lv_obj_t *barra = lv_obj_create(parent);
    lv_obj_remove_style_all(barra);
    lv_obj_set_size(barra, LV_PCT(100), 50);
    lv_obj_set_flex_flow(barra, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(barra, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_titulo = lv_label_create(barra);
    lv_label_set_text(lbl_titulo, "LabGeo");
    lv_obj_set_style_text_font(lbl_titulo, &lv_font_montserrat_28, 0);

    ui.btn_ver_grafica = lv_button_create(barra);
    lv_obj_set_size(ui.btn_ver_grafica, 180, 44);
    lv_obj_t *lbl_btn = lv_label_create(ui.btn_ver_grafica);
    lv_label_set_text(lbl_btn, LV_SYMBOL_IMAGE " Ver Grafica");
    lv_obj_center(lbl_btn);
}

// Columna izquierda (2/3 de la pantalla): corrida activa, grid de sensores
// y botones de control.
static void crear_columna_numeros(lv_obj_t *parent)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_PCT(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 14, 0);

    ui.lbl_corrida_activa = lv_label_create(col);
    lv_label_set_text(ui.lbl_corrida_activa, "Corrida activa: Primera Corrida");
    lv_obj_set_style_text_font(ui.lbl_corrida_activa, &lv_font_montserrat_20, 0);

    // Grid 2x2 de sensores (se lleva todo el alto sobrante)
    lv_obj_t *grid = lv_obj_create(col);
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

    ui.lbl_dial1 = crear_tarjeta_sensor(fila1, "Dial 1 - Deformacion", "0.0000", "mm");
    ui.lbl_dial2 = crear_tarjeta_sensor(fila1, "Dial 2 - Deformacion", "0.0000", "mm");
    ui.lbl_peso  = crear_tarjeta_sensor(fila2, "Peso / Carga", "0.0", "N");
    ui.lbl_tiempo = crear_tarjeta_sensor(fila2, "Tiempo Transcurrido", "00:00:00", "");

    // Botones de control del flujo de corridas
    lv_obj_t *fila_botones = lv_obj_create(col);
    lv_obj_remove_style_all(fila_botones);
    lv_obj_set_size(fila_botones, LV_PCT(100), 64);
    lv_obj_set_flex_flow(fila_botones, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fila_botones, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(fila_botones, 16, 0);

    ui.btn_iniciar = lv_button_create(fila_botones);
    lv_obj_set_size(ui.btn_iniciar, 260, 64);
    ui.lbl_btn_iniciar = lv_label_create(ui.btn_iniciar);
    lv_label_set_text(ui.lbl_btn_iniciar, "Iniciar Primera Corrida");
    lv_obj_set_style_text_font(ui.lbl_btn_iniciar, &lv_font_montserrat_14, 0);
    lv_obj_center(ui.lbl_btn_iniciar);

    ui.btn_siguiente = lv_button_create(fila_botones);
    lv_obj_set_size(ui.btn_siguiente, 180, 64);
    lv_obj_add_state(ui.btn_siguiente, LV_STATE_DISABLED);
    lv_obj_t *lbl_siguiente = lv_label_create(ui.btn_siguiente);
    lv_label_set_text(lbl_siguiente, "Siguiente " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(lbl_siguiente, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_siguiente);
}

// Columna derecha (1/3 de la pantalla): imagen del equipo encuadrada en una
// tarjeta blanca, igual que las de los sensores.
static void crear_columna_imagen(lv_obj_t *parent)
{
    lv_obj_t *card = crear_tarjeta_base(parent);
    lv_obj_set_size(card, ANCHO_COL_DERECHA, LV_PCT(100));
    lv_obj_set_style_pad_all(card, 12, 0);

    lv_obj_t *lbl_titulo = lv_label_create(card);
    lv_label_set_text(lbl_titulo, "Equipo de Ensayo");
    lv_obj_set_style_text_color(lbl_titulo, COLOR_TITULO, 0);

    lv_obj_t *img = lv_image_create(card);
    lv_image_set_src(img, &ui_img_prensa);
}

ui_labgeo_t *ui_labgeo_create(lv_obj_t *parent)
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

    lv_obj_t *cuerpo = lv_obj_create(ui.cont);
    lv_obj_remove_style_all(cuerpo);
    lv_obj_set_width(cuerpo, LV_PCT(100));
    lv_obj_set_flex_grow(cuerpo, 1);
    lv_obj_set_flex_flow(cuerpo, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cuerpo, 20, 0);

    crear_columna_numeros(cuerpo);
    crear_columna_imagen(cuerpo);

    return &ui;
}

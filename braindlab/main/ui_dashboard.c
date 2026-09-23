#include "ui_dashboard.h"
#include "ui_img_air_conditioner.h"

// ---------------------------------------------------------------------
// Vista "Inicio": dashboard de aires acondicionados + sensores + energia.
// Solo vista -- todos los valores de abajo son de ejemplo (los mismos del
// mockup). La logica que los actualice con datos reales todavia no existe;
// cuando se agregue, va a escribir sobre los handles guardados en
// ui_dashboard_t (ver ui_dashboard.h), igual que hace uart_labgeo.c con
// ui_labgeo_t en el proyecto controlador_labgeo/blink.
// ---------------------------------------------------------------------

#define COLOR_BG_ROOT        lv_color_hex(0x0A0E17)
#define COLOR_CARD_BG        lv_color_hex(0x111A2B)
#define COLOR_CARD_BORDER    lv_color_hex(0x1E2A3D)
#define COLOR_TITULO_SECCION lv_color_hex(0x4FA3F7)
#define COLOR_TEXTO_SEC      lv_color_hex(0x8A94A6)
#define COLOR_TEXTO_PRINC    lv_color_white()
#define COLOR_VALOR_AZUL     lv_color_hex(0x3B9EFF)
#define COLOR_VERDE          lv_color_hex(0x22C55E)
#define COLOR_ROJO           lv_color_hex(0xEF4444)
#define COLOR_NARANJA        lv_color_hex(0xF59E0B)
#define COLOR_NAV_ACTIVO     lv_color_hex(0x2563EB)
#define COLOR_GRIS_APAGADO   lv_color_hex(0x4B5563)

#define ANCHO_COL_DERECHA 230

static ui_dashboard_t ui;

// ---------- Helpers genericos ----------

static lv_obj_t *crear_tarjeta_base(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_style_bg_color(card, COLOR_CARD_BG, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, COLOR_CARD_BORDER, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    return card;
}

static lv_obj_t *crear_titulo_seccion(lv_obj_t *parent, const char *texto)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, texto);
    lv_obj_set_style_text_color(lbl, COLOR_TITULO_SECCION, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    return lbl;
}

static lv_obj_t *crear_led(lv_obj_t *parent, lv_color_t color, int size)
{
    lv_obj_t *led = lv_obj_create(parent);
    lv_obj_remove_style_all(led);
    lv_obj_set_size(led, size, size);
    lv_obj_set_style_radius(led, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(led, color, 0);
    lv_obj_set_style_bg_opa(led, LV_OPA_COVER, 0);
    return led;
}

// ---------- Barra superior ----------

static void crear_barra_superior(lv_obj_t *parent)
{
    lv_obj_t *barra = lv_obj_create(parent);
    lv_obj_remove_style_all(barra);
    lv_obj_set_size(barra, LV_PCT(100), 34);
    lv_obj_set_flex_flow(barra, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(barra, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Grupo izquierdo: menu + titulo
    lv_obj_t *grupo_titulo = lv_obj_create(barra);
    lv_obj_remove_style_all(grupo_titulo);
    lv_obj_set_size(grupo_titulo, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grupo_titulo, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grupo_titulo, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grupo_titulo, 10, 0);

    lv_obj_t *lbl_menu = lv_label_create(grupo_titulo);
    lv_label_set_text(lbl_menu, LV_SYMBOL_BARS);
    lv_obj_set_style_text_color(lbl_menu, COLOR_TEXTO_PRINC, 0);

    lv_obj_t *lbl_titulo = lv_label_create(grupo_titulo);
    lv_label_set_text(lbl_titulo, "SISTEMA DE MONITOREO");
    lv_obj_set_style_text_color(lbl_titulo, COLOR_TEXTO_PRINC, 0);
    lv_obj_set_style_text_font(lbl_titulo, &lv_font_montserrat_16, 0);

    // Grupo central: estado del sistema
    lv_obj_t *grupo_estado = lv_obj_create(barra);
    lv_obj_remove_style_all(grupo_estado);
    lv_obj_set_size(grupo_estado, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grupo_estado, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grupo_estado, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grupo_estado, 6, 0);

    ui.led_estado_sistema = crear_led(grupo_estado, COLOR_GRIS_APAGADO, 10);
    ui.lbl_estado_sistema = lv_label_create(grupo_estado);
    lv_label_set_text(ui.lbl_estado_sistema, "Esperando datos...");
    lv_obj_set_style_text_color(ui.lbl_estado_sistema, COLOR_TEXTO_PRINC, 0);

    // Grupo derecho: hora + fecha
    lv_obj_t *grupo_fecha_hora = lv_obj_create(barra);
    lv_obj_remove_style_all(grupo_fecha_hora);
    lv_obj_set_size(grupo_fecha_hora, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grupo_fecha_hora, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grupo_fecha_hora, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grupo_fecha_hora, 18, 0);

    ui.lbl_hora = lv_label_create(grupo_fecha_hora);
    lv_label_set_text(ui.lbl_hora, "--:--:--");
    lv_obj_set_style_text_color(ui.lbl_hora, COLOR_TEXTO_PRINC, 0);

    ui.lbl_fecha = lv_label_create(grupo_fecha_hora);
    lv_label_set_text(ui.lbl_fecha, "--/--/----");
    lv_obj_set_style_text_color(ui.lbl_fecha, COLOR_TEXTO_PRINC, 0);
}

// ---------- Aires acondicionados ----------

// Efecto de "viento": franjas debajo del equipo que titilan en cascada
// (opacidad 0 -> parcial -> 0, con delay creciente entre una y otra) para
// simular el aire fluyendo. Solo corren mientras el equipo esta encendido.
static void anim_opa_viento_cb(void *var, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void iniciar_anim_viento(ui_dash_aire_t *aire)
{
    for (int i = 0; i < UI_DASH_NUM_VIENTO; i++) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, aire->viento[i]);
        lv_anim_set_exec_cb(&a, anim_opa_viento_cb);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_70);
        lv_anim_set_time(&a, 450);
        lv_anim_set_playback_time(&a, 450);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_delay(&a, i * 150);
        lv_anim_start(&a);
    }
}

static void detener_anim_viento(ui_dash_aire_t *aire)
{
    for (int i = 0; i < UI_DASH_NUM_VIENTO; i++) {
        lv_anim_delete(aire->viento[i], anim_opa_viento_cb);
        lv_obj_set_style_bg_opa(aire->viento[i], LV_OPA_TRANSP, 0);
    }
}

// Aplica el estado ON/OFF (colores, icono, viento animado) -- compartido
// entre el toggle visual del boton y ui_dashboard_aire_set_encendido() (esta
// ultima la usa uart_braindlab.c para aplicar el estado real que llega por
// UART, ver ui_dashboard.h).
void ui_dashboard_aire_set_encendido(ui_dash_aire_t *aire, bool encendido)
{
    aire->encendido = encendido;

    if (encendido) {
        lv_obj_set_style_bg_color(aire->btn_power, COLOR_VERDE, 0);
        lv_label_set_text(aire->lbl_btn_power, LV_SYMBOL_POWER " ON");
        lv_obj_set_style_bg_color(aire->led_estado, COLOR_VERDE, 0);
        lv_obj_set_style_image_recolor_opa(aire->img_unidad, LV_OPA_TRANSP, 0);
        lv_obj_set_style_image_opa(aire->img_unidad, LV_OPA_COVER, 0);
        iniciar_anim_viento(aire);
    } else {
        lv_obj_set_style_bg_color(aire->btn_power, COLOR_GRIS_APAGADO, 0);
        lv_label_set_text(aire->lbl_btn_power, LV_SYMBOL_POWER " OFF");
        lv_obj_set_style_bg_color(aire->led_estado, COLOR_GRIS_APAGADO, 0);
        lv_obj_set_style_image_recolor(aire->img_unidad, COLOR_GRIS_APAGADO, 0);
        lv_obj_set_style_image_recolor_opa(aire->img_unidad, LV_OPA_70, 0);
        lv_obj_set_style_image_opa(aire->img_unidad, LV_OPA_50, 0);
        detener_anim_viento(aire);
    }
}

// Boton ON/OFF: alterna el estado visualmente de inmediato (respuesta
// rapida al toque); uart_braindlab.c se encarga de mandar el pedido real por
// UART (ver braindlab.c, donde se registra el callback que llama a
// uart_braindlab_enviar_control_aire()) y de corregir la vista si la
// automatica termina pisando el pedido (llega en el proximo ESTADO_UPDATE).
static void toggle_power_cb(lv_event_t *e)
{
    ui_dash_aire_t *aire = (ui_dash_aire_t *)lv_event_get_user_data(e);
    ui_dashboard_aire_set_encendido(aire, !aire->encendido);
}

static void crear_tarjeta_aire(lv_obj_t *parent, ui_dash_aire_t *aire, int numero,
                                const char *nombre, const char *temp, const char *set,
                                const char *velocidad)
{
    lv_obj_t *card = crear_tarjeta_base(parent);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_style_pad_row(card, 2, 0);   // comprimido, ver conversacion -- todo mas pegado (header, set/vel, boton)
    lv_obj_set_style_pad_top(card, 6, 0);   // menos aire arriba de "Aire N"
    lv_obj_set_style_pad_bottom(card, 6, 0); // menos aire debajo del boton

    // Cabecera: numero + nombre + led de estado
    lv_obj_t *fila_header = lv_obj_create(card);
    lv_obj_remove_style_all(fila_header);
    lv_obj_set_size(fila_header, LV_PCT(100), LV_SIZE_CONTENT); // sin alto explicito LVGL le da ~130px y "Aire N" queda a media tarjeta
    lv_obj_set_flex_flow(fila_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fila_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *grupo_id = lv_obj_create(fila_header);
    lv_obj_remove_style_all(grupo_id);
    lv_obj_set_size(grupo_id, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grupo_id, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grupo_id, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grupo_id, 6, 0);

    lv_obj_t *lbl_numero = lv_label_create(grupo_id);
    lv_label_set_text_fmt(lbl_numero, "%02d", numero);
    lv_obj_set_style_text_color(lbl_numero, COLOR_TEXTO_PRINC, 0);
    lv_obj_set_style_text_font(lbl_numero, &lv_font_montserrat_16, 0);

    aire->lbl_nombre = lv_label_create(grupo_id);
    lv_label_set_text(aire->lbl_nombre, nombre);
    lv_obj_set_style_text_color(aire->lbl_nombre, COLOR_TEXTO_SEC, 0);

    aire->led_estado = crear_led(fila_header, COLOR_VERDE, 10);

    // Icono del equipo (air-conditioner.png) + franjas de viento animadas
    lv_obj_t *caja_icono = lv_obj_create(card);
    lv_obj_set_style_bg_color(caja_icono, lv_color_hex(0x1A2740), 0);
    lv_obj_set_style_bg_opa(caja_icono, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(caja_icono, 8, 0);
    lv_obj_set_style_border_width(caja_icono, 0, 0);
    lv_obj_set_width(caja_icono, LV_PCT(100));
    // Alto flexible (no fijo): el icono es puramente decorativo, asi que es
    // el que se achica solo si a la tarjeta le falta espacio -- todo lo
    // demas (header/temp/set-vel/boton) es funcional y tiene que entrar
    // siempre. Antes tenia altura fija (46px) y el BOTON de encendido (el
    // ultimo hijo) era el que quedaba cortado/invisible cuando la tarjeta
    // resultaba un poco mas chica de lo calculado -- ver conversacion.
    lv_obj_set_flex_grow(caja_icono, 1);
    lv_obj_set_style_min_height(caja_icono, 40, 0);
    lv_obj_clear_flag(caja_icono, LV_OBJ_FLAG_SCROLLABLE);

    aire->img_unidad = lv_image_create(caja_icono);
    lv_image_set_src(aire->img_unidad, &air_conditioner);
    lv_obj_align(aire->img_unidad, LV_ALIGN_CENTER, 0, -4);

    for (int i = 0; i < UI_DASH_NUM_VIENTO; i++) {
        lv_obj_t *franja = lv_obj_create(caja_icono);
        lv_obj_remove_style_all(franja);
        lv_obj_set_size(franja, 44 - i * 10, 3);
        lv_obj_set_style_bg_color(franja, COLOR_VALOR_AZUL, 0);
        lv_obj_set_style_bg_opa(franja, LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(franja, LV_RADIUS_CIRCLE, 0);
        lv_obj_align(franja, LV_ALIGN_BOTTOM_MID, 0, -3 - i * 6);
        aire->viento[i] = franja;
    }

    // Temperatura actual
    lv_obj_t *fila_temp = lv_obj_create(card);
    lv_obj_remove_style_all(fila_temp);
    lv_obj_set_flex_flow(fila_temp, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fila_temp, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(fila_temp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(fila_temp, 2, 0);

    aire->lbl_temp_actual = lv_label_create(fila_temp);
    lv_label_set_text(aire->lbl_temp_actual, temp);
    lv_obj_set_style_text_color(aire->lbl_temp_actual, COLOR_VALOR_AZUL, 0);
    lv_obj_set_style_text_font(aire->lbl_temp_actual, &lv_font_montserrat_24, 0);

    lv_obj_t *lbl_grado = lv_label_create(fila_temp);
    lv_label_set_text(lbl_grado, "\xC2\xB0" "C");
    lv_obj_set_style_text_color(lbl_grado, COLOR_VALOR_AZUL, 0);

    // Setpoint + velocidad en UNA sola fila (antes eran 2 filas separadas --
    // no entraban, ver comentario de pad_row arriba).
    lv_obj_t *fila_set_vel = lv_obj_create(card);
    lv_obj_remove_style_all(fila_set_vel);
    lv_obj_set_size(fila_set_vel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fila_set_vel, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fila_set_vel, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_set_prefijo = lv_label_create(fila_set_vel);
    lv_obj_set_style_text_color(lbl_set_prefijo, COLOR_TEXTO_SEC, 0);
    aire->lbl_set = lbl_set_prefijo;
    lv_label_set_text_fmt(aire->lbl_set, "Set: %s\xC2\xB0" "C", set);

    aire->lbl_velocidad = lv_label_create(fila_set_vel);
    lv_label_set_text(aire->lbl_velocidad, velocidad);
    lv_obj_set_style_text_color(aire->lbl_velocidad, COLOR_TEXTO_PRINC, 0);

    // Boton ON/OFF
    aire->btn_power = lv_button_create(card);
    lv_obj_set_size(aire->btn_power, LV_PCT(100), 24); // comprimido, ver conversacion
    lv_obj_set_style_radius(aire->btn_power, 6, 0);
    aire->lbl_btn_power = lv_label_create(aire->btn_power);
    lv_obj_center(aire->lbl_btn_power);

    // Arranca APAGADO/gris -- no hay dato real todavia (recien llega con el
    // primer ESTADO_UPDATE por UART, ver uart_braindlab.c). Antes arrancaba
    // "encendido" a proposito visualmente, pero eso mostraba un estado
    // inventado antes de tener el dato real.
    ui_dashboard_aire_set_encendido(aire, false);
    lv_obj_add_event_cb(aire->btn_power, toggle_power_cb, LV_EVENT_CLICKED, aire);
}

static void crear_fila_aires(lv_obj_t *parent)
{
    lv_obj_t *fila = lv_obj_create(parent);
    lv_obj_remove_style_all(fila);
    lv_obj_set_width(fila, LV_PCT(100));
    lv_obj_set_flex_grow(fila, 1);
    lv_obj_set_flex_flow(fila, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(fila, 10, 0);

    // "temp"/"set"/"velocidad" quedan en "--" -- no hay dato real de setpoint
    // ni velocidad por unidad en el protocolo (solo ON/OFF, ver
    // uart_braindlab.c), asi que nunca se van a actualizar solos. Mostrar un
    // numero fijo ahi seria un dato inventado.
    crear_tarjeta_aire(fila, &ui.aires[0], 1, "Aire 1", "--", "--", "--");
    crear_tarjeta_aire(fila, &ui.aires[1], 2, "Aire 2", "--", "--", "--");
    crear_tarjeta_aire(fila, &ui.aires[2], 3, "Aire 3", "--", "--", "--");
    crear_tarjeta_aire(fila, &ui.aires[3], 4, "Aire 4", "--", "--", "--");
}

// ---------- Columna izquierda ----------

static void crear_columna_izquierda(lv_obj_t *parent)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_PCT(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 8, 0);

    lv_obj_t *titulo_aires = crear_titulo_seccion(col, "AIRES ACONDICIONADOS");
    lv_obj_set_width(titulo_aires, LV_PCT(100));

    crear_fila_aires(col);
}

// ---------- Sensores ambientales ----------

static void crear_tarjeta_sensor(lv_obj_t *parent, ui_dash_sensor_t *sensor,
                                  const char *nombre, const char *valor)
{
    lv_obj_t *card = crear_tarjeta_base(parent);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_height(card, LV_PCT(100));

    sensor->lbl_nombre = lv_label_create(card);
    lv_label_set_text(sensor->lbl_nombre, nombre);
    lv_obj_set_style_text_color(sensor->lbl_nombre, COLOR_TEXTO_SEC, 0);

    sensor->lbl_valor = lv_label_create(card);
    lv_label_set_text(sensor->lbl_valor, valor);
    lv_obj_set_style_text_color(sensor->lbl_valor, COLOR_TEXTO_PRINC, 0);
    lv_obj_set_style_text_font(sensor->lbl_valor, &lv_font_montserrat_16, 0);
}

static void crear_panel_sensores(lv_obj_t *parent)
{
    lv_obj_t *titulo = crear_titulo_seccion(parent, "SENSORES AMBIENTALES");
    lv_obj_set_width(titulo, LV_PCT(100));

    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_set_height(grid, 110); // comprimido, ver conversacion (T1-T4 no necesitan tanto alto, solo un numero chico)

    lv_obj_t *fila1 = lv_obj_create(grid);
    lv_obj_remove_style_all(fila1);
    lv_obj_set_width(fila1, LV_PCT(100));
    lv_obj_set_flex_grow(fila1, 1);
    lv_obj_set_flex_flow(fila1, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(fila1, 8, 0);

    lv_obj_t *fila2 = lv_obj_create(grid);
    lv_obj_remove_style_all(fila2);
    lv_obj_set_width(fila2, LV_PCT(100));
    lv_obj_set_flex_grow(fila2, 1);
    lv_obj_set_flex_flow(fila2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(fila2, 8, 0);

    crear_tarjeta_sensor(fila1, &ui.sensores[0], "T1", "--");
    crear_tarjeta_sensor(fila1, &ui.sensores[1], "T2", "--");
    crear_tarjeta_sensor(fila2, &ui.sensores[2], "T3", "--");
    crear_tarjeta_sensor(fila2, &ui.sensores[3], "T4", "--");
}

// Una columna "titulo chico + valor grande" -- helper compartido por
// temperatura y humedad del gabinete, mismo par de labels en los dos casos.
static lv_obj_t *crear_columna_valor(lv_obj_t *parent, const char *titulo, const char *valor_inicial)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_titulo = lv_label_create(col);
    lv_label_set_text(lbl_titulo, titulo);
    lv_obj_set_style_text_color(lbl_titulo, COLOR_TEXTO_SEC, 0);

    lv_obj_t *lbl_valor = lv_label_create(col);
    lv_label_set_text(lbl_valor, valor_inicial);
    lv_obj_set_style_text_color(lbl_valor, COLOR_VALOR_AZUL, 0);
    lv_obj_set_style_text_font(lbl_valor, &lv_font_montserrat_24, 0);

    return lbl_valor;
}

// Temperatura + humedad del gabinete (AM2301A, ver sensor_gestor.c en
// controlador_braindlab) -- antes esta tarjeta solo mostraba humedad, la
// temperatura llegaba por UART pero no tenia widget propio.
static void crear_panel_gabinete(lv_obj_t *parent)
{
    lv_obj_t *titulo = crear_titulo_seccion(parent, "GABINETE");
    lv_obj_set_width(titulo, LV_PCT(100));

    lv_obj_t *card = crear_tarjeta_base(parent);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ui.lbl_temp_gestor = crear_columna_valor(card, "Temp.", "--\xC2\xB0" "C");
    ui.lbl_humedad = crear_columna_valor(card, "Humedad", "-- %RH");
}

// ---------- Alarmas activas ----------

static lv_obj_t *crear_fila_alarma(lv_obj_t *parent, const char *codigo, const char *desc, lv_color_t color)
{
    lv_obj_t *fila = lv_obj_create(parent);
    lv_obj_set_style_bg_color(fila, lv_color_hex(0x1A1420), 0);
    lv_obj_set_style_bg_opa(fila, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(fila, 8, 0);
    lv_obj_set_style_border_width(fila, 1, 0);
    lv_obj_set_style_border_color(fila, color, 0);
    lv_obj_set_style_pad_all(fila, 8, 0);
    lv_obj_clear_flag(fila, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(fila, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fila, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fila, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(fila, 8, 0);

    lv_obj_t *grupo = lv_obj_create(fila);
    lv_obj_remove_style_all(grupo);
    lv_obj_set_size(grupo, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grupo, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grupo, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grupo, 8, 0);

    lv_obj_t *lbl_icono = lv_label_create(grupo);
    lv_label_set_text(lbl_icono, LV_SYMBOL_WARNING);
    lv_obj_set_style_text_color(lbl_icono, color, 0);

    lv_obj_t *col_texto = lv_obj_create(grupo);
    lv_obj_remove_style_all(col_texto);
    lv_obj_set_size(col_texto, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col_texto, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *lbl_codigo = lv_label_create(col_texto);
    lv_label_set_text(lbl_codigo, codigo);
    lv_obj_set_style_text_color(lbl_codigo, color, 0);

    lv_obj_t *lbl_desc = lv_label_create(col_texto);
    lv_label_set_text(lbl_desc, desc);
    lv_obj_set_style_text_color(lbl_desc, COLOR_TEXTO_SEC, 0);

    lv_obj_t *lbl_chevron = lv_label_create(fila);
    lv_label_set_text(lbl_chevron, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(lbl_chevron, COLOR_TEXTO_SEC, 0);

    return fila;
}

static void crear_panel_alarmas(lv_obj_t *parent)
{
    lv_obj_t *titulo = crear_titulo_seccion(parent, "ALARMAS ACTIVAS");
    lv_obj_set_style_text_color(titulo, COLOR_ROJO, 0);
    lv_obj_set_width(titulo, LV_PCT(100));

    ui.cont_alarmas = lv_obj_create(parent);
    lv_obj_remove_style_all(ui.cont_alarmas);
    lv_obj_set_width(ui.cont_alarmas, LV_PCT(100));
    lv_obj_set_flex_grow(ui.cont_alarmas, 1);
    lv_obj_set_flex_flow(ui.cont_alarmas, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ui.cont_alarmas, 8, 0);

    ui.fila_alarma_at  = crear_fila_alarma(ui.cont_alarmas, "AT", "Alta Temperatura", COLOR_ROJO);
    ui.fila_alarma_bps = crear_fila_alarma(ui.cont_alarmas, "BPS", "Bypass de Aires", COLOR_NARANJA);
    // Arrancan ocultas -- uart_braindlab.c las muestra segun el estado real
    // (alarma_at/bypass_activo) que llegue por UART.
    lv_obj_add_flag(ui.fila_alarma_at, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui.fila_alarma_bps, LV_OBJ_FLAG_HIDDEN);
}

// ---------- Controles Bypass / AT ----------
//
// Mismo patron que el boton de encendido de cada aire (toggle_power_cb +
// ui_dashboard_aire_set_encendido()): el toque cambia el color/texto de
// inmediato (respuesta rapida), y quien registra el callback que manda el
// pedido real por UART es braindlab.c (uart_braindlab_enviar_control_bypass/
// _at()) -- registrado DESPUES de este, mismo motivo que con los aires (para
// que lea el valor ya actualizado). uart_braindlab.c corrige la vista con el
// estado real (bypass_solicitado/alarma_at) en el proximo ESTADO_UPDATE.
// Distintos de fila_alarma_at/fila_alarma_bps (arriba): esas son
// indicadores de solo lectura que aparecen/desaparecen segun haya alarma de
// verdad, estos son botones fijos siempre visibles para forzar el pedido a
// mano.

void ui_dashboard_bypass_set_activo(ui_dashboard_t *dash, bool activo)
{
    dash->bypass_activo = activo;
    lv_obj_set_style_bg_color(dash->btn_bypass, activo ? COLOR_NARANJA : COLOR_GRIS_APAGADO, 0);
    lv_label_set_text_fmt(dash->lbl_btn_bypass, "Bypass " LV_SYMBOL_POWER " %s", activo ? "ON" : "OFF");
}

void ui_dashboard_at_set_activo(ui_dashboard_t *dash, bool activo)
{
    dash->at_activo = activo;
    lv_obj_set_style_bg_color(dash->btn_at, activo ? COLOR_ROJO : COLOR_GRIS_APAGADO, 0);
    lv_label_set_text_fmt(dash->lbl_btn_at, "AT " LV_SYMBOL_POWER " %s", activo ? "ON" : "OFF");
}

static void toggle_bypass_cb(lv_event_t *e)
{
    ui_dashboard_bypass_set_activo(&ui, !ui.bypass_activo);
}

static void toggle_at_cb(lv_event_t *e)
{
    ui_dashboard_at_set_activo(&ui, !ui.at_activo);
}

static lv_obj_t *crear_boton_control(lv_obj_t *parent, lv_obj_t **out_lbl)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, 30); // comprimido -- la columna derecha no tenia lugar para este panel entero, ver conversacion
    lv_obj_set_style_radius(btn, 6, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_center(lbl);
    *out_lbl = lbl;

    return btn;
}

static void crear_panel_controles(lv_obj_t *parent)
{
    lv_obj_t *titulo = crear_titulo_seccion(parent, "CONTROLES");
    lv_obj_set_width(titulo, LV_PCT(100));

    lv_obj_t *fila = lv_obj_create(parent);
    lv_obj_remove_style_all(fila);
    lv_obj_set_size(fila, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(fila, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(fila, 8, 0);

    ui.btn_bypass = crear_boton_control(fila, &ui.lbl_btn_bypass);
    ui.btn_at     = crear_boton_control(fila, &ui.lbl_btn_at);

    // Arrancan OFF/gris -- no hay dato real todavia (llega con el primer
    // ESTADO_UPDATE), mismo criterio que ui_dashboard_aire_set_encendido()
    // en crear_tarjeta_aire().
    ui_dashboard_bypass_set_activo(&ui, false);
    ui_dashboard_at_set_activo(&ui, false);

    lv_obj_add_event_cb(ui.btn_bypass, toggle_bypass_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui.btn_at, toggle_at_cb, LV_EVENT_CLICKED, NULL);
}

// ---------- Columna derecha ----------

static void crear_columna_derecha(lv_obj_t *parent)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, ANCHO_COL_DERECHA, LV_PCT(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 4, 0); // comprimido, ver conversacion (4 paneles ahora, antes eran 3)

    crear_panel_sensores(col);
    crear_panel_gabinete(col);
    crear_panel_controles(col);
    crear_panel_alarmas(col);
}

// ---------- Barra inferior de navegacion ----------

static lv_obj_t *crear_boton_nav(lv_obj_t *parent, const char *icono, const char *texto, bool activo)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, LV_PCT(100));
    lv_obj_set_style_radius(btn, 8, 0);
    if (activo) {
        lv_obj_set_style_bg_color(btn, COLOR_NAV_ACTIVO, 0);
    } else {
        lv_obj_set_style_bg_color(btn, COLOR_CARD_BG, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, COLOR_CARD_BORDER, 0);
    }

    lv_obj_t *col = lv_obj_create(btn);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(col, 6, 0);
    lv_obj_center(col);

    lv_obj_t *lbl_icono = lv_label_create(col);
    lv_label_set_text(lbl_icono, icono);
    lv_obj_set_style_text_color(lbl_icono, COLOR_TEXTO_PRINC, 0);

    lv_obj_t *lbl_texto = lv_label_create(col);
    lv_label_set_text(lbl_texto, texto);
    lv_obj_set_style_text_color(lbl_texto, COLOR_TEXTO_PRINC, 0);

    return btn;
}

static void crear_barra_inferior(lv_obj_t *parent)
{
    lv_obj_t *barra = lv_obj_create(parent);
    lv_obj_remove_style_all(barra);
    lv_obj_set_size(barra, LV_PCT(100), 46);
    lv_obj_set_flex_flow(barra, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(barra, 8, 0);

    ui.btn_nav_inicio      = crear_boton_nav(barra, LV_SYMBOL_HOME, "INICIO", true);
    ui.btn_nav_graficas    = crear_boton_nav(barra, LV_SYMBOL_IMAGE, "GRAFICAS", false);
    ui.btn_nav_ajustes     = crear_boton_nav(barra, LV_SYMBOL_SETTINGS, "AJUSTES", false);
    ui.btn_nav_red         = crear_boton_nav(barra, LV_SYMBOL_WIFI, "RED", false);
    ui.btn_nav_fecha_hora  = crear_boton_nav(barra, LV_SYMBOL_LOOP, "FECHA/HORA", false);
}

// ---------- Ensamblado ----------

ui_dashboard_t *ui_dashboard_create(lv_obj_t *parent)
{
    ui.cont = lv_obj_create(parent);
    lv_obj_remove_style_all(ui.cont);
    lv_obj_set_size(ui.cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(ui.cont, COLOR_BG_ROOT, 0);
    lv_obj_set_style_bg_opa(ui.cont, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(ui.cont, 12, 0);
    lv_obj_set_style_pad_row(ui.cont, 8, 0);
    lv_obj_clear_flag(ui.cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ui.cont, LV_FLEX_FLOW_COLUMN);

    crear_barra_superior(ui.cont);

    lv_obj_t *cuerpo = lv_obj_create(ui.cont);
    lv_obj_remove_style_all(cuerpo);
    lv_obj_set_width(cuerpo, LV_PCT(100));
    lv_obj_set_flex_grow(cuerpo, 1);
    lv_obj_set_flex_flow(cuerpo, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cuerpo, 10, 0);

    crear_columna_izquierda(cuerpo);
    crear_columna_derecha(cuerpo);

    crear_barra_inferior(ui.cont);

    return &ui;
}

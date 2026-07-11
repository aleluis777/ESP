#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "ui_labgeo.h"
#include "ui_grafica.h"
#include "uart_labgeo.h"

static const char *TAG = "PANEL";

#define LCD_H_RES 800
#define LCD_V_RES 480
#define PIN_BL    GPIO_NUM_2

static ui_labgeo_t *ui;
static ui_grafica_t *grafica;

// ---------- Estado del ensayo: dos corridas consecutivas ----------
typedef enum {
    CORRIDA_PRIMERA = 1,
    CORRIDA_SEGUNDA = 2,
} corrida_t;

static corrida_t corrida_actual = CORRIDA_PRIMERA;
static bool corriendo = false;

// Vuelve los 4 sensores a su estado inicial (nueva corrida). Es solo un
// placeholder visual: en cuanto el controlador empiece a mandar
// LABGEO_CMD_SENSOR_UPDATE, uart_labgeo.c pisa estos valores con los reales.
static void reset_sensores(void)
{
    lv_label_set_text(ui->lbl_dial1, "0.000");
    lv_label_set_text(ui->lbl_dial2, "0.000");
    lv_label_set_text(ui->lbl_peso, "0.0");
    lv_label_set_text(ui->lbl_tiempo, "00:00:00");
}

// Boton principal: inicia o detiene la corrida activa. El tiempo y los 4
// sensores los maneja el controlador (llegan por UART); aca solo se manda
// la orden de start/stop y se actualiza el estado de los botones.
static void iniciar_detener_cb(lv_event_t *e)
{
    corriendo = !corriendo;

    if (corriendo) {
        ESP_LOGI(TAG, "Corrida %d iniciada", (int)corrida_actual);
        uart_labgeo_enviar_start((uint8_t)corrida_actual);
        lv_label_set_text(ui->lbl_btn_iniciar, "Detener Corrida");
        lv_obj_add_state(ui->btn_siguiente, LV_STATE_DISABLED);
        return;
    }

    ESP_LOGI(TAG, "Corrida %d detenida", (int)corrida_actual);
    uart_labgeo_enviar_stop((uint8_t)corrida_actual);
    if (corrida_actual == CORRIDA_PRIMERA) {
        lv_label_set_text(ui->lbl_btn_iniciar, "Primera Corrida Finalizada");
        lv_obj_add_state(ui->btn_iniciar, LV_STATE_DISABLED);
        lv_obj_remove_state(ui->btn_siguiente, LV_STATE_DISABLED);
    } else {
        lv_label_set_text(ui->lbl_btn_iniciar, "Ensayo Finalizado");
        lv_obj_add_state(ui->btn_iniciar, LV_STATE_DISABLED);
    }
}

// Boton "Siguiente": pasa de la Primera a la Segunda Corrida.
static void siguiente_cb(lv_event_t *e)
{
    corrida_actual = CORRIDA_SEGUNDA;
    reset_sensores();

    lv_label_set_text(ui->lbl_corrida_activa, "Corrida activa: Segunda Corrida");
    lv_label_set_text(ui->lbl_btn_iniciar, "Iniciar Segunda Corrida");
    lv_obj_remove_state(ui->btn_iniciar, LV_STATE_DISABLED);
    lv_obj_add_state(ui->btn_siguiente, LV_STATE_DISABLED);

    ESP_LOGI(TAG, "Avanzando a la Segunda Corrida");
}

// ---------- Navegacion entre vistas (dashboard <-> grafica) ----------
static void ver_grafica_cb(lv_event_t *e)
{
    lv_obj_add_flag(ui->cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(grafica->cont, LV_OBJ_FLAG_HIDDEN);
    uart_labgeo_enviar_request_run((uint8_t)corrida_actual);   // muestra la corrida activa por defecto
}

static void volver_dashboard_cb(lv_event_t *e)
{
    lv_obj_add_flag(grafica->cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui->cont, LV_OBJ_FLAG_HIDDEN);
}

// Botones "Corrida 1" / "Corrida 2" dentro de la vista de graficas: piden
// los datos guardados de esa corrida (CMD_REQUEST_RUN) para graficarlos.
static void ver_corrida1_cb(lv_event_t *e)
{
    uart_labgeo_enviar_request_run((uint8_t)CORRIDA_PRIMERA);
}

static void ver_corrida2_cb(lv_event_t *e)
{
    uart_labgeo_enviar_request_run((uint8_t)CORRIDA_SEGUNDA);
}

void app_main(void)
{
    // ---------- 1. Panel RGB ----------
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_width = 16,
        // .psram_trans_align = 64,
        .num_fbs = 1,
        .de_gpio_num = GPIO_NUM_41,
        .vsync_gpio_num = GPIO_NUM_40,
        .hsync_gpio_num = GPIO_NUM_39,
        .pclk_gpio_num = GPIO_NUM_42,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            GPIO_NUM_15, GPIO_NUM_7,  GPIO_NUM_6,  GPIO_NUM_5,  GPIO_NUM_4,
            GPIO_NUM_9,  GPIO_NUM_46, GPIO_NUM_3,  GPIO_NUM_8,  GPIO_NUM_16, GPIO_NUM_1,
            GPIO_NUM_14, GPIO_NUM_21, GPIO_NUM_47, GPIO_NUM_48, GPIO_NUM_45,
        },
        .timings = {
            .pclk_hz = 10000000,     // Bajamos a 10 MHz para estabilidad absoluta y cero parpadeos
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_back_porch = 40,  // Centra la pantalla horizontalmente
            .hsync_front_porch = 40,
            .hsync_pulse_width = 48,
            .vsync_back_porch = 32,  // Amarra la sincronización vertical para que no se mueva
            .vsync_front_porch = 13,
            .vsync_pulse_width = 3,
        },
        .flags.fb_in_psram = true,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    // Backlight
    gpio_reset_pin(PIN_BL);
    gpio_set_direction(PIN_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BL, 1);

    // ---------- 2. Tactil GT911 (I2C: SDA 19, SCL 20, RST 38) ----------
    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t i2c_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = 0,
        .sda_io_num = GPIO_NUM_19,
        .scl_io_num = GPIO_NUM_20,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &i2c_bus));

    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io));

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_38,
        .int_gpio_num = GPIO_NUM_NC,   // INT no conectado en la S070
    };
    esp_lcd_touch_handle_t tp = NULL;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp));

    // ---------- 3. LVGL port (crea la tarea de UI en el nucleo 1) ----------
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_affinity = 1;        // UI en el nucleo 1
    lvgl_cfg.task_stack = 16384;       // UI con varios contenedores flex anidados necesita mas stack que el default (7168)
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * 100,        // buffer de dibujo (100 lineas)
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = false, .buff_spiram = true },
    };
    lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = { .bb_mode = false, .avoid_tearing = false },
    };
    lv_display_t *disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);

    lvgl_port_touch_cfg_t touch_cfg = { .disp = disp, .handle = tp };
    lvgl_port_add_touch(&touch_cfg);

    // ---------- 4. La interfaz (siempre entre lock/unlock) ----------
    if (lvgl_port_lock(0)) {
        ui = ui_labgeo_create(lv_screen_active());
        grafica = ui_grafica_create(lv_screen_active());
        lv_obj_add_flag(grafica->cont, LV_OBJ_FLAG_HIDDEN);   // arranca en el dashboard

        lv_obj_add_event_cb(ui->btn_iniciar, iniciar_detener_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(ui->btn_siguiente, siguiente_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(ui->btn_ver_grafica, ver_grafica_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(grafica->btn_volver, volver_dashboard_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(grafica->btn_corrida1, ver_corrida1_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(grafica->btn_corrida2, ver_corrida2_cb, LV_EVENT_CLICKED, NULL);

        lvgl_port_unlock();
    }

    // ---------- 5. Enlace UART con el controlador de sensores ----------
    uart_labgeo_init(ui, grafica);

    ESP_LOGI(TAG, "UI lista. app_main libre para tu logica en el nucleo 0");
}
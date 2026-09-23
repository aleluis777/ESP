#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_lvgl_port.h"
#include "ui_dashboard.h"
#include "ui_energia.h"
#include "uart_braindlab.h"

static const char *TAG = "BRAINDLAB";

// Panel + touch identicos a los de "blink" (misma placa, ESP32-S3 + LCD RGB
// 800x480 S070 + tactil GT911 por I2C). Si braindlab termina en otro
// hardware, ajustar esta seccion junto con sdkconfig.defaults.
#define LCD_H_RES 800
#define LCD_V_RES 480
#define PIN_BL    GPIO_NUM_2

static ui_dashboard_t *ui;
static ui_energia_t *energia;

// Placeholder de navegacion para las vistas que todavia no existen
// (Ajustes, Red, Fecha/Hora). Cuando se agregue cada una esto pasa a
// ocultar ui->cont y mostrar la vista correspondiente, igual que
// ver_energia_cb/volver_dashboard_cb de abajo.
static void nav_pendiente_cb(lv_event_t *e)
{
    const char *nombre = (const char *)lv_event_get_user_data(e);
    ESP_LOGI(TAG, "Nav '%s': vista todavia no implementada", nombre);
}

// ---------- Navegacion entre vistas (dashboard <-> parametros electricos) ----------
static void ver_energia_cb(lv_event_t *e)
{
    lv_obj_add_flag(ui->cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(energia->cont, LV_OBJ_FLAG_HIDDEN);
}

static void volver_dashboard_cb(lv_event_t *e)
{
    lv_obj_add_flag(energia->cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(ui->cont, LV_OBJ_FLAG_HIDDEN);
}

// El boton de cada tarjeta de aire ya se alterna visualmente solo
// (toggle_power_cb en ui_dashboard.c, respuesta inmediata al toque); esto
// solo manda el pedido real por UART. 'indice' llega como user_data
// (0..UI_DASH_NUM_AIRES-1) -- se registra DESPUES del toggle visual, asi que
// para cuando esto corre 'aires[indice].encendido' ya tiene el valor nuevo.
static void enviar_control_aire_cb(lv_event_t *e)
{
    int indice = (int)(intptr_t)lv_event_get_user_data(e);
    uart_braindlab_enviar_control_aire((uint8_t)indice, ui->aires[indice].encendido);
}

// Mismo criterio que enviar_control_aire_cb: el toggle visual ya corrio
// (toggle_bypass_cb/toggle_at_cb en ui_dashboard.c, registrados ANTES que
// esto), asi que ui->bypass_activo/ui->at_activo ya tienen el valor nuevo
// para cuando esto se ejecuta.
static void enviar_control_bypass_cb(lv_event_t *e)
{
    uart_braindlab_enviar_control_bypass(ui->bypass_activo);
}

static void enviar_control_at_cb(lv_event_t *e)
{
    uart_braindlab_enviar_control_at(ui->at_activo);
}

void app_main(void)
{
    // ---------- 1. Panel RGB ----------
    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .data_width = 16,
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
            .pclk_hz = 10000000,
            .h_res = LCD_H_RES,
            .v_res = LCD_V_RES,
            .hsync_back_porch = 40,
            .hsync_front_porch = 40,
            .hsync_pulse_width = 48,
            .vsync_back_porch = 32,
            .vsync_front_porch = 13,
            .vsync_pulse_width = 3,
        },
        .flags.fb_in_psram = true,
        // El DMA lee de dos buffers chicos en SRAM interna (que el CPU rellena
        // desde el framebuffer en PSRAM) en vez de leer la PSRAM directo:
        // evita que la imagen se corra a un lado cuando el CPU compite por la
        // PSRAM al redibujar. 10 lineas por bounce buffer x 2.
        .bounce_buffer_size_px = LCD_H_RES * 10,
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
        .int_gpio_num = GPIO_NUM_NC,
    };
    esp_lcd_touch_handle_t tp = NULL;
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &tp));

    // ---------- 3. LVGL port ----------
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_affinity = 1;
    lvgl_cfg.task_stack = 16384;   // el dashboard tiene varios contenedores flex anidados
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = panel,
        .buffer_size = LCD_H_RES * 30,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = true, .buff_spiram = false }, // buffer de dibujo en SRAM interna, no en PSRAM
    };
    lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = { .bb_mode = false, .avoid_tearing = false },
    };
    lv_display_t *disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);

    lvgl_port_touch_cfg_t touch_cfg = { .disp = disp, .handle = tp };
    lvgl_port_add_touch(&touch_cfg);

    // ---------- 4. La interfaz ----------
    if (lvgl_port_lock(0)) {
        ui = ui_dashboard_create(lv_screen_active());
        energia = ui_energia_create(lv_screen_active());
        lv_obj_add_flag(energia->cont, LV_OBJ_FLAG_HIDDEN);   // arranca en el dashboard

        lv_obj_add_event_cb(ui->btn_nav_graficas, ver_energia_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(energia->btn_volver, volver_dashboard_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(ui->btn_nav_ajustes, nav_pendiente_cb, LV_EVENT_CLICKED, "AJUSTES");
        lv_obj_add_event_cb(ui->btn_nav_red, nav_pendiente_cb, LV_EVENT_CLICKED, "RED");
        lv_obj_add_event_cb(ui->btn_nav_fecha_hora, nav_pendiente_cb, LV_EVENT_CLICKED, "FECHA/HORA");

        // Un callback mas por boton de aire (ademas del toggle visual que ya
        // registra ui_dashboard.c) -- este manda el pedido real por UART.
        for (int i = 0; i < UI_DASH_NUM_AIRES; i++) {
            lv_obj_add_event_cb(ui->aires[i].btn_power, enviar_control_aire_cb, LV_EVENT_CLICKED,
                                 (void *)(intptr_t)i);
        }

        // Igual que arriba: un callback mas encima del toggle visual que ya
        // registra ui_dashboard.c, este manda el pedido real por UART.
        lv_obj_add_event_cb(ui->btn_bypass, enviar_control_bypass_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(ui->btn_at, enviar_control_at_cb, LV_EVENT_CLICKED, NULL);

        lvgl_port_unlock();
    }

    // ---------- 5. Enlace UART con controlador_braindlab ----------
    uart_braindlab_init(ui);

    ESP_LOGI(TAG, "UI lista, UART con controlador_braindlab activo");
}

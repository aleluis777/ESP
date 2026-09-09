#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "protocolo_braindlab.h"
#include "uart_braindlab.h"

static const char *TAG = "UART_BRAINDLAB";

// Ver comentario de pines en uart_braindlab.h.
#define BRAINDLAB_UART_PORT   UART_NUM_1
#define BRAINDLAB_UART_TX_PIN GPIO_NUM_17
#define BRAINDLAB_UART_RX_PIN GPIO_NUM_18
#define BRAINDLAB_UART_BAUD   115200

#define COLOR_VERDE    lv_color_hex(0x22C55E)
#define COLOR_ROJO     lv_color_hex(0xEF4444)
#define COLOR_NARANJA  lv_color_hex(0xF59E0B)

static ui_dashboard_t *s_ui = NULL;

// ---------- Envio de tramas ----------

static void enviar_frame(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    uint8_t buf[4 + BRAINDLAB_MAX_PAYLOAD + 1];
    uint16_t i = 0;

    buf[i++] = BRAINDLAB_SOF;
    buf[i++] = cmd;
    buf[i++] = (uint8_t)(len & 0xFF);
    buf[i++] = (uint8_t)((len >> 8) & 0xFF);
    if (len > 0) {
        memcpy(&buf[i], payload, len);
        i += len;
    }
    buf[i++] = braindlab_crc8(&buf[1], (uint16_t)(3 + len)); // CMD+LEN+PAYLOAD, sin el SOF

    uart_write_bytes(BRAINDLAB_UART_PORT, (const char *)buf, i);
}

void uart_braindlab_enviar_control_aire(uint8_t indice, bool encendido)
{
    ESP_LOGI(TAG, "-> CONTROL_AIRE indice=%u encendido=%u", (unsigned)indice, (unsigned)encendido);
    uint8_t payload[2] = { indice, encendido ? 1 : 0 };
    enviar_frame(BRAINDLAB_CMD_CONTROL_AIRE, payload, sizeof(payload));
}

void uart_braindlab_enviar_control_bypass(bool solicitado)
{
    ESP_LOGI(TAG, "-> CONTROL_BYPASS solicitado=%u", (unsigned)solicitado);
    uint8_t payload[1] = { solicitado ? 1 : 0 };
    enviar_frame(BRAINDLAB_CMD_CONTROL_BYPASS, payload, sizeof(payload));
}

void uart_braindlab_enviar_control_at(bool activa)
{
    ESP_LOGI(TAG, "-> CONTROL_AT activa=%u", (unsigned)activa);
    uint8_t payload[1] = { activa ? 1 : 0 };
    enviar_frame(BRAINDLAB_CMD_CONTROL_AT, payload, sizeof(payload));
}

void uart_braindlab_enviar_control_automatico(void)
{
    ESP_LOGI(TAG, "-> CONTROL_AUTOMATICO");
    enviar_frame(BRAINDLAB_CMD_CONTROL_AUTOMATICO, NULL, 0);
}

// ---------- Aplicar datos recibidos a la UI ----------

// "235" (decimas) -> "23.5<sufijo>". Con snprintf de <stdio.h> (no el de
// LVGL) a proposito: CONFIG_LV_USE_FLOAT esta apagado en este proyecto (ver
// sdkconfig), igual que en blink -- ver comentario de formatear_milesimas()
// en uart_labgeo.c para el detalle de por que.
static void formatear_decimas(char *buf, size_t buf_len, int16_t valor_decimas, const char *sufijo)
{
    bool negativo = valor_decimas < 0;
    int v = negativo ? -(int)valor_decimas : (int)valor_decimas;
    snprintf(buf, buf_len, "%s%d.%d%s", negativo ? "-" : "", v / 10, v % 10, sufijo);
}

static void aplicar_estado_update(const uint8_t *p, uint16_t len)
{
    if (len < BRAINDLAB_ESTADO_UPDATE_LEN || !s_ui) {
        ESP_LOGW(TAG, "ESTADO_UPDATE invalido (len=%u)", (unsigned)len);
        return;
    }

    uint8_t ciclo             = p[0];
    int16_t temps_decimas[4]  = { braindlab_leer_i16(&p[1]), braindlab_leer_i16(&p[3]),
                                  braindlab_leer_i16(&p[5]), braindlab_leer_i16(&p[7]) };
    uint8_t bitmask_salida    = p[9];
    uint8_t indice_reserva    = p[11];
    bool alarma_at            = p[12] != 0;
    bool bypass_activo        = p[16] != 0;
    int16_t humedad_decimas   = braindlab_leer_i16(&p[22]);
    uint16_t anio             = braindlab_leer_u16(&p[28]);
    uint8_t mes = p[30], dia = p[31], hora = p[32], minuto = p[33], segundo = p[34];
    (void) indice_reserva; // sin widget propio en el dashboard todavia

    if (!lvgl_port_lock(0)) {
        return;
    }

    // ---- Aires (encendido/apagado real) ----
    for (int i = 0; i < UI_DASH_NUM_AIRES; i++) {
        ui_dashboard_aire_set_encendido(&s_ui->aires[i], (bitmask_salida >> i) & 1);
    }

    // ---- Sensores ambientales (T1-T4) ----
    for (int i = 0; i < UI_DASH_NUM_SENSORES; i++) {
        char texto[16];
        formatear_decimas(texto, sizeof(texto), temps_decimas[i], "\xC2\xB0" "C");
        lv_label_set_text(s_ui->sensores[i].lbl_valor, texto);
    }

    // ---- Humedad del gabinete ----
    char texto_hum[16];
    formatear_decimas(texto_hum, sizeof(texto_hum), humedad_decimas, " %RH");
    lv_label_set_text(s_ui->lbl_humedad, texto_hum);

    // ---- Estado del sistema (barra superior) ----
    if (alarma_at) {
        lv_obj_set_style_bg_color(s_ui->led_estado_sistema, COLOR_ROJO, 0);
        lv_label_set_text(s_ui->lbl_estado_sistema, "Alta Temperatura");
    } else if (ciclo == 3) { // CLIMA_BYPASS, ver climatizacion.h de controlador_braindlab
        lv_obj_set_style_bg_color(s_ui->led_estado_sistema, COLOR_NARANJA, 0);
        lv_label_set_text(s_ui->lbl_estado_sistema, "Bypass Activo");
    } else {
        lv_obj_set_style_bg_color(s_ui->led_estado_sistema, COLOR_VERDE, 0);
        lv_label_set_text(s_ui->lbl_estado_sistema, "Sistema OK");
    }

    // ---- Alarmas activas (se muestran solo si estan realmente activas) ----
    if (alarma_at) {
        lv_obj_remove_flag(s_ui->fila_alarma_at, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui->fila_alarma_at, LV_OBJ_FLAG_HIDDEN);
    }
    if (bypass_activo) {
        lv_obj_remove_flag(s_ui->fila_alarma_bps, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui->fila_alarma_bps, LV_OBJ_FLAG_HIDDEN);
    }

    // ---- Hora / fecha ----
    if (anio == 0) {
        lv_label_set_text(s_ui->lbl_hora, "--:--:--");
        lv_label_set_text(s_ui->lbl_fecha, "--/--/----");
    } else {
        lv_label_set_text_fmt(s_ui->lbl_hora, "%02u:%02u:%02u", (unsigned)hora, (unsigned)minuto, (unsigned)segundo);
        lv_label_set_text_fmt(s_ui->lbl_fecha, "%02u/%02u/%04u", (unsigned)dia, (unsigned)mes, (unsigned)anio);
    }

    lvgl_port_unlock();

    ESP_LOGD(TAG, "<- ESTADO_UPDATE ciclo=%u salida=0x%X", (unsigned)ciclo, (unsigned)bitmask_salida);
}

static void procesar_frame(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    switch (cmd) {
    case BRAINDLAB_CMD_ESTADO_UPDATE:
        aplicar_estado_update(payload, len);
        break;
    default:
        ESP_LOGW(TAG, "Comando desconocido 0x%02X", (unsigned)cmd);
        break;
    }
}

// ---------- Tarea de recepcion (parser byte a byte) ----------

typedef enum {
    ST_SOF, ST_CMD, ST_LEN0, ST_LEN1, ST_PAYLOAD, ST_CRC,
} estado_parser_t;

static void uart_rx_task(void *arg)
{
    estado_parser_t estado = ST_SOF;
    uint8_t hdr_payload[3 + BRAINDLAB_MAX_PAYLOAD];
    uint16_t len = 0, idx = 0;
    uint8_t b;

    while (1) {
        if (uart_read_bytes(BRAINDLAB_UART_PORT, &b, 1, pdMS_TO_TICKS(200)) != 1) {
            continue;
        }

        switch (estado) {
        case ST_SOF:
            if (b == BRAINDLAB_SOF) {
                estado = ST_CMD;
            }
            break;
        case ST_CMD:
            hdr_payload[0] = b;
            estado = ST_LEN0;
            break;
        case ST_LEN0:
            hdr_payload[1] = b;
            estado = ST_LEN1;
            break;
        case ST_LEN1:
            hdr_payload[2] = b;
            len = (uint16_t)hdr_payload[1] | ((uint16_t)hdr_payload[2] << 8);
            if (len > BRAINDLAB_MAX_PAYLOAD) {
                ESP_LOGW(TAG, "LEN invalido (%u), trama descartada", (unsigned)len);
                estado = ST_SOF;
            } else {
                idx = 0;
                estado = (len == 0) ? ST_CRC : ST_PAYLOAD;
            }
            break;
        case ST_PAYLOAD:
            hdr_payload[3 + idx] = b;
            idx++;
            if (idx >= len) {
                estado = ST_CRC;
            }
            break;
        case ST_CRC: {
            uint8_t crc_calc = braindlab_crc8(hdr_payload, (uint16_t)(3 + len));
            if (crc_calc == b) {
                procesar_frame(hdr_payload[0], &hdr_payload[3], len);
            } else {
                ESP_LOGW(TAG, "CRC invalido, trama descartada");
            }
            estado = ST_SOF;
            break;
        }
        }
    }
}

void uart_braindlab_init(ui_dashboard_t *ui)
{
    s_ui = ui;

    uart_config_t cfg = {
        .baud_rate = BRAINDLAB_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(BRAINDLAB_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(BRAINDLAB_UART_PORT, BRAINDLAB_UART_TX_PIN, BRAINDLAB_UART_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(BRAINDLAB_UART_PORT, 1024, 1024, 0, NULL, 0));

    xTaskCreate(uart_rx_task, "uart_braindlab_rx", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "UART listo en UART_NUM_%d (TX=%d, RX=%d, %d baud)",
             BRAINDLAB_UART_PORT, BRAINDLAB_UART_TX_PIN, BRAINDLAB_UART_RX_PIN, BRAINDLAB_UART_BAUD);
}

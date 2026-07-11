#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "protocolo_labgeo.h"
#include "uart_labgeo.h"

static const char *TAG = "UART_LABGEO";

// ---------- Pinout del enlace con el controlador de sensores ----------
// Confirmado: conector UART de esta pantalla = IO17 (TX) / IO18 (RX).
#define LABGEO_UART_PORT   UART_NUM_1
#define LABGEO_UART_TX_PIN GPIO_NUM_17
#define LABGEO_UART_RX_PIN GPIO_NUM_18
#define LABGEO_UART_BAUD   115200

static ui_labgeo_t  *s_ui = NULL;
static ui_grafica_t *s_grafica = NULL;

// ---------- Envio de tramas ----------

static void enviar_frame(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    uint8_t buf[4 + LABGEO_MAX_PAYLOAD + 1];
    uint16_t i = 0;

    buf[i++] = LABGEO_SOF;
    buf[i++] = cmd;
    buf[i++] = (uint8_t)(len & 0xFF);
    buf[i++] = (uint8_t)((len >> 8) & 0xFF);
    if (len > 0) {
        memcpy(&buf[i], payload, len);
        i += len;
    }
    buf[i++] = labgeo_crc8(&buf[1], (uint16_t)(3 + len)); // CMD+LEN+PAYLOAD, sin el SOF

    uart_write_bytes(LABGEO_UART_PORT, (const char *)buf, i);
}

void uart_labgeo_enviar_start(uint8_t run_id)
{
    ESP_LOGI(TAG, "-> START corrida %u", (unsigned)run_id);
    enviar_frame(LABGEO_CMD_START, &run_id, 1);
}

void uart_labgeo_enviar_stop(uint8_t run_id)
{
    ESP_LOGI(TAG, "-> STOP corrida %u", (unsigned)run_id);
    enviar_frame(LABGEO_CMD_STOP, &run_id, 1);
}

void uart_labgeo_enviar_request_run(uint8_t run_id)
{
    ESP_LOGI(TAG, "-> REQUEST_RUN corrida %u", (unsigned)run_id);

    if (s_grafica && lvgl_port_lock(0)) {
        ui_grafica_reset(s_grafica);
        lvgl_port_unlock();
    }
    enviar_frame(LABGEO_CMD_REQUEST_RUN, &run_id, 1);
}

// ---------- Aplicar datos recibidos a la UI ----------

static void aplicar_sensor_update(const uint8_t *p, uint16_t len)
{
    if (len < 17 || !s_ui) {
        ESP_LOGW(TAG, "SENSOR_UPDATE invalido (len=%u)", (unsigned)len);
        return;
    }

    uint8_t  run_id     = p[0];
    int32_t  dial1_um   = labgeo_leer_i32(&p[1]);
    int32_t  dial2_um   = labgeo_leer_i32(&p[5]);
    int32_t  peso_mN    = labgeo_leer_i32(&p[9]);
    uint32_t tiempo_ms  = labgeo_leer_u32(&p[13]);
    (void) run_id; // se podria comparar contra la corrida activa si hace falta validar

    if (lvgl_port_lock(0)) {
        lv_label_set_text_fmt(s_ui->lbl_dial1, "%.3f", dial1_um / 1000.0);
        lv_label_set_text_fmt(s_ui->lbl_dial2, "%.3f", dial2_um / 1000.0);
        lv_label_set_text_fmt(s_ui->lbl_peso, "%.1f", peso_mN / 1000.0);

        uint32_t total_s = tiempo_ms / 1000;
        lv_label_set_text_fmt(s_ui->lbl_tiempo, "%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32,
                               total_s / 3600, (total_s / 60) % 60, total_s % 60);

        lvgl_port_unlock();
    }
}

static void aplicar_run_chunk(const uint8_t *p, uint16_t len)
{
    if (len < 3 || !s_grafica) {
        ESP_LOGW(TAG, "RUN_CHUNK invalido (len=%u)", (unsigned)len);
        return;
    }

    uint8_t run_id    = p[0];
    uint8_t count     = p[1];
    uint8_t es_ultimo = p[2];

    if (count > LABGEO_CHUNK_MAX_PUNTOS || len < (uint16_t)(3 + count * 12)) {
        ESP_LOGW(TAG, "RUN_CHUNK con datos incompletos (count=%u, len=%u)", (unsigned)count, (unsigned)len);
        return;
    }

    if (lvgl_port_lock(0)) {
        for (uint8_t i = 0; i < count; i++) {
            const uint8_t *punto = &p[3 + i * 12];
            int32_t dial1_um = labgeo_leer_i32(&punto[0]);
            int32_t peso_mN  = labgeo_leer_i32(&punto[4]);
            // El tiempo del punto (punto[8..11]) no se usa como eje X todavia;
            // el chart grafica por indice de punto.
            ui_grafica_agregar_punto(s_grafica, dial1_um / 1000, peso_mN / 1000);
        }
        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "<- RUN_CHUNK corrida %u: %u puntos%s", (unsigned)run_id, (unsigned)count,
             es_ultimo ? " (ultimo)" : "");
}

static void procesar_frame(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    switch (cmd) {
    case LABGEO_CMD_SENSOR_UPDATE:
        aplicar_sensor_update(payload, len);
        break;
    case LABGEO_CMD_RUN_CHUNK:
        aplicar_run_chunk(payload, len);
        break;
    default:
        ESP_LOGW(TAG, "Comando desconocido 0x%02X", (unsigned)cmd);
        break;
    }
}

// ---------- Tarea de recepcion (parser byte a byte, corre en su propia tarea) ----------

typedef enum {
    ST_SOF, ST_CMD, ST_LEN0, ST_LEN1, ST_PAYLOAD, ST_CRC,
} estado_parser_t;

static void uart_rx_task(void *arg)
{
    estado_parser_t estado = ST_SOF;
    uint8_t hdr_payload[3 + LABGEO_MAX_PAYLOAD]; // cmd, len_lo, len_hi, payload...
    uint16_t len = 0, idx = 0;
    uint8_t b;

    while (1) {
        if (uart_read_bytes(LABGEO_UART_PORT, &b, 1, pdMS_TO_TICKS(200)) != 1) {
            continue;
        }

        switch (estado) {
        case ST_SOF:
            if (b == LABGEO_SOF) {
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
            if (len > LABGEO_MAX_PAYLOAD) {
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
            uint8_t crc_calc = labgeo_crc8(hdr_payload, (uint16_t)(3 + len));
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

void uart_labgeo_init(ui_labgeo_t *ui, ui_grafica_t *grafica)
{
    s_ui = ui;
    s_grafica = grafica;

    uart_config_t cfg = {
        .baud_rate = LABGEO_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(LABGEO_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LABGEO_UART_PORT, LABGEO_UART_TX_PIN, LABGEO_UART_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(LABGEO_UART_PORT, 1024, 1024, 0, NULL, 0));

    xTaskCreate(uart_rx_task, "uart_labgeo_rx", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "UART listo en UART_NUM_%d (TX=%d, RX=%d, %d baud)",
             LABGEO_UART_PORT, LABGEO_UART_TX_PIN, LABGEO_UART_RX_PIN, LABGEO_UART_BAUD);
}

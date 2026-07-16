#include <string.h>
#include <stdio.h>
#include <stdbool.h>
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
static uart_labgeo_cb_estado_t s_cb_estado = NULL;

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

void uart_labgeo_set_cb_estado(uart_labgeo_cb_estado_t cb)
{
    s_cb_estado = cb;
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

// Formatea 'valor_milesimas' (entero en milesimas de la unidad -- p.ej.
// micrometros para mostrar mm, o mN para mostrar N) como texto
// "[-]entero.fraccion" con 'decimales' cifras decimales, SIN usar "%f" en
// ningun lado. CONFIG_LV_USE_FLOAT esta apagado en este proyecto (ver
// sdkconfig), y con eso el sprintf propio de LVGL (CONFIG_LV_USE_BUILTIN_SPRINTF)
// no soporta "%f" -- lv_label_set_text_fmt(..., "%.3f", ...) terminaba
// mostrando literalmente la letra "f" en la pantalla en vez del numero.
static void formatear_milesimas(char *buf, size_t buf_len, int32_t valor_milesimas, int decimales)
{
    bool negativo = valor_milesimas < 0;
    uint32_t v = negativo ? (uint32_t)(-valor_milesimas) : (uint32_t)valor_milesimas;

    uint32_t divisor = 1;
    for (int i = 0; i < (3 - decimales); i++) {
        divisor *= 10;
    }
    uint32_t tope_frac = 1;
    for (int i = 0; i < decimales; i++) {
        tope_frac *= 10;
    }

    uint32_t entero = v / 1000;
    uint32_t resto = v % 1000;
    uint32_t frac = (resto + divisor / 2) / divisor; // redondeo al mas cercano
    if (frac >= tope_frac) {                          // se llevo un digito entero (ej 999.96 -> 1000)
        frac -= tope_frac;
        entero += 1;
    }

    snprintf(buf, buf_len, "%s%" PRIu32 ".%0*" PRIu32,
             negativo ? "-" : "", entero, decimales, frac);
}

static void aplicar_sensor_update(const uint8_t *p, uint16_t len)
{
    if (len < 18 || !s_ui) {
        ESP_LOGW(TAG, "SENSOR_UPDATE invalido (len=%u)", (unsigned)len);
        return;
    }

    uint8_t  run_id     = p[0];
    int32_t  dial1_um   = labgeo_leer_i32(&p[1]);
    int32_t  dial2_um   = labgeo_leer_i32(&p[5]);
    int32_t  peso_mN    = labgeo_leer_i32(&p[9]);
    uint32_t tiempo_ms  = labgeo_leer_u32(&p[13]);
    uint8_t  estado_ensayo = p[17]; // ver uart_labgeo_cb_estado_t en uart_labgeo.h
    (void) run_id; // se podria comparar contra la corrida activa si hace falta validar

    if (lvgl_port_lock(0)) {
        char texto[16];

        formatear_milesimas(texto, sizeof(texto), dial1_um, 3);
        lv_label_set_text(s_ui->lbl_dial1, texto);

        formatear_milesimas(texto, sizeof(texto), dial2_um, 3);
        lv_label_set_text(s_ui->lbl_dial2, texto);

        formatear_milesimas(texto, sizeof(texto), peso_mN, 1);
        lv_label_set_text(s_ui->lbl_peso, texto);

        uint32_t total_s = tiempo_ms / 1000;
        lv_label_set_text_fmt(s_ui->lbl_tiempo, "%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32,
                               total_s / 3600, (total_s / 60) % 60, total_s % 60);

        // Con el LVGL lock ya tomado -- el callback solo toca lv_obj/lv_label,
        // no vuelve a pedir el lock (evita bloqueo/reentrancia).
        if (s_cb_estado) {
            s_cb_estado(estado_ensayo);
        }

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

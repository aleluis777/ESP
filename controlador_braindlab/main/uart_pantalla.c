// Arma y parsea las tramas del protocolo binario definido en
// protocolo_braindlab.h. Mismo patron que uart_link.c en controlador_labgeo:
// el envio arma el buffer y lo escribe de una; la recepcion corre en su
// propia tarea de FreeRTOS con una maquina de estados byte a byte (no
// depende de que las tramas lleguen completas de un solo tiron).

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "protocolo_braindlab.h"
#include "uart_pantalla.h"

static const char *TAG = "UART_PANTALLA";

// Ver comentario de pines en uart_pantalla.h.
#define BRAINDLAB_UART_PORT   UART_NUM_1
#define BRAINDLAB_UART_TX_PIN GPIO_NUM_26
#define BRAINDLAB_UART_RX_PIN GPIO_NUM_39
#define BRAINDLAB_UART_BAUD   115200

static uart_pantalla_callbacks_t s_cb = {0};

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
    // CRC sobre CMD+LEN+PAYLOAD (todo menos el SOF), por eso arranca en &buf[1].
    buf[i++] = braindlab_crc8(&buf[1], (uint16_t)(3 + len));

    uart_write_bytes(BRAINDLAB_UART_PORT, (const char *)buf, i);
}

// float en grados/% -> i16 en decimas, con clamp para no desbordar el
// rango de un i16 si algun dia llega un numero fuera de lo razonable.
static int16_t a_decimas(float valor)
{
    float escalado = valor * 10.0f;
    if (escalado > 32767.0f) return 32767;
    if (escalado < -32768.0f) return -32768;
    return (int16_t)escalado;
}

// Para magnitudes siempre positivas (voltaje, corriente): valor * escala en
// u16, recortado a 0..65535.
static uint16_t a_u16_escalado(float valor, float escala)
{
    float escalado = valor * escala;
    if (escalado > 65535.0f) return 65535;
    if (escalado < 0.0f) return 0;
    return (uint16_t)(escalado + 0.5f);
}

void uart_pantalla_enviar_estado(uint8_t ciclo, const float temperaturas[4],
                                  const bool salida_aire[4], const bool aire_manual[4],
                                  uint8_t indice_reserva, bool alarma_at, bool alarma_at_manual,
                                  bool bypass_solicitado, bool bypass_manual, bool bypass_activo,
                                  bool modo_manual, bool eth_conectado,
                                  bool gestor_ok, float temp_gestor, float humedad_gestor,
                                  uint32_t uptime_s,
                                  uint16_t anio, uint8_t mes, uint8_t dia,
                                  uint8_t hora, uint8_t minuto, uint8_t segundo,
                                  bool energia_ok, const float voltajes[3], const float corrientes[3],
                                  uint8_t sd_estado, uint8_t fallas)
{
    uint8_t payload[BRAINDLAB_ESTADO_UPDATE_LEN];
    uint16_t idx = 0;

    braindlab_put_u8(payload, &idx, ciclo);
    for (int i = 0; i < 4; i++) {
        braindlab_put_i16(payload, &idx, a_decimas(temperaturas[i]));
    }

    uint8_t bitmask_salida = 0, bitmask_manual = 0;
    for (int i = 0; i < 4; i++) {
        if (salida_aire[i]) bitmask_salida |= (1 << i);
        if (aire_manual[i]) bitmask_manual |= (1 << i);
    }
    braindlab_put_u8(payload, &idx, bitmask_salida);
    braindlab_put_u8(payload, &idx, bitmask_manual);

    braindlab_put_u8(payload, &idx, indice_reserva);
    braindlab_put_u8(payload, &idx, alarma_at ? 1 : 0);
    braindlab_put_u8(payload, &idx, alarma_at_manual ? 1 : 0);
    braindlab_put_u8(payload, &idx, bypass_solicitado ? 1 : 0);
    braindlab_put_u8(payload, &idx, bypass_manual ? 1 : 0);
    braindlab_put_u8(payload, &idx, bypass_activo ? 1 : 0);
    braindlab_put_u8(payload, &idx, modo_manual ? 1 : 0);
    braindlab_put_u8(payload, &idx, eth_conectado ? 1 : 0);
    braindlab_put_u8(payload, &idx, gestor_ok ? 1 : 0);
    braindlab_put_i16(payload, &idx, a_decimas(temp_gestor));
    braindlab_put_i16(payload, &idx, a_decimas(humedad_gestor));
    braindlab_put_u32(payload, &idx, uptime_s);
    braindlab_put_u16(payload, &idx, anio);
    braindlab_put_u8(payload, &idx, mes);
    braindlab_put_u8(payload, &idx, dia);
    braindlab_put_u8(payload, &idx, hora);
    braindlab_put_u8(payload, &idx, minuto);
    braindlab_put_u8(payload, &idx, segundo);

    braindlab_put_u8(payload, &idx, energia_ok ? 1 : 0);
    for (int i = 0; i < 3; i++) {
        braindlab_put_u16(payload, &idx, a_u16_escalado(voltajes[i], 10.0f));    // decimas de V
    }
    for (int i = 0; i < 3; i++) {
        braindlab_put_u16(payload, &idx, a_u16_escalado(corrientes[i], 100.0f)); // centesimas de A
    }
    braindlab_put_u8(payload, &idx, sd_estado);
    braindlab_put_u8(payload, &idx, fallas);

    enviar_frame(BRAINDLAB_CMD_ESTADO_UPDATE, payload, idx);
}

// ---------- Recepcion ----------

static void procesar_frame(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    switch (cmd) {
    case BRAINDLAB_CMD_CONTROL_AIRE:
        if (len < 2) {
            ESP_LOGW(TAG, "CONTROL_AIRE sin payload completo, descartado");
            return;
        }
        ESP_LOGI(TAG, "<- CONTROL_AIRE indice=%u encendido=%u", (unsigned)payload[0], (unsigned)payload[1]);
        if (s_cb.on_control_aire) {
            s_cb.on_control_aire(payload[0], payload[1] != 0);
        }
        break;
    case BRAINDLAB_CMD_CONTROL_BYPASS:
        if (len < 1) {
            ESP_LOGW(TAG, "CONTROL_BYPASS sin payload, descartado");
            return;
        }
        ESP_LOGI(TAG, "<- CONTROL_BYPASS solicitado=%u", (unsigned)payload[0]);
        if (s_cb.on_control_bypass) {
            s_cb.on_control_bypass(payload[0] != 0);
        }
        break;
    case BRAINDLAB_CMD_CONTROL_AT:
        if (len < 1) {
            ESP_LOGW(TAG, "CONTROL_AT sin payload, descartado");
            return;
        }
        ESP_LOGI(TAG, "<- CONTROL_AT activa=%u", (unsigned)payload[0]);
        if (s_cb.on_control_at) {
            s_cb.on_control_at(payload[0] != 0);
        }
        break;
    case BRAINDLAB_CMD_CONTROL_AUTOMATICO:
        ESP_LOGI(TAG, "<- CONTROL_AUTOMATICO");
        if (s_cb.on_control_automatico) {
            s_cb.on_control_automatico();
        }
        break;
    default:
        ESP_LOGW(TAG, "Comando desconocido 0x%02X", (unsigned)cmd);
        break;
    }
}

// Misma maquina de estados que uart_link.c/uart_labgeo.c -- ver esos
// archivos para el comentario completo de cada estado.
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

void uart_pantalla_set_callbacks(uart_pantalla_callbacks_t callbacks)
{
    s_cb = callbacks;
}

void uart_pantalla_init(void)
{
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

    xTaskCreate(uart_rx_task, "uart_pantalla_rx", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "UART listo en UART_NUM_%d (TX=%d, RX=%d, %d baud)",
             BRAINDLAB_UART_PORT, BRAINDLAB_UART_TX_PIN, BRAINDLAB_UART_RX_PIN, BRAINDLAB_UART_BAUD);
}

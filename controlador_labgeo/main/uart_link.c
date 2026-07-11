// Arma y parsea las tramas del protocolo binario definido en
// protocolo_labgeo.h. app_main.c no sabe nada de bytes ni de UART: solo le
// pide a este archivo "mandale esto a la pantalla" (uart_link_enviar_*) y
// registra callbacks para "avisame cuando llegue esto" (uart_link_set_callbacks).
//
// El envio es simple (arma el buffer y lo escribe). La recepcion es mas
// interesante: corre en su propia tarea de FreeRTOS con una maquina de
// estados que va leyendo byte a byte, para no bloquear ni depender de que
// las tramas lleguen completas de una sola vez (el UART puede entregar los
// bytes de a poquito).

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "protocolo_labgeo.h"
#include "uart_link.h"

static const char *TAG = "UART_LINK";

// OJO: estos son placeholders, todavia sin confirmar. GPIO17/18 (los que
// usa la pantalla del otro lado) NO SIRVEN ACA: en este controlador GPIO17
// es el CS del W5500 (Ethernet.init(17) en LinaresETH.cpp) y GPIO18 es casi
// seguro el SCK del bus SPI por defecto (VSPI) que usa el mismo modulo.
// GPIO4 y GPIO16 no aparecen usados en LABGEO2.ino ni en LinaresETH, pero
// confirmalos contra tu cableado real antes de flashear.
#define LABGEO_UART_PORT   UART_NUM_1
#define LABGEO_UART_TX_PIN GPIO_NUM_4
#define LABGEO_UART_RX_PIN GPIO_NUM_16
#define LABGEO_UART_BAUD   115200

static uart_link_callbacks_t s_cb = {0};

// ---------- Envio de tramas ----------

// Arma una trama completa (SOF + CMD + LEN + PAYLOAD + CRC8) y la escribe
// de una al UART. 'payload' ya viene armado por el que llama (ver las dos
// funciones de abajo).
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
    // El CRC se calcula sobre CMD+LEN+PAYLOAD (todo menos el SOF), por eso
    // arranca en &buf[1] con longitud 3+len.
    buf[i++] = labgeo_crc8(&buf[1], (uint16_t)(3 + len));

    uart_write_bytes(LABGEO_UART_PORT, (const char *)buf, i);
}

// Arma el payload de SENSOR_UPDATE (17 bytes: run_id + 3 valores i32 + 1
// tiempo u32) usando los helpers little-endian de protocolo_labgeo.h, y lo
// manda. Lo llama app_main.c hasta 5 veces por segundo mientras hay una
// corrida activa.
void uart_link_enviar_sensor_update(uint8_t run_id, int32_t dial1_um, int32_t dial2_um,
                                     int32_t peso_mN, uint32_t tiempo_ms)
{
    uint8_t payload[17];
    uint16_t idx = 0;
    labgeo_put_u8(payload, &idx, run_id);
    labgeo_put_i32(payload, &idx, dial1_um);
    labgeo_put_i32(payload, &idx, dial2_um);
    labgeo_put_i32(payload, &idx, peso_mN);
    labgeo_put_u32(payload, &idx, tiempo_ms);

    enviar_frame(LABGEO_CMD_SENSOR_UPDATE, payload, idx);
}

// Arma un RUN_CHUNK: run_id + count + es_ultimo + los bytes crudos de hasta
// 16 puntos (ya vienen armados desde almacenamiento.c, se copian tal cual
// sin reinterpretarlos). Lo llama app_main.c una vez por cada bloque que le
// entrega almacenamiento_leer_corrida().
void uart_link_enviar_run_chunk(uint8_t run_id, const uint8_t *puntos_buf, uint8_t count, uint8_t es_ultimo)
{
    uint8_t payload[3 + 12 * LABGEO_CHUNK_MAX_PUNTOS];
    uint16_t idx = 0;
    labgeo_put_u8(payload, &idx, run_id);
    labgeo_put_u8(payload, &idx, count);
    labgeo_put_u8(payload, &idx, es_ultimo);
    if (count > 0) {
        memcpy(&payload[idx], puntos_buf, (size_t)count * 12);
        idx += (uint16_t)(count * 12);
    }

    enviar_frame(LABGEO_CMD_RUN_CHUNK, payload, idx);
}

// ---------- Recepcion ----------

// Una vez que uart_rx_task() valido el CRC de una trama completa, esta
// funcion mira el CMD y llama al callback de app_main.c que corresponda.
// Los 3 comandos que puede mandar la pantalla llevan siempre el run_id como
// primer (y unico) byte del payload.
static void procesar_frame(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    if (len < 1) {
        ESP_LOGW(TAG, "Trama 0x%02X sin payload de run_id, descartada", (unsigned)cmd);
        return;
    }
    uint8_t run_id = payload[0];

    switch (cmd) {
    case LABGEO_CMD_START:
        ESP_LOGI(TAG, "<- START corrida %u", (unsigned)run_id);
        if (s_cb.on_start) {
            s_cb.on_start(run_id);
        }
        break;
    case LABGEO_CMD_STOP:
        ESP_LOGI(TAG, "<- STOP corrida %u", (unsigned)run_id);
        if (s_cb.on_stop) {
            s_cb.on_stop(run_id);
        }
        break;
    case LABGEO_CMD_REQUEST_RUN:
        ESP_LOGI(TAG, "<- REQUEST_RUN corrida %u", (unsigned)run_id);
        if (s_cb.on_request_run) {
            s_cb.on_request_run(run_id);
        }
        break;
    default:
        ESP_LOGW(TAG, "Comando desconocido 0x%02X", (unsigned)cmd);
        break;
    }
}

// Estados de la maquina que arma la trama byte a byte: espera el byte de
// sincronismo (SOF), despues el comando, despues los 2 bytes de longitud,
// despues 'len' bytes de payload, y por ultimo el CRC.
typedef enum {
    ST_SOF, ST_CMD, ST_LEN0, ST_LEN1, ST_PAYLOAD, ST_CRC,
} estado_parser_t;

// Tarea dedicada que lee el UART byte a byte en un loop infinito. Vive
// separada de tarea_sensores para que un problema de timing en un lado no
// afecte al otro. 'hdr_payload' guarda CMD+LEN+PAYLOAD tal como van
// llegando, para poder calcularle el CRC completo al final sin tener que
// rearmar el buffer.
static void uart_rx_task(void *arg)
{
    estado_parser_t estado = ST_SOF;
    uint8_t hdr_payload[3 + LABGEO_MAX_PAYLOAD];
    uint16_t len = 0, idx = 0;
    uint8_t b;

    while (1) {
        // Timeout de 200ms: si no llega nada, vuelve a intentar (no bloquea
        // para siempre, permite que la tarea siga viva y responsiva).
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
            // Si el CRC no coincide, se descarta la trama entera y se
            // vuelve a ST_SOF a esperar la proxima -- es la unica forma de
            // recuperarse de una trama corrupta, no hay reintentos ni ACK
            // en este protocolo (ver PROTOCOLO_UART.md).
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

// app_main.c llama a esto ANTES de uart_link_init(), para que no haya
// ventana de tiempo donde pueda llegar un comando y no haya nadie
// escuchando.
void uart_link_set_callbacks(uart_link_callbacks_t callbacks)
{
    s_cb = callbacks;
}

// Configura el puerto UART fisico y lanza la tarea de recepcion. Despues de
// esto, cualquier comando que mande la pantalla ya dispara los callbacks
// registrados.
void uart_link_init(void)
{
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

    xTaskCreate(uart_rx_task, "uart_link_rx", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "UART listo en UART_NUM_%d (TX=%d, RX=%d, %d baud)",
             LABGEO_UART_PORT, LABGEO_UART_TX_PIN, LABGEO_UART_RX_PIN, LABGEO_UART_BAUD);
}

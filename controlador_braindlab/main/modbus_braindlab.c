#include "modbus_braindlab.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "MODBUS";

// Hardware -- HARDWARE.md §4.3. UART0 es la consola, UART1 la pantalla.
#define MB_UART        UART_NUM_2
#define MB_PIN_TX      GPIO_NUM_33
#define MB_PIN_RX      GPIO_NUM_36 // solo entrada, sin pull interno (la salida RO del SP3485 lo maneja)
#define MB_PIN_EN      GPIO_NUM_32 // EN_485 = DE + /RE, va como RTS del UART
#define MB_BAUDIOS     9600        // valores de fabrica del JSY-MK-333G: 9600 8N1, esclavo 1
#define MB_ESCLAVO     1

// Registros (ver JSY-MK-333G_monofasico_modbus.md §3): 0x0100..0x0105 =
// voltaje A, B, C y corriente A, B, C (= fases R, S, T), todos ÷100. Se
// leen los 6 juntos en una sola trama.
#define MB_REG_INICIO      0x0100
#define MB_REG_CANTIDAD    6
#define MB_REG_MODELO      0x0000 // prueba de vida: devuelve 0x0333
#define MB_MODELO_ESPERADO 0x0333

// Tiempos. El medidor actualiza sus datos cada 1 s, no sirve leer mas rapido.
// Una respuesta de 6 registros (17 bytes) tarda ~18 ms a 9600 -- 300 ms de
// timeout cubre de sobra la demora de proceso del medidor.
#define MB_TIMEOUT_MS      300
#define MB_PERIODO_MS      1000
#define MB_PERIODO_FALLA_MS 10000 // sin medidor: preguntar cada 10 s, no ensuciar log ni bus
#define MB_FALLAS_P_LENTO  3      // fallas seguidas antes de pasar al periodo lento
#define MB_VIGENCIA_MS     10000  // una lectura mas vieja que esto se publica como ok=false

static SemaphoreHandle_t s_mutex = NULL;
static modbus_lectura_t s_lectura = { 0 };
static int64_t s_ultima_ok_us = 0;

// CRC16 Modbus (polinomio 0xA001, inicial 0xFFFF). Va en la trama byte
// BAJO primero.
static uint16_t crc16(const uint8_t *datos, size_t largo)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < largo; i++) {
        crc ^= datos[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
        }
    }
    return crc;
}

static void log_trama(const char *dir, const uint8_t *t, int n)
{
    // Nivel DEBUG: para ver las tramas crudas al validar A/B, subir el
    // nivel con esp_log_level_set("MODBUS", ESP_LOG_DEBUG).
    ESP_LOG_BUFFER_HEX_LEVEL(dir, t, n, ESP_LOG_DEBUG);
}

// Funcion 03 (Read Holding Registers). Devuelve ESP_OK y llena 'regs' solo
// si la respuesta llego completa, con CRC valido y del esclavo esperado.
static esp_err_t leer_registros(uint16_t inicio, uint16_t cantidad, uint16_t *regs)
{
    uint8_t pedido[8] = {
        MB_ESCLAVO, 0x03,
        inicio >> 8, inicio & 0xFF,
        cantidad >> 8, cantidad & 0xFF,
    };
    uint16_t crc = crc16(pedido, 6);
    pedido[6] = crc & 0xFF;
    pedido[7] = crc >> 8;

    uart_flush_input(MB_UART); // basura de una respuesta tardia anterior
    log_trama("MODBUS TX", pedido, sizeof(pedido));
    uart_write_bytes(MB_UART, pedido, sizeof(pedido));
    uart_wait_tx_done(MB_UART, pdMS_TO_TICKS(100));

    // Respuesta normal: esclavo, 03, nbytes, datos(2*cantidad), crc(2).
    // Excepcion: esclavo, 0x83, codigo, crc(2) = 5 bytes.
    uint8_t resp[5 + 2 * 125];
    const int esperado = 5 + 2 * cantidad;
    int n = 0;
    TickType_t limite = xTaskGetTickCount() + pdMS_TO_TICKS(MB_TIMEOUT_MS);
    while (n < esperado) {
        TickType_t ahora = xTaskGetTickCount();
        if (ahora >= limite) {
            break;
        }
        int r = uart_read_bytes(MB_UART, resp + n, esperado - n, limite - ahora);
        if (r <= 0) {
            break;
        }
        n += r;
        if (n >= 5 && resp[1] == (0x03 | 0x80)) {
            break; // excepcion, ya llego entera
        }
    }

    if (n > 0) {
        log_trama("MODBUS RX", resp, n);
    }
    if (n == 0) {
        return ESP_ERR_TIMEOUT;
    }
    if (n >= 5 && resp[1] == (0x03 | 0x80)) {
        ESP_LOGW(TAG, "El medidor respondio excepcion %02X (01=funcion, 02=registro, 03=valor)", resp[2]);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (n < esperado) {
        ESP_LOGW(TAG, "Respuesta incompleta (%d de %d bytes)", n, esperado);
        return ESP_ERR_INVALID_SIZE;
    }
    uint16_t crc_rx = resp[esperado - 2] | (resp[esperado - 1] << 8);
    if (crc16(resp, esperado - 2) != crc_rx) {
        ESP_LOGW(TAG, "CRC invalido -- ruido en la linea o A/B mal cableados");
        return ESP_ERR_INVALID_CRC;
    }
    if (resp[0] != MB_ESCLAVO || resp[1] != 0x03 || resp[2] != 2 * cantidad) {
        ESP_LOGW(TAG, "Respuesta inesperada (esclavo %u, funcion %02X, %u bytes)", resp[0], resp[1], resp[2]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    for (int i = 0; i < cantidad; i++) {
        regs[i] = (resp[3 + 2 * i] << 8) | resp[4 + 2 * i]; // byte alto primero
    }
    return ESP_OK;
}

static void tarea_modbus(void *arg)
{
    // Prueba de vida al arrancar: el registro de modelo debe valer 0x0333.
    // Solo informativo (deja claro en el log si el cableado anda), el loop
    // de abajo arranca igual.
    uint16_t modelo = 0;
    esp_err_t err = leer_registros(MB_REG_MODELO, 1, &modelo);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Medidor responde: modelo 0x%04X%s", modelo,
                 modelo == MB_MODELO_ESPERADO ? " (JSY-MK-333G OK)" : " -- no es el esperado 0x0333");
    } else {
        ESP_LOGW(TAG, "El medidor no responde a la prueba de vida (%s) -- revisar alimentacion, A/B "
                      "(probar invertirlos), %d 8N1 y direccion %d", esp_err_to_name(err), MB_BAUDIOS, MB_ESCLAVO);
    }

    int fallas_seguidas = 0;
    while (1) {
        uint16_t regs[MB_REG_CANTIDAD];
        err = leer_registros(MB_REG_INICIO, MB_REG_CANTIDAD, regs);
        int64_t ahora_us = esp_timer_get_time();

        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (err == ESP_OK) {
            for (int i = 0; i < MODBUS_NUM_FASES; i++) {
                s_lectura.voltajes[i] = regs[i] / 100.0f;
                s_lectura.corrientes[i] = regs[3 + i] / 100.0f;
            }
            s_ultima_ok_us = ahora_us;
        }
        xSemaphoreGive(s_mutex);

        if (err == ESP_OK) {
            if (fallas_seguidas >= MB_FALLAS_P_LENTO) {
                ESP_LOGI(TAG, "Medidor de vuelta en linea");
            }
            fallas_seguidas = 0;
            ESP_LOGD(TAG, "V R/S/T=%.2f/%.2f/%.2f  I R/S/T=%.2f/%.2f/%.2f",
                     regs[0] / 100.0f, regs[1] / 100.0f, regs[2] / 100.0f,
                     regs[3] / 100.0f, regs[4] / 100.0f, regs[5] / 100.0f);
        } else {
            fallas_seguidas++;
            if (fallas_seguidas == MB_FALLAS_P_LENTO) {
                ESP_LOGW(TAG, "Medidor sin respuesta (%s) %d veces seguidas -- se pregunta cada %d s",
                         esp_err_to_name(err), fallas_seguidas, MB_PERIODO_FALLA_MS / 1000);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(fallas_seguidas >= MB_FALLAS_P_LENTO ? MB_PERIODO_FALLA_MS : MB_PERIODO_MS));
    }
}

esp_err_t modbus_braindlab_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        return ESP_ERR_NO_MEM;
    }

    uart_config_t cfg = {
        .baud_rate = MB_BAUDIOS,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(MB_UART, 512, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install fallo (%s)", esp_err_to_name(err));
        return err;
    }
    ESP_ERROR_CHECK(uart_param_config(MB_UART, &cfg));
    // EN_485 en el pin RTS: en modo RS-485 half-duplex el UART lo pone en
    // ALTO mientras transmite (DE activo) y en BAJO al terminar el ultimo
    // bit (/RE activo, escucha) -- justo la logica de HARDWARE.md §4.3.
    ESP_ERROR_CHECK(uart_set_pin(MB_UART, MB_PIN_TX, MB_PIN_RX, MB_PIN_EN, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_mode(MB_UART, UART_MODE_RS485_HALF_DUPLEX));

    xTaskCreate(tarea_modbus, "tarea_modbus", 3072, NULL, 4, NULL);
    ESP_LOGI(TAG, "RS-485 listo en UART_NUM_%d (TX=%d, RX=%d, EN=%d, %d 8N1, esclavo %d)",
             MB_UART, MB_PIN_TX, MB_PIN_RX, MB_PIN_EN, MB_BAUDIOS, MB_ESCLAVO);
    return ESP_OK;
}

void modbus_braindlab_leer(modbus_lectura_t *lectura)
{
    if (!s_mutex) {
        memset(lectura, 0, sizeof(*lectura));
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *lectura = s_lectura;
    int64_t ultima = s_ultima_ok_us;
    xSemaphoreGive(s_mutex);

    if (ultima == 0) {
        lectura->ok = false;
        lectura->edad_ms = 0;
        return;
    }
    lectura->edad_ms = (uint32_t)((esp_timer_get_time() - ultima) / 1000);
    lectura->ok = lectura->edad_ms <= MB_VIGENCIA_MS;
}

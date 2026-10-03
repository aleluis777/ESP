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
static ui_energia_t *s_energia = NULL;

// Enlace con el controlador: si no llega una trama valida en este tiempo
// se muestra la fila "COM" (el controlador manda ESTADO_UPDATE cada ~2 s).
// Es lo unico que la pantalla puede saber por si sola -- el controlador, a
// su vez, no sabe si la pantalla esta (el protocolo no tiene ACK).
#define ENLACE_TIMEOUT_MS 6000
static volatile TickType_t s_ultimo_frame_ok = 0;
static bool s_sin_enlace = false; // solo lo toca uart_rx_task

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
    // Minimo = payload v1 (sin energia); los campos de energia se leen solo
    // si la trama los trae (controlador con firmware nuevo).
    if (len < BRAINDLAB_ESTADO_UPDATE_LEN_V1 || !s_ui) {
        ESP_LOGW(TAG, "ESTADO_UPDATE invalido (len=%u)", (unsigned)len);
        return;
    }

    uint8_t ciclo             = p[0];
    int16_t temps_decimas[4]  = { braindlab_leer_i16(&p[1]), braindlab_leer_i16(&p[3]),
                                  braindlab_leer_i16(&p[5]), braindlab_leer_i16(&p[7]) };
    uint8_t bitmask_salida    = p[9];
    uint8_t indice_reserva    = p[11];
    bool alarma_at            = p[12] != 0;
    bool bypass_solicitado    = p[14] != 0;
    bool bypass_activo        = p[16] != 0;
    int16_t temp_gestor_decimas = braindlab_leer_i16(&p[20]);
    int16_t humedad_decimas   = braindlab_leer_i16(&p[22]);
    uint16_t anio             = braindlab_leer_u16(&p[28]);
    uint8_t mes = p[30], dia = p[31], hora = p[32], minuto = p[33], segundo = p[34];
    (void) indice_reserva; // sin widget propio en el dashboard todavia

    bool hay_energia = len >= 48;
    // Controlador viejo (sin el campo): se asume OK para no mostrar una
    // alarma de SD que no se puede saber.
    uint8_t sd_estado = len >= 49 ? p[48] : BRAINDLAB_SD_OK;
    // Idem para 'fallas': sin el campo, no se muestra la fila MOD.
    uint8_t fallas = len >= BRAINDLAB_ESTADO_UPDATE_LEN ? p[49] : 0;
    bool energia_ok = hay_energia && p[35] != 0;
    uint16_t voltajes[3] = { 0 }, corrientes[3] = { 0 };
    if (hay_energia) {
        for (int i = 0; i < 3; i++) {
            voltajes[i] = braindlab_leer_u16(&p[36 + 2 * i]);
            corrientes[i] = braindlab_leer_u16(&p[42 + 2 * i]);
        }
    }

    // Print de TODO lo que llego (una linea por trama), antes de tocar LVGL.
    // Las decimas se muestran como entero crudo (235 = 23.5) para ver
    // exactamente lo que mando el controlador.
    ESP_LOGI(TAG, "<- ESTADO_UPDATE len=%u | ciclo=%u T1..T4=%d,%d,%d,%d (dec) salida=0x%02X manual=0x%02X reserva=%u "
                  "at=%u at_man=%u bps_sol=%u bps_man=%u bps_act=%u modo_man=%u eth=%u gestor_ok=%u "
                  "T_gestor=%d HR=%d (dec) uptime=%lus fecha=%04u-%02u-%02u %02u:%02u:%02u",
             (unsigned)len, (unsigned)ciclo,
             (int)temps_decimas[0], (int)temps_decimas[1], (int)temps_decimas[2], (int)temps_decimas[3],
             (unsigned)bitmask_salida, (unsigned)p[10], (unsigned)indice_reserva,
             (unsigned)alarma_at, (unsigned)p[13], (unsigned)bypass_solicitado, (unsigned)p[15],
             (unsigned)bypass_activo, (unsigned)p[17], (unsigned)p[18], (unsigned)p[19],
             (int)temp_gestor_decimas, (int)humedad_decimas, (unsigned long)braindlab_leer_u32(&p[24]),
             (unsigned)anio, (unsigned)mes, (unsigned)dia, (unsigned)hora, (unsigned)minuto, (unsigned)segundo);
    if (hay_energia) {
        ESP_LOGI(TAG, "   energia_ok=%u V R/S/T=%u/%u/%u (dec) I R/S/T=%u/%u/%u (cent)",
                 (unsigned)energia_ok, voltajes[0], voltajes[1], voltajes[2],
                 corrientes[0], corrientes[1], corrientes[2]);
    }
    ESP_LOGI(TAG, "   sd_estado=%u fallas=0x%02X", (unsigned)sd_estado, (unsigned)fallas);

    if (!lvgl_port_lock(0)) {
        ESP_LOGW(TAG, "No se pudo tomar el lock de LVGL, trama recibida pero no aplicada a la UI");
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

    // ---- Temperatura y humedad del gabinete ----
    char texto_temp_gestor[16];
    formatear_decimas(texto_temp_gestor, sizeof(texto_temp_gestor), temp_gestor_decimas, "\xC2\xB0" "C");
    lv_label_set_text(s_ui->lbl_temp_gestor, texto_temp_gestor);

    char texto_hum[16];
    formatear_decimas(texto_hum, sizeof(texto_hum), humedad_decimas, " %RH");
    lv_label_set_text(s_ui->lbl_humedad, texto_hum);

    // ---- Controles Bypass / AT (resincroniza el boton con el estado real
    // ya aplicado -- por si la automatica lo cambio, o por si el pedido
    // manual que mando esta pantalla ya se aplico) ----
    ui_dashboard_bypass_set_activo(s_ui, bypass_solicitado);
    ui_dashboard_at_set_activo(s_ui, alarma_at);

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
    if (sd_estado == BRAINDLAB_SD_OK) {
        lv_obj_add_flag(s_ui->fila_alarma_sd, LV_OBJ_FLAG_HIDDEN);
    } else {
        const char *texto_sd =
            sd_estado == BRAINDLAB_SD_SIN_TARJETA ? "SD: sin tarjeta" :
            sd_estado == BRAINDLAB_SD_SIN_FORMATO ? "SD: sin formato FAT32" :
            sd_estado == BRAINDLAB_SD_ERROR_ESCRITURA ? "SD: error de escritura" :
            sd_estado == BRAINDLAB_SD_FORMATEANDO ? "SD: formateando..." : "SD: estado desconocido";
        lv_label_set_text(s_ui->lbl_alarma_sd, texto_sd);
        lv_obj_remove_flag(s_ui->fila_alarma_sd, LV_OBJ_FLAG_HIDDEN);
    }

    // Modulos con falla (la SD ya tiene su propia fila, no se repite aca).
    static const struct { uint8_t bit; const char *nombre; } modulos[] = {
        { BRAINDLAB_FALLA_RTC,    "RTC" },
        { BRAINDLAB_FALLA_ADC,    "ADC" },
        { BRAINDLAB_FALLA_GESTOR, "Sensor HR" },
        { BRAINDLAB_FALLA_ETH,    "Ethernet" },
        { BRAINDLAB_FALLA_MODBUS, "Medidor" },
    };
    char texto_mod[64] = "";
    for (size_t i = 0; i < sizeof(modulos) / sizeof(modulos[0]); i++) {
        if (fallas & modulos[i].bit) {
            if (texto_mod[0]) strlcat(texto_mod, ", ", sizeof(texto_mod));
            strlcat(texto_mod, modulos[i].nombre, sizeof(texto_mod));
        }
    }
    if (texto_mod[0]) {
        lv_label_set_text(s_ui->lbl_alarma_mod, texto_mod);
        lv_obj_remove_flag(s_ui->fila_alarma_mod, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui->fila_alarma_mod, LV_OBJ_FLAG_HIDDEN);
    }

    // ---- Energia (vista Parametros Electricos) ----
    if (s_energia) {
        ui_energia_set_valores(s_energia, energia_ok, voltajes, corrientes);
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
}

static void procesar_frame(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    switch (cmd) {
    case BRAINDLAB_CMD_ESTADO_UPDATE:
        s_ultimo_frame_ok = xTaskGetTickCount();
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
    TickType_t ultimo_rx = xTaskGetTickCount();
    TickType_t ultimo_aviso = ultimo_rx;

    ESP_LOGI(TAG, "Tarea RX lista, esperando tramas en UART_NUM_%d (RX=GPIO%d)",
             BRAINDLAB_UART_PORT, BRAINDLAB_UART_RX_PIN);

    s_ultimo_frame_ok = xTaskGetTickCount();

    while (1) {
        // Fila "COM": se revisa en cada vuelta (como mucho cada 200 ms) y
        // solo se toca LVGL cuando el estado del enlace cambia.
        bool sin_enlace = (xTaskGetTickCount() - s_ultimo_frame_ok) >= pdMS_TO_TICKS(ENLACE_TIMEOUT_MS);
        if (sin_enlace != s_sin_enlace && s_ui && lvgl_port_lock(0)) {
            if (sin_enlace) {
                lv_obj_remove_flag(s_ui->fila_alarma_com, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_ui->fila_alarma_com, LV_OBJ_FLAG_HIDDEN);
            }
            lvgl_port_unlock();
            s_sin_enlace = sin_enlace;
            ESP_LOGW(TAG, "Enlace con el controlador %s", sin_enlace ? "PERDIDO" : "recuperado");
        }

        if (uart_read_bytes(BRAINDLAB_UART_PORT, &b, 1, pdMS_TO_TICKS(200)) != 1) {
            // Sin bytes: avisa cada 5 s para saber que el enlace esta mudo
            // (cable/pines/GND/baud) y no solo que "no se ve nada".
            TickType_t ahora = xTaskGetTickCount();
            if ((ahora - ultimo_aviso) >= pdMS_TO_TICKS(5000)) {
                ESP_LOGW(TAG, "Sin bytes del controlador hace %lu ms (revisar TX/RX cruzados, GND, baud %d)",
                         (unsigned long)((ahora - ultimo_rx) * portTICK_PERIOD_MS), BRAINDLAB_UART_BAUD);
                ultimo_aviso = ahora;
            }
            continue;
        }
        ultimo_rx = xTaskGetTickCount();
        ultimo_aviso = ultimo_rx;

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
                ESP_LOGW(TAG, "CRC invalido (calculado=0x%02X recibido=0x%02X cmd=0x%02X len=%u), trama descartada",
                         (unsigned)crc_calc, (unsigned)b, (unsigned)hdr_payload[0], (unsigned)len);
                ESP_LOG_BUFFER_HEX(TAG, hdr_payload, 3 + len);
            }
            estado = ST_SOF;
            break;
        }
        }
    }
}

void uart_braindlab_init(ui_dashboard_t *ui, ui_energia_t *energia)
{
    s_ui = ui;
    s_energia = energia;

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

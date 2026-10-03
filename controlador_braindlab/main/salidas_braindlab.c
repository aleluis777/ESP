#include "salidas_braindlab.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "SALIDAS_BRAINDLAB";

// GPIO tomados directo de HARDWARE.md §3/§5.1/§11 -- no tocar sin actualizar
// ese documento tambien.
#define PIN_AA1        GPIO_NUM_12
#define PIN_AA2        GPIO_NUM_2
#define PIN_AA3        GPIO_NUM_16
#define PIN_AA4        GPIO_NUM_4
#define PIN_OUT_AT     GPIO_NUM_13
#define PIN_BP_S       GPIO_NUM_14
#define PIN_BPS_STATUS GPIO_NUM_34 // solo entrada, sin pull interno (HARDWARE.md §10)
#define PIN_BUZZER     GPIO_NUM_15 // strapping: R25+Q12 lo mantienen bajo al arrancar (HARDWARE.md §10)

static const gpio_num_t s_pines_aire[SALIDAS_NUM_AIRES] = { PIN_AA1, PIN_AA2, PIN_AA3, PIN_AA4 };

esp_err_t salidas_init(void)
{
    for (int i = 0; i < SALIDAS_NUM_AIRES; i++) {
        gpio_reset_pin(s_pines_aire[i]);
        gpio_set_direction(s_pines_aire[i], GPIO_MODE_OUTPUT);
        gpio_set_level(s_pines_aire[i], 0); // reles desenergizados (HARDWARE.md §10)
    }

    gpio_reset_pin(PIN_OUT_AT);
    gpio_set_direction(PIN_OUT_AT, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_OUT_AT, 0); // sin alerta

    gpio_reset_pin(PIN_BP_S);
    gpio_set_direction(PIN_BP_S, GPIO_MODE_OUTPUT);
    // HIGH = bypass activo (logica invertida a proposito, fail-safe -- ver
    // salidas_braindlab.h). Arranca en este estado hasta que
    // tarea_climatizacion haga su primera vuelta y lo corrija segun la
    // temperatura real; es la misma ventana de "fail-safe hasta que el
    // control tome la posta" que ya existe en el resto del sistema.
    gpio_set_level(PIN_BP_S, 1);

    // Solo entrada, sin pull interno disponible en este GPIO -- el pull-up
    // externo (R26, 100K) ya esta en la placa (HARDWARE.md §7.2).
    gpio_reset_pin(PIN_BPS_STATUS);
    gpio_set_direction(PIN_BPS_STATUS, GPIO_MODE_INPUT);

    gpio_reset_pin(PIN_BUZZER);
    gpio_set_direction(PIN_BUZZER, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BUZZER, 0); // silencio (HARDWARE.md §10)

    ESP_LOGI(TAG, "Salidas inicializadas: AA1-4=LOW, OUT_AT=LOW, BUZZER=LOW, BP_S=HIGH (bypass fail-safe activo hasta la 1ra vuelta de control)");
    return ESP_OK;
}

void salidas_set_aire(uint8_t indice, bool encendido)
{
    if (indice >= SALIDAS_NUM_AIRES) {
        ESP_LOGE(TAG, "salidas_set_aire: indice %u fuera de rango", indice);
        return;
    }
    gpio_set_level(s_pines_aire[indice], encendido ? 1 : 0);
}

void salidas_set_alarma_at(bool activa)
{
    gpio_set_level(PIN_OUT_AT, activa ? 1 : 0);
}

void salidas_set_bypass_solicitado(bool solicitado)
{
    // Logica invertida a proposito (fail-safe, confirmado contra el
    // esquematico) -- ver salidas_braindlab.h: HIGH = bypass activo.
    gpio_set_level(PIN_BP_S, solicitado ? 1 : 0);
}

bool salidas_leer_bypass_activo(void)
{
    // Logica invertida a proposito, igual que BP_S -- HIGH = bypass
    // fisicamente activo.
    return gpio_get_level(PIN_BPS_STATUS) == 1;
}

void salidas_buzzer_pitido(uint32_t duracion_ms)
{
    gpio_set_level(PIN_BUZZER, 1);
    vTaskDelay(pdMS_TO_TICKS(duracion_ms));
    gpio_set_level(PIN_BUZZER, 0);
}

// Implementacion real -- ver sensores_temp.h.
//
// Confirmado contra el esquematico (conversacion): el ADS1115 "U2" tiene su
// pin ADDR puenteado a 3V3 (R8 poblada de 0 ohm, R9 sin poblar) => direccion
// I2C 0x49 (ADS111X_ADDR_VCC). Sus 4 canales de entrada:
//
//   AIN0 = A5 = T1
//   AIN1 = A6 = T2
//   AIN2 = A7 = T3
//   AIN3 = A8 = T4
//
// Cada canal es un divisor resistivo: 3V3 --[10K fijo, R15-R18]-- Ax
// --[NTC 10K/Beta=3950]-- GND (R14 es un puente de 0 ohm entre 3V3 y el
// nodo comun de los 4 fijos, no una resistencia real del divisor). El
// ADS1115 mide el voltaje en Ax contra GND (mux "X_GND").
//
// U1 (el segundo ADS1115 del esquematico, canales A0-A3) no esta poblado
// todavia -- no se usa.

#include "sensores_temp.h"
#include "ads111x.h"
#include "i2cdev.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "SENSORES_TEMP";

#define ADS1115_ADDR ADS111X_ADDR_VCC   // ADDR->3V3 (R8 poblada), confirmado
#define ADS1115_GAIN ADS111X_GAIN_4V096 // +-4.096V -- cubre 0-3.3V del divisor con margen

#define NTC_R_FIJA_OHM  10000.0f // R15-R18
#define NTC_R25_OHM     10000.0f // NTC 10K a 25 C, confirmado
#define NTC_BETA        3950.0f  // HARDWARE.md: "NTC B3950"
#define NTC_T25_KELVIN  298.15f  // 25 C en Kelvin
#define V_ALIMENTACION  3.3f

static i2c_dev_t s_ads;

// AIN0..AIN3 = T1..T4, en ese orden -- ver comentario de arriba.
static const ads111x_mux_t s_mux_por_canal[4] = {
    ADS111X_MUX_0_GND,
    ADS111X_MUX_1_GND,
    ADS111X_MUX_2_GND,
    ADS111X_MUX_3_GND,
};

esp_err_t sensores_temp_init(void)
{
    // i2cdev_init() es idempotente (esp-idf-lib guarda un flag interno y
    // no vuelve a inicializar si ya se llamo, por ejemplo desde
    // rtc_braindlab_init()) -- no hace falta coordinar el orden entre
    // modulos, cada uno que necesite I2C lo puede llamar por su cuenta.
    esp_err_t err = i2cdev_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2cdev_init() fallo (%s)", esp_err_to_name(err));
        return err;
    }

    err = ads111x_init_desc(&s_ads, ADS1115_ADDR, I2C_NUM_0, GPIO_NUM_21, GPIO_NUM_22);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ads111x_init_desc() fallo (%s)", esp_err_to_name(err));
        return err;
    }

    err = ads111x_set_gain(&s_ads, ADS1115_GAIN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ads111x_set_gain() fallo (%s)", esp_err_to_name(err));
        return err;
    }

    err = ads111x_set_mode(&s_ads, ADS111X_MODE_SINGLE_SHOT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ads111x_set_mode() fallo (%s)", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "ADS1115 (U2, direccion 0x%02x) listo -- 4 canales NTC (A5-A8 = T1-T4)", ADS1115_ADDR);
    return ESP_OK;
}

// Ecuacion Beta: 1/T = 1/T25 + (1/Beta)*ln(R/R25)
static float ntc_resistencia_a_celsius(float r_ntc_ohm)
{
    float inv_t_kelvin = 1.0f / NTC_T25_KELVIN + (1.0f / NTC_BETA) * logf(r_ntc_ohm / NTC_R25_OHM);
    return (1.0f / inv_t_kelvin) - 273.15f;
}

static esp_err_t leer_canal(int canal, float *temperatura_c)
{
    esp_err_t err = ads111x_set_input_mux(&s_ads, s_mux_por_canal[canal]);
    if (err != ESP_OK) {
        return err;
    }

    err = ads111x_start_conversion(&s_ads);
    if (err != ESP_OK) {
        return err;
    }

    // Single-shot: esperar a que termine la conversion (tipico <10ms a
    // 128 SPS, el default de data rate de este driver).
    bool ocupado = true;
    for (int intentos = 0; ocupado && intentos < 20; intentos++) {
        err = ads111x_is_busy(&s_ads, &ocupado);
        if (err != ESP_OK) {
            return err;
        }
        if (ocupado) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    if (ocupado) {
        return ESP_ERR_TIMEOUT;
    }

    int16_t crudo;
    err = ads111x_get_value(&s_ads, &crudo);
    if (err != ESP_OK) {
        return err;
    }

    float voltaje = (float)crudo * ads111x_gain_values[ADS1115_GAIN] / ADS111X_MAX_VALUE;

    // Divisor: 3V3 --R_fija-- Ax --NTC-- GND => V_ax = 3V3 * R_ntc/(R_fija+R_ntc)
    // => R_ntc = R_fija * V_ax / (3V3 - V_ax). Si el voltaje sale fuera de
    // (0, 3V3) el NTC esta desconectado o en corto -- no calcular basura.
    if (voltaje <= 0.0f || voltaje >= V_ALIMENTACION) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    float r_ntc_ohm = NTC_R_FIJA_OHM * voltaje / (V_ALIMENTACION - voltaje);

    *temperatura_c = ntc_resistencia_a_celsius(r_ntc_ohm);
    return ESP_OK;
}

esp_err_t sensores_temp_leer(float temperaturas[4])
{
    esp_err_t ultimo_error = ESP_OK;
    for (int i = 0; i < 4; i++) {
        float valor;
        esp_err_t err = leer_canal(i, &valor);
        if (err == ESP_OK) {
            temperaturas[i] = valor;
        } else {
            ESP_LOGW(TAG, "No se pudo leer T%d (%s) -- se mantiene el ultimo valor", i + 1, esp_err_to_name(err));
            ultimo_error = err;
        }
    }
    return ultimo_error;
}

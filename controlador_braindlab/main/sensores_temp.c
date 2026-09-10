// Implementacion real -- ver sensores_temp.h.
//
// Confirmado contra el esquematico Y verificado en la placa real (escaneo
// de las 4 direcciones posibles por I2C, ver conversacion): el ADS1115 "U2"
// tiene su pin ADDR puenteado a SDA => direccion I2C 0x4A (ADS111X_ADDR_SDA).
// Sus 4 canales de entrada:
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

#define ADS1115_ADDR ADS111X_ADDR_SDA   // ADDR->SDA, confirmado
#define ADS1115_GAIN ADS111X_GAIN_4V096 // +-4.096V -- cubre 0-3.3V del divisor con margen

#define NTC_R_FIJA_OHM  10000.0f // R15-R18
#define NTC_R25_OHM     10000.0f // NTC 10K a 25 C, confirmado
#define NTC_BETA        3950.0f  // HARDWARE.md: "NTC B3950"
#define NTC_T25_KELVIN  298.15f  // 25 C en Kelvin
#define V_ALIMENTACION  3.3f
#define NTC_MARGEN_V    0.1f    // ver comentario en leer_canal() -- deteccion de canal abierto/en corto

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

    // ads111x_init_desc() deja el bus a 1MHz (I2C_FREQ_HZ en ads111x.c) --
    // el ADS1115 no soporta esa velocidad "de frente" (solo 100k/400k, o
    // 3.4M en modo High-Speed con una secuencia de entrada especial que este
    // driver no hace), lo que causaba timeouts intermitentes en las lecturas
    // (el init a veces pasaba de pura suerte). Se baja a 400kHz (Fast mode,
    // dentro de spec del chip) y se habilitan los pull-ups internos del
    // ESP32 (~45k) por si el modulo no trae los suyos (ver HARDWARE.md
    // #4.2, sin confirmar) -- hay que hacerlo ANTES de la primera
    // transaccion real (el set_gain de abajo), porque i2cdev arma el bus
    // recien en el primer uso, leyendo estos campos de dev->cfg en ese
    // momento.
    s_ads.cfg.master.clk_speed = 400000;
    s_ads.cfg.sda_pullup_en = true;
    s_ads.cfg.scl_pullup_en = true;

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

// DIAGNOSTICO TEMPORAL: cada paso loguea su propio error especifico (antes
// leer_canal() los devolvia todos mezclados como un solo esp_err_t generico
// y no se podia saber cual de las 4 llamadas al ADS1115 era la que fallaba)
// -- ver conversacion sobre timeouts intermitentes en T1-T4. Sacar estos
// ESP_LOGE una vez identificada la causa real.
static esp_err_t leer_canal(int canal, float calibracion, float *temperatura_c)
{
    esp_err_t err = ads111x_set_input_mux(&s_ads, s_mux_por_canal[canal]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "canal %d: set_input_mux fallo (%s)", canal, esp_err_to_name(err));
        return err;
    }

    err = ads111x_start_conversion(&s_ads);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "canal %d: start_conversion fallo (%s)", canal, esp_err_to_name(err));
        return err;
    }

    // Single-shot: esperar a que termine la conversion (tipico <10ms a
    // 128 SPS, el default de data rate de este driver).
    //
    // OJO: pdMS_TO_TICKS(2) con CONFIG_FREERTOS_HZ=100 (tick=10ms) trunca a
    // 0 ticks -- (2*100)/1000=0 por division entera -- asi que
    // vTaskDelay(pdMS_TO_TICKS(2)) NO esperaba nada real. Los 20 intentos
    // corrian casi instantaneo (sin I2C real fallando, is_busy siempre
    // devolvia ESP_OK) y nunca le daban al ADS1115 los ~8ms que tarda en
    // convertir -- por eso "nunca bajaba" pese a que el chip respondia bien.
    // 10ms es exactamente 1 tick a 100Hz, un delay real.
    bool ocupado = true;
    int intentos;
    for (intentos = 0; ocupado && intentos < 20; intentos++) {
        err = ads111x_is_busy(&s_ads, &ocupado);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "canal %d: is_busy fallo en intento %d (%s)", canal, intentos, esp_err_to_name(err));
            return err;
        }
        if (ocupado) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (ocupado) {
        ESP_LOGE(TAG, "canal %d: is_busy nunca bajo despues de %d intentos", canal, intentos);
        return ESP_ERR_TIMEOUT;
    }

    int16_t crudo;
    err = ads111x_get_value(&s_ads, &crudo);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "canal %d: get_value fallo (%s)", canal, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "canal %d: crudo=%d (tras %d intentos de is_busy)", canal, crudo, intentos);

    float voltaje = (float)crudo * ads111x_gain_values[ADS1115_GAIN] / ADS111X_MAX_VALUE;

    // Divisor: 3V3 --R_fija-- Ax --NTC-- GND => V_ax = 3V3 * R_ntc/(R_fija+R_ntc)
    // => R_ntc = R_fija * V_ax / (3V3 - V_ax). Si el voltaje sale fuera de
    // (MARGEN, 3V3-MARGEN) el NTC esta desconectado (voltaje ~3V3, R_ntc
    // tendiendo a infinito) o en corto (voltaje ~0V) -- no calcular basura.
    // El limite exacto (0, 3V3) no alcanza: un canal realmente abierto mide
    // ~3.27V en la practica (no exactamente 3.3V, por la impedancia de
    // entrada del ADS1115 y tolerancias), y eso colaba sin error dando una
    // "temperatura" de -52C sin sentido en vez de detectarse como invalido.
    // NTC_MARGEN_V=0.1V deja pasar el rango real esperado (-40C a +100C
    // aprox con NTC 10K/3950) sin falsos rechazos, ver conversacion.
    if (voltaje <= NTC_MARGEN_V || voltaje >= (V_ALIMENTACION - NTC_MARGEN_V)) {
        ESP_LOGW(TAG, "canal %d: voltaje %.3fV fuera de rango valido -- NTC desconectado o en corto", canal, voltaje);
        return ESP_ERR_INVALID_RESPONSE;
    }
    float r_ntc_ohm = NTC_R_FIJA_OHM * voltaje / (V_ALIMENTACION - voltaje);

    // Calibracion de un solo punto: constante = valor_real - valor_leido,
    // se suma tal cual sobre el resultado de la ecuacion Beta (0.0 si el
    // canal no se calibro todavia, ver config_calibracion_t).
    *temperatura_c = ntc_resistencia_a_celsius(r_ntc_ohm) + calibracion;
    return ESP_OK;
}

esp_err_t sensores_temp_leer(float temperaturas[4], const float calibracion[4])
{
    esp_err_t ultimo_error = ESP_OK;
    for (int i = 0; i < 4; i++) {
        float valor;
        esp_err_t err = leer_canal(i, calibracion[i], &valor);
        if (err == ESP_OK) {
            temperaturas[i] = valor;
        } else {
            ESP_LOGW(TAG, "No se pudo leer T%d (%s) -- se mantiene el ultimo valor", i + 1, esp_err_to_name(err));
            ultimo_error = err;
        }
    }
    return ultimo_error;
}

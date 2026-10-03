#include <time.h>
#include "rtc_braindlab.h"
#include "ds1307.h"
#include "i2cdev.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "RTC_BRAINDLAB";

static i2c_dev_t s_rtc;
static bool s_disponible = false;

// Deteccion de reloj congelado (ver rtc_braindlab_detenido()).
#define RTC_DETENIDO_MS 5000
static time_t s_ultima_hora = (time_t)-1;  // ultima hora leida (segundos)
static int64_t s_cambio_us = 0;            // cuando cambio por ultima vez
static bool s_detenido = false;

esp_err_t rtc_braindlab_init(void)
{
    esp_err_t err = i2cdev_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2cdev_init() fallo (%s)", esp_err_to_name(err));
        return err;
    }

    err = ds1307_init_desc(&s_rtc, I2C_NUM_0, GPIO_NUM_21, GPIO_NUM_22);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ds1307_init_desc() fallo (%s)", esp_err_to_name(err));
        return err;
    }

    bool corriendo = false;
    err = ds1307_is_running(&s_rtc, &corriendo);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo consultar el DS1307 (%s) -- revisar cableado I2C (SDA=21 SCL=22)",
                 esp_err_to_name(err));
        return err;
    }

    if (!corriendo) {
        // Chip parado (tipico si se quedo sin pila de respaldo o es la
        // primera vez que se energiza) -- lo arrancamos, pero la hora que
        // tenga guardada puede estar vieja/en 0. Ajustarla de verdad queda
        // pendiente (todavia no hay endpoint para eso).
        ESP_LOGW(TAG, "DS1307 detenido -- arrancando el oscilador (la hora puede estar desactualizada)");
        ds1307_start(&s_rtc, true);
    }

    s_disponible = true;
    ESP_LOGI(TAG, "DS1307 detectado y corriendo");
    return ESP_OK;
}

esp_err_t rtc_braindlab_leer(struct tm *tiempo)
{
    if (!s_disponible) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ds1307_get_time(&s_rtc, tiempo);
    if (err != ESP_OK) {
        return err;
    }

    // Copia: mktime() normaliza el struct, no tocar el que se devuelve.
    struct tm copia = *tiempo;
    time_t hora = mktime(&copia);
    int64_t ahora_us = esp_timer_get_time();
    if (hora != s_ultima_hora) {
        if (s_detenido) {
            ESP_LOGI(TAG, "El DS1307 volvio a avanzar");
        }
        s_ultima_hora = hora;
        s_cambio_us = ahora_us;
        s_detenido = false;
    } else if (!s_detenido && (ahora_us - s_cambio_us) >= (int64_t)RTC_DETENIDO_MS * 1000) {
        s_detenido = true;
        ESP_LOGE(TAG, "El DS1307 responde pero su hora NO avanza (%d s congelada) -- el oscilador no corre: "
                      "revisar alimentacion (el DS1307 es de 5 V, a 3.3 V suele pasar esto), pila/VBAT "
                      "(pila puesta o VBAT a GND) y el cristal de 32.768 kHz",
                 RTC_DETENIDO_MS / 1000);
    }
    return ESP_OK;
}

bool rtc_braindlab_detenido(void)
{
    return s_detenido;
}

esp_err_t rtc_braindlab_ajustar(const struct tm *tiempo)
{
    if (!s_disponible) {
        return ESP_ERR_INVALID_STATE;
    }

    // mktime() normaliza el struct completo (recalcula tm_wday/tm_yday a
    // partir de year/mon/mday) -- se lo aplicamos a una copia para no exigir
    // que quien llama sepa calcular el dia de la semana a mano. El time_t
    // que devuelve no se usa para nada (no nos importa la conversion a
    // epoch, solo el efecto colateral de normalizar los campos).
    struct tm normalizado = *tiempo;
    mktime(&normalizado);

    esp_err_t err = ds1307_set_time(&s_rtc, &normalizado);
    if (err == ESP_OK) {
        // Hora nueva: se vuelve a medir desde cero si avanza o no.
        s_ultima_hora = (time_t)-1;
        s_detenido = false;
        ESP_LOGI(TAG, "RTC ajustado a %04d-%02d-%02d %02d:%02d:%02d",
                 normalizado.tm_year + 1900, normalizado.tm_mon + 1, normalizado.tm_mday,
                 normalizado.tm_hour, normalizado.tm_min, normalizado.tm_sec);
    } else {
        ESP_LOGE(TAG, "ds1307_set_time() fallo (%s)", esp_err_to_name(err));
    }
    return err;
}

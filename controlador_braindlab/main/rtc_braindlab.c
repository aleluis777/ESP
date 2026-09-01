#include "rtc_braindlab.h"
#include "ds1307.h"
#include "i2cdev.h"
#include "esp_log.h"

static const char *TAG = "RTC_BRAINDLAB";

static i2c_dev_t s_rtc;
static bool s_disponible = false;

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
    return ds1307_get_time(&s_rtc, tiempo);
}

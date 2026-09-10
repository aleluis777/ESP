#include "sensor_gestor.h"
#include "dht.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "SENSOR_GESTOR";

#define SENSOR_GESTOR_GPIO GPIO_NUM_27 // net "HUM", ver HARDWARE.md

esp_err_t sensor_gestor_leer(float *temperatura, float *humedad, float calibracion_temp, float calibracion_humedad)
{
    esp_err_t err = dht_read_float_data(DHT_TYPE_AM2301, SENSOR_GESTOR_GPIO, humedad, temperatura);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo leer el AM2301A (%s)", esp_err_to_name(err));
        return err;
    }

    // Calibracion de un solo punto: constante = valor_real - valor_leido.
    *temperatura += calibracion_temp;
    *humedad += calibracion_humedad;
    return ESP_OK;
}

// Guarda/lee la calibracion (pendiente + offset de cada sensor) en NVS, la
// particion de flash pensada para pares clave/valor chicos -- reemplaza a
// la EEPROM que usaba el .ino anterior. Se guarda todo el struct
// config_calibracion_t de una, como un solo "blob" bajo una clave.

#include <string.h>
#include "config_labgeo.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "CONFIG_LABGEO";
#define NVS_NAMESPACE "labgeo"  // "carpeta" logica dentro de NVS
#define NVS_KEY_CALIB "calib"   // clave donde vive el blob de calibracion

// Prepara la particion NVS para poder usarla. Si esta corrupta (por ejemplo
// por un corte de luz a mitad de una escritura) o quedo de una version de
// ESP-IDF vieja incompatible, la borra y la vuelve a inicializar en blanco
// -- se pierde la calibracion guardada, pero el equipo arranca igual en vez
// de quedar trabado.
esp_err_t config_labgeo_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS corrupta o de otra version, se borra y reintenta");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void config_labgeo_cargar(config_calibracion_t *cfg)
{
    // Defaults neutros: si todavia no se calibro nada, dial_mm = dial_crudo
    // y peso_N = celda_cruda (sin escalar), para que el sistema arranque
    // sin dividir por cero.
    config_calibracion_t defaults = {
        .dial1_pendiente = 1.0f, .dial1_offset = 0.0f,
        .dial2_pendiente = 1.0f, .dial2_offset = 0.0f,
        .celda_pendiente = 1.0f, .celda_offset = 0.0f,
    };

    // Si el namespace ni siquiera existe todavia (primera vez que corre el
    // firmware, nunca se guardo nada), usar los defaults directamente.
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Sin calibracion guardada, uso valores por defecto");
        *cfg = defaults;
        return;
    }

    // nvs_get_blob necesita que 'len' venga con el tamano del buffer
    // destino; a la vuelta trae el tamano real leido (por eso el chequeo
    // len != sizeof(*cfg): si alguna vez cambia el struct, no confiamos en
    // un blob viejo con otro tamano).
    size_t len = sizeof(*cfg);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_CALIB, cfg, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(*cfg)) {
        ESP_LOGW(TAG, "Calibracion invalida o inexistente, uso valores por defecto");
        *cfg = defaults;
    }
}

// Persiste la calibracion actual. nvs_commit() es necesario porque NVS
// guarda en cache hasta que se pide explicitamente escribir a flash.
esp_err_t config_labgeo_guardar(const config_calibracion_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(h, NVS_KEY_CALIB, cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

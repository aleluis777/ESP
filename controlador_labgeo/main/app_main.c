// ---------------------------------------------------------------------------
// VERSION DE DIAGNOSTICO -- no es el firmware normal.
//
// Ya confirmado: hardware/IRQ del W5500 OK, red_eth_init() OK (ping
// responde, bug de MAC vs esp_netif arreglado en red_eth.c), y
// servidor_web_init() OK (HTTP + WebSocket + /files.html). Paso siguiente:
// agregar la celda de carga (HX711) sola -- todavia sin los diales, sin
// UART hacia la pantalla, sin programador de corrida -- para ver sus datos
// crudos antes de sumar el resto de los sensores.
//
// Que probar una vez flasheado:
//   - En el monitor serie, tag "CELDA": deberia loguear un valor crudo cada
//     ~200ms (cambia si le poner/sacas peso a la celda).
//   - En el navegador, http://192.168.18.91/ -- el JSON que llega por /ws
//     ahora incluye "celda_crudo" (aunque el resto de la pagina siga
//     mostrando los diales en 0, porque esos todavia no estan conectados).
//
// El firmware completo (sensores + UART + red_eth + servidor_web +
// ping_monitor) esta respaldado en "app_main copy.c" -- restaurarlo cuando
// todos los sensores esten confirmados.
// ---------------------------------------------------------------------------

#include <inttypes.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "red_eth.h"
#include "servidor_web.h"
#include "hx711.h"

static const char *TAG = "DIAG_RED_ETH";

// Mismos pines que usaba LABGEO2.ino / el firmware real (app_main copy.c).
#define PIN_HX711_DOUT GPIO_NUM_22
#define PIN_HX711_SCK  GPIO_NUM_21

static hx711_t s_celda;

// Lee la celda en loop y manda el valor crudo (sin calibrar todavia -- eso
// es config_labgeo.c, que no metimos en este diagnostico) por log y por WS.
static void tarea_celda(void *arg)
{
    while (1) {
        int32_t crudo = 0;
        if (hx711_leer(&s_celda, &crudo, 50)) {
            ESP_LOGI("CELDA", "crudo=%" PRId32, crudo);

            char json[64];
            snprintf(json, sizeof(json), "{\"celda_crudo\":%" PRId32 "}", crudo);
            servidor_web_enviar_ws(json);
        } else {
            ESP_LOGW("CELDA", "timeout de lectura (HX711 no conectado o sin alimentacion?)");
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Diagnostico: red_eth + servidor_web + celda de carga (sin diales/UART todavia)");

    // La celda no depende de la red -- se inicializa y arranca su tarea
    // siempre, este o no este el Ethernet disponible (mismo criterio que el
    // firmware real: sensores y red son independientes).
    hx711_init(&s_celda, PIN_HX711_DOUT, PIN_HX711_SCK);
    xTaskCreate(tarea_celda, "tarea_celda", 3072, NULL, 5, NULL);

    esp_err_t err = red_eth_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "red_eth_init() fallo (%s) -- no tiene sentido levantar el servidor web sin red", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "red_eth_init() OK");
        err = servidor_web_init();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "servidor_web_init() OK -- probar http://192.168.18.91/ desde el navegador");
        } else {
            ESP_LOGE(TAG, "servidor_web_init() fallo (%s)", esp_err_to_name(err));
        }
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "(sigue corriendo, esperando eventos de Ethernet/HTTP)");
    }
}

// ---------------------------------------------------------------------------
// PRUEBA MINIMA: solo UART hacia la pantalla, nada de red/sensores/almacenamiento.
// El resto del firmware esta respaldado en otra copia -- este archivo se
// puede pisar sin miedo. Unico objetivo: loguear en el RX del controlador
// (GPIO14) lo que llega cuando se toca un boton en la pantalla (START/STOP/
// REQUEST_RUN, ver protocolo_labgeo.h).
// ---------------------------------------------------------------------------

#include "esp_log.h"
#include "uart_link.h"

static const char *TAG = "PRUEBA_UART";

static void on_start(uint8_t run_id)
{
    ESP_LOGI(TAG, "<- START corrida %u", (unsigned)run_id);
}

static void on_stop(uint8_t run_id)
{
    ESP_LOGI(TAG, "<- STOP corrida %u", (unsigned)run_id);
}

static void on_request_run(uint8_t run_id)
{
    ESP_LOGI(TAG, "<- REQUEST_RUN corrida %u", (unsigned)run_id);
}

void app_main(void)
{
    uart_link_callbacks_t cbs = {
        .on_start = on_start,
        .on_stop = on_stop,
        .on_request_run = on_request_run,
    };
    uart_link_set_callbacks(cbs);
    uart_link_init();

    ESP_LOGI(TAG, "Escuchando UART (RX=GPIO14) -- toca un boton en la pantalla");
}

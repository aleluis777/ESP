// ---------------------------------------------------------------------------
// VERSION DE DIAGNOSTICO -- no es el firmware normal.
//
// Ya confirmado: hardware/IRQ del W5500 OK, red_eth_init() OK (ping
// responde), servidor_web_init() OK (HTTP + WS + /files.html), y la celda
// de carga lee crudo por HX711. Paso siguiente: calibracion real via
// config.json (SPIFFS "www") en vez de valores fijos, con dos endpoints
// para calibrar sin reflashear.
//
// Flujo de calibracion (dos pasos, en este orden):
//   1) Sacar el peso de la celda (nada de peso encima) y hacer
//      POST /calibrar_cero (sin body) -- guarda el crudo actual como offset.
//   2) Poner un peso de referencia conocido y hacer
//      POST /calibrar_maximo con body {"peso_n": <peso en Newtons>} --
//      calcula la pendiente contra el offset del paso 1 y guarda todo en
//      config.json.
//
// Ejemplo con curl:
//   curl -X POST http://192.168.18.91/calibrar_cero
//   curl -X POST http://192.168.18.91/calibrar_maximo -d "{\"peso_n\":98.1}"
//
// El firmware completo (sensores + UART + red_eth + servidor_web +
// ping_monitor) esta respaldado en "app_main copy.c" -- restaurarlo cuando
// todos los sensores esten confirmados.
// ---------------------------------------------------------------------------

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "red_eth.h"
#include "servidor_web.h"
#include "hx711.h"
#include "config_labgeo.h"

static const char *TAG = "DIAG_RED_ETH";

// Mismos pines que usaba LABGEO2.ino / el firmware real (app_main copy.c).
#define PIN_HX711_DOUT GPIO_NUM_22
#define PIN_HX711_SCK  GPIO_NUM_21

static hx711_t s_celda;
static config_calibracion_t s_cal; // calibracion cargada de /www/config.json

// Lee la celda, aplica la calibracion actual, y manda el resultado por log
// y por WS (crudo + peso ya calibrado, para poder comparar los dos mientras
// se prueba).
static void tarea_celda(void *arg)
{
    while (1) {
        int32_t crudo = 0;
        if (hx711_leer(&s_celda, &crudo, 50)) {
            float peso_n = (s_cal.celda_pendiente != 0.0f)
                               ? ((float)crudo - s_cal.celda_offset) / s_cal.celda_pendiente
                               : 0.0f;

            ESP_LOGI("CELDA", "crudo=%" PRId32 "  peso_n=%.3f", crudo, peso_n);

            char json[96];
            snprintf(json, sizeof(json), "{\"celda_crudo\":%" PRId32 ",\"peso_n\":%.3f}", crudo, peso_n);
            servidor_web_enviar_ws(json);
        } else {
            ESP_LOGW("CELDA", "timeout de lectura (HX711 no conectado o sin alimentacion?)");
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// POST /calibrar_cero: sin peso sobre la celda, el crudo de ahora mismo ES
// el offset. Hay que llamar esto ANTES de /calibrar_maximo.
static void on_calibrar_cero(void)
{
    int32_t crudo = 0;
    if (!hx711_leer(&s_celda, &crudo, 200)) {
        ESP_LOGW(TAG, "calibrar_cero: timeout leyendo la celda, no se guarda nada");
        return;
    }

    s_cal.celda_offset = (float)crudo;
    if (config_labgeo_guardar(&s_cal) == ESP_OK) {
        ESP_LOGI(TAG, "calibrar_cero: offset=%.0f guardado en config.json", s_cal.celda_offset);
    } else {
        ESP_LOGE(TAG, "calibrar_cero: no se pudo guardar config.json");
    }
}

// POST /calibrar_maximo: con el peso de referencia 'peso_n' ya puesto,
// calcula la pendiente contra el offset que ya deberia estar guardado por
// /calibrar_cero. Si peso_n es 0 no hace nada (evita dividir por cero).
static void on_calibrar_maximo(float peso_n)
{
    if (peso_n == 0.0f) {
        ESP_LOGW(TAG, "calibrar_maximo: peso_n=0, no se puede calcular la pendiente");
        return;
    }

    int32_t crudo = 0;
    if (!hx711_leer(&s_celda, &crudo, 200)) {
        ESP_LOGW(TAG, "calibrar_maximo: timeout leyendo la celda, no se guarda nada");
        return;
    }

    s_cal.celda_pendiente = ((float)crudo - s_cal.celda_offset) / peso_n;
    if (config_labgeo_guardar(&s_cal) == ESP_OK) {
        ESP_LOGI(TAG, "calibrar_maximo: pendiente=%.4f guardada (crudo=%" PRId32 ", peso_n=%.2f, offset=%.0f)",
                 s_cal.celda_pendiente, crudo, peso_n, s_cal.celda_offset);
    } else {
        ESP_LOGE(TAG, "calibrar_maximo: no se pudo guardar config.json");
    }
}

// POST /configurar_red: por ahora SOLO guarda ip/gateway/mascara en
// config.json -- todavia no los aplica al W5500 (red_eth.c sigue usando sus
// valores fijos hasta que hagamos el paso 3, pendiente a proposito).
static void on_configurar_red(const char *ip, const char *gateway, const char *mascara)
{
    config_red_t red;
    strncpy(red.ip, ip, sizeof(red.ip) - 1);
    red.ip[sizeof(red.ip) - 1] = '\0';
    strncpy(red.gateway, gateway, sizeof(red.gateway) - 1);
    red.gateway[sizeof(red.gateway) - 1] = '\0';
    strncpy(red.mascara, mascara, sizeof(red.mascara) - 1);
    red.mascara[sizeof(red.mascara) - 1] = '\0';

    if (config_labgeo_guardar_red(&red) != ESP_OK) {
        ESP_LOGE(TAG, "configurar_red: no se pudo guardar config.json");
    }
    // El log de exito (con los valores) ya lo hace config_labgeo_guardar_red().
}

void app_main(void)
{
    ESP_LOGI(TAG, "Diagnostico: red_eth + servidor_web + celda con calibracion via config.json");

    // 1) Calibracion primero: monta SPIFFS "www" y carga config.json (o los
    // defaults de config_labgeo_defaults.h si no existe/esta corrupto).
    // Tiene que pasar antes de arrancar la celda, y no depende de la red.
    config_labgeo_init();
    config_labgeo_cargar(&s_cal);

    // 2) La celda tampoco depende de la red -- se inicializa y arranca su
    // tarea siempre, este o no este el Ethernet disponible.
    hx711_init(&s_celda, PIN_HX711_DOUT, PIN_HX711_SCK);
    xTaskCreate(tarea_celda, "tarea_celda", 3072, NULL, 5, NULL);

    // 3) Registrar los callbacks ANTES de levantar el servidor web, mismo
    // criterio que uart_link_set_callbacks().
    servidor_web_callbacks_t web_cbs = {
        .on_calibrar_cero = on_calibrar_cero,
        .on_calibrar_maximo = on_calibrar_maximo,
        .on_configurar_red = on_configurar_red,
    };
    servidor_web_set_callbacks(web_cbs);

    esp_err_t err = red_eth_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "red_eth_init() fallo (%s) -- no tiene sentido levantar el servidor web sin red", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "red_eth_init() OK");
        err = servidor_web_init();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "servidor_web_init() OK -- probar http://192.168.18.91/ y los POST de calibracion");
        } else {
            ESP_LOGE(TAG, "servidor_web_init() fallo (%s)", esp_err_to_name(err));
        }
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "(sigue corriendo, esperando eventos de Ethernet/HTTP)");
    }
}

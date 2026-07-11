// Driver bit-bang del HX711 (amplificador de la celda de carga). No hay
// libreria de por medio: este archivo mueve el pin SCK a mano y lee DOUT en
// cada pulso, siguiendo el protocolo que describe el datasheet del chip.
//
// Como funciona en la practica: el HX711 muestrea la celda continuamente y
// deja el resultado listo internamente; DOUT baja a 0 para avisar "tengo un
// dato nuevo". Para leerlo, el maestro (nosotros) pulsa SCK 24 veces y en
// cada pulso lee un bit de DOUT (MSB primero) -> son los 24 bits del dato.
// Un par de pulsos extra despues del bit 24 le dicen al chip que canal/
// ganancia usar para la SIGUIENTE lectura (no afecta la que se acaba de leer).

#include "hx711.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "HX711";

// Ancho de pulso de reloj: el HX711 pide minimo 0.2us en alto, y si se deja
// en alto mas de 60us el chip entra en power-down. 1us da margen de sobra
// para ambos limites sin acercarse a ninguno.
#define HX711_PULSO_US 1

// Para el pulso de reset/power-down se necesita lo opuesto: mantener SCK en
// alto MAS de 60us a proposito (95us da margen). Bajarlo despues saca al
// chip de power-down (o lo resetea si ya estaba despierto).
#define HX711_RESET_PULSO_US 95

void hx711_init(hx711_t *h, gpio_num_t pin_dout, gpio_num_t pin_sck)
{
    h->pin_dout = pin_dout;
    h->pin_sck = pin_sck;

    ESP_LOGI(TAG, "init: configurando pines (DOUT=%d, SCK=%d)", pin_dout, pin_sck);

    // SCK lo manejamos nosotros (salida); arranca en bajo, que es el estado
    // de reposo del protocolo.
    gpio_reset_pin(pin_sck);
    gpio_set_direction(pin_sck, GPIO_MODE_OUTPUT);
    gpio_set_level(pin_sck, 0);
    ESP_LOGI(TAG, "init: SCK (GPIO%d) configurado como salida, en bajo", pin_sck);

    // DOUT lo maneja el HX711 (entrada de nuestro lado): ahi vamos a leer
    // tanto el "esta listo" (hx711_listo) como los 24 bits del dato.
    gpio_reset_pin(pin_dout);
    gpio_set_direction(pin_dout, GPIO_MODE_INPUT);
    ESP_LOGI(TAG, "init: DOUT (GPIO%d) configurado como entrada, nivel actual=%d",
             pin_dout, gpio_get_level(pin_dout));

    // Secuencia de reset: subir SCK y mantenerlo en alto mas de 60us fuerza
    // al chip a power-down; al bajarlo de nuevo, arranca (o rearranca) desde
    // un estado conocido. Util si el HX711 quedo "colgado" de un reset
    // anterior del ESP32 a mitad de una lectura.
    ESP_LOGI(TAG, "init: reset - subiendo SCK por %dus", HX711_RESET_PULSO_US);
    gpio_set_level(pin_sck, 1);
    esp_rom_delay_us(HX711_RESET_PULSO_US);
    ESP_LOGI(TAG, "init: reset - bajando SCK, chip deberia estar despertando");
    gpio_set_level(pin_sck, 0);

    // El datasheet pide ~400ms de settling despues de encender/resetear
    // para que la primera lectura sea confiable.
    vTaskDelay(pdMS_TO_TICKS(400));
    ESP_LOGI(TAG, "init: listo. DOUT=%d (0 = ya hay dato disponible)", gpio_get_level(pin_dout));
}

// El HX711 avisa que tiene un dato nuevo poniendo DOUT en bajo. Mientras
// esta ocupado midiendo, DOUT queda en alto.
bool hx711_listo(hx711_t *h)
{
    return gpio_get_level(h->pin_dout) == 0;
}

bool hx711_leer(hx711_t *h, int32_t *valor_crudo, uint32_t timeout_ms)
{
    // Espera activa (con timeout) a que DOUT baje. Si el chip no esta
    // conectado o no responde, esto vuelve false despues de 'timeout_ms'
    // en vez de colgar el programa para siempre.
    TickType_t inicio = xTaskGetTickCount();
    while (!hx711_listo(h)) {
        if ((xTaskGetTickCount() - inicio) > pdMS_TO_TICKS(timeout_ms)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // 24 pulsos de reloj = 24 bits del dato, MSB primero: en cada pulso
    // subimos SCK, leemos el bit de DOUT, y bajamos SCK de nuevo.
    uint32_t dato = 0;
    for (int i = 0; i < 24; i++) {
        gpio_set_level(h->pin_sck, 1);
        esp_rom_delay_us(HX711_PULSO_US);
        dato = (dato << 1) | (gpio_get_level(h->pin_dout) ? 1 : 0);
        gpio_set_level(h->pin_sck, 0);
        esp_rom_delay_us(HX711_PULSO_US);
    }

    // 25° pulso: fija canal A / ganancia 128 para la proxima lectura
    // (el mismo modo que usaba scale.begin() en el codigo anterior).
    gpio_set_level(h->pin_sck, 1);
    esp_rom_delay_us(HX711_PULSO_US);
    gpio_set_level(h->pin_sck, 0);
    esp_rom_delay_us(HX711_PULSO_US);

    // Extiende el signo: el HX711 manda 24 bits en complemento a 2.
    if (dato & 0x800000) {
        dato |= 0xFF000000;
    }
    *valor_crudo = (int32_t)dato;
    return true;
}

bool hx711_leer_promedio(hx711_t *h, uint8_t n, int32_t *valor_promedio, uint32_t timeout_ms)
{
    if (n == 0) {
        return false;
    }
    int64_t suma = 0;
    for (uint8_t i = 0; i < n; i++) {
        int32_t v;
        if (!hx711_leer(h, &v, timeout_ms)) {
            return false;
        }
        suma += v;
    }
    *valor_promedio = (int32_t)(suma / n);
    return true;
}

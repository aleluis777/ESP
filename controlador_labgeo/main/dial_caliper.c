// Port directo de la funcion dial() de labgeo2025.ino/LABGEO2.ino (Arduino),
// que ya funcionaba en produccion -- ver dial_caliper.h para el detalle del
// protocolo (13 digitos BCD, dato leido tras el flanco de bajada de CLK).
//
// IMPORTANTE sobre los tiempos: el CLK lo genera el calibre, no el ESP32 --
// nosotros solo lo sondeamos (polling). Por eso la espera entre cada lectura
// de gpio_get_level() no es un detalle cosmetico: hay que sondear al mismo
// ritmo que el codigo original ya probado (10us entre polls, timeout de
// 100000 intentos por cada semiflanco, ~2000 ciclos vacios de asentamiento
// antes de leer DATA) en vez de un timeout generico por wall-clock. Sondear
// mas rapido o mas lento que eso puede hacer perder flancos o leer DATA
// antes de que se estabilice.

#include "dial_caliper.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DIAL_REQ_ACTIVO_ALTO 1 // el original hace digitalWrite(REQ, 1) para pedir lectura

#define DIAL_TIMEOUT_ITERACIONES 100000 // igual que el .ino probado (timeout=100000)
#define DIAL_POLL_DELAY_US       10     // igual que el .ino probado (delayMicroseconds(10))
#define DIAL_SETTLE_ITERACIONES  2000   // igual que el .ino probado (timeout=2000; while((timeout--)!=0);)

static inline void req_activar(dial_caliper_t *d)
{
    gpio_set_level(d->pin_req, DIAL_REQ_ACTIVO_ALTO ? 1 : 0);
}

static inline void req_liberar(dial_caliper_t *d)
{
    gpio_set_level(d->pin_req, DIAL_REQ_ACTIVO_ALTO ? 0 : 1);
}

void dial_caliper_init(dial_caliper_t *d, gpio_num_t pin_req, gpio_num_t pin_clk, gpio_num_t pin_data)
{
    d->pin_req = pin_req;
    d->pin_clk = pin_clk;
    d->pin_data = pin_data;

    gpio_reset_pin(pin_req);
    gpio_set_direction(pin_req, GPIO_MODE_OUTPUT);
    req_liberar(d);

    gpio_reset_pin(pin_clk);
    gpio_set_direction(pin_clk, GPIO_MODE_INPUT);

    gpio_reset_pin(pin_data);
    gpio_set_direction(pin_data, GPIO_MODE_INPUT);
}

bool dial_caliper_leer_digitos(dial_caliper_t *d, char digitos[13], uint32_t timeout_ms)
{
    // El timeout real ahora es por semiflanco (DIAL_TIMEOUT_ITERACIONES x
    // DIAL_POLL_DELAY_US ~ 1s por semiflanco), igual que el .ino probado --
    // timeout_ms queda sin usar a proposito, se mantiene en la firma para no
    // romper a quien ya llama a esta funcion.
    (void)timeout_ms;

    req_activar(d);

    for (int i = 0; i < 13; i++) {
        int k = 0;

        for (int j = 0; j < 4; j++) {
            uint32_t timeout = DIAL_TIMEOUT_ITERACIONES;

            // Espera a que CLK suba (estaba en reposo bajo). El sondeo cada
            // DIAL_POLL_DELAY_US es igual de importante que el timeout: es
            // el ritmo con el que ya se probo que el calibre responde bien.
            // Cada 1000 iteraciones (~10ms de trabajo) se cede el CPU con
            // vTaskDelay(1): sin esto, un dial desconectado (CLK sin
            // flancos) deja esta tarea en espera activa pura por ~1s de
            // corrido y, al correr en una prioridad mayor a la de IDLE,
            // esa tarea nunca llega a ejecutar -- el Task Watchdog de
            // ESP-IDF termina reseteando el equipo a los ~5s.
            while (gpio_get_level(d->pin_clk) == 0) {
                esp_rom_delay_us(DIAL_POLL_DELAY_US);
                if ((timeout--) == 0) {
                    req_liberar(d);
                    return false;
                }
                if ((timeout % 1000) == 0) {
                    vTaskDelay(1);
                }
            }

            timeout = DIAL_TIMEOUT_ITERACIONES;

            // Espera a que CLK vuelva a bajar -- el dato se lee DESPUES de
            // este flanco de bajada (asi lo hacia el codigo original). Mismo
            // motivo que arriba para el vTaskDelay(1) periodico.
            while (gpio_get_level(d->pin_clk) == 1) {
                esp_rom_delay_us(DIAL_POLL_DELAY_US);
                if ((timeout--) == 0) {
                    req_liberar(d);
                    return false;
                }
                if ((timeout % 1000) == 0) {
                    vTaskDelay(1);
                }
            }

            // Espera de asentamiento antes de leer DATA -- mismo bucle vacio
            // de ~2000 iteraciones que el original (no un delay fijo en us),
            // marcado volatile para que el compilador no lo elimine.
            volatile uint32_t espera = DIAL_SETTLE_ITERACIONES;
            while ((espera--) != 0) {
                ;
            }

            int bit = gpio_get_level(d->pin_data);
            if (bit) {
                switch (j) {
                case 0: k = 1; break;
                case 1: k = 2 + k; break;
                case 2: k = 4 + k; break;
                case 3: k = 8 + k; break;
                }
            }
            if (k == 15) {
                k = 0; // mismo caso especial que el codigo original (digito "en blanco")
            }
        }

        digitos[i] = (char)(k + '0');
    }

    req_liberar(d);
    return true;
}

bool dial_caliper_leer(dial_caliper_t *d, int32_t *valor_um, uint32_t timeout_ms)
{
    char digitos[13];
    if (!dial_caliper_leer_digitos(d, digitos, timeout_ms)) {
        return false;
    }

    // Los digitos 6..10 forman "DD.DDD" (mm con 3 decimales) -> ya es
    // resolucion de micrometros, sin reescalar.
    int32_t entero = (digitos[6] - '0') * 10 + (digitos[7] - '0');
    int32_t decimales = (digitos[8] - '0') * 100 + (digitos[9] - '0') * 10 + (digitos[10] - '0');

    *valor_um = entero * 1000 + decimales;
    return true;
}

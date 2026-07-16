// Port directo de la funcion dial() de labgeo2025.ino/LABGEO2.ino (Arduino),
// que ya funcionaba en produccion -- ver dial_caliper.h para el detalle del
// protocolo (13 digitos BCD, dato leido tras el flanco de bajada de CLK).
//
// IMPORTANTE sobre los tiempos: el CLK lo genera el calibre, no el ESP32 --
// nosotros solo lo sondeamos (polling). Por eso la espera entre cada lectura
// de gpio_get_level() no es un detalle cosmetico: hay que sondear al mismo
// ritmo que el codigo original ya probado (delayMicroseconds(1) entre polls,
// timeout de 100000 intentos por cada semiflanco, 2000 ciclos vacios de
// asentamiento antes de leer DATA) en vez de un timeout generico por
// wall-clock. Sondear mas rapido o mas lento que eso puede hacer perder
// flancos o leer DATA antes de que se estabilice -- por eso NO hay ningun
// vTaskDelay/yield dentro de este sondeo (una version anterior lo tenia
// cada 100 iteraciones, pero un semiflanco real del calibre suele durar mas
// que esas 100 iteraciones, asi que en la practica se disparaba tambien en
// lecturas sanas, no solo con el dial desconectado -- introduciendo una
// pausa de varios ms de FreeRTOS en medio del protocolo, justo lo que este
// comentario advierte que no hay que hacer). La proteccion contra inanicion
// del IDLE/Watchdog vive en el llamador (tarea_dial1/tarea_dial2 en
// app_main.c), no aca adentro -- ver ese archivo.
//
// NOTA: se probo una version por interrupcion de GPIO (ver historial/notas
// de la conversacion) y se descarto -- sin ningun filtro de rebote era
// mas rapida que una transaccion real (contaba rebote como bits), y ni con
// debounce por software se estabilizo. Esta version por sondeo es la que
// esta confirmada funcionando contra el hardware real.

#include "dial_caliper.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>

static const char *TAG = "DIAL_CALIPER";

#define DIAL_REQ_ACTIVO_ALTO 1 // el original hace digitalWrite(REQ, 1) para pedir lectura

#define DIAL_TIMEOUT_ITERACIONES 100000 // igual que el .ino probado (timeout=100000)
#define DIAL_POLL_DELAY_US       3      // igual que el .ino probado (delayMicroseconds(1))
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
    gpio_set_pull_mode(pin_clk, GPIO_PULLUP_ONLY); // igual que INPUT_PULLUP en el .ino probado

    gpio_reset_pin(pin_data);
    gpio_set_direction(pin_data, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin_data, GPIO_PULLUP_ONLY); // igual que INPUT_PULLUP en el .ino probado
}

// 'margen_minimo_out' (puede ser NULL) devuelve, de los 104 semiflancos de
// la lectura, cuantas iteraciones le quedaban al mas ajustado antes de
// timeoutear (DIAL_TIMEOUT_ITERACIONES = margen comodo, 0 = casi timeoutea).
// Es diagnostico puro -- sirve para ver si una lectura que salio "bien"
// (no hubo timeout) igual estuvo pasando muy justa en algun flanco puntual.
// No se loguea nada DENTRO del sondeo (romperia el timing); solo se guarda
// el numero minimo visto, un simple compare, y se devuelve al terminar.
bool dial_caliper_leer_digitos_dbg(dial_caliper_t *d, char digitos[13], uint32_t timeout_ms,
                                    uint32_t *margen_minimo_out)
{
    (void)timeout_ms;

    uint32_t margen_minimo = DIAL_TIMEOUT_ITERACIONES;

    req_activar(d);

    for (int i = 0; i < 13; i++) {
        int k = 0;

        for (int j = 0; j < 4; j++) {
            uint32_t timeout = DIAL_TIMEOUT_ITERACIONES;

            // Espera a que CLK suba (estaba en reposo bajo). El sondeo cada
            // DIAL_POLL_DELAY_US es igual de importante que el timeout: es
            // el ritmo con el que ya se probo que el calibre responde bien
            // (identico al .ino, sin ningun yield en el medio -- ver el
            // comentario grande al principio del archivo).
            while (gpio_get_level(d->pin_clk) == 0) {
                esp_rom_delay_us(DIAL_POLL_DELAY_US);
                if ((timeout--) == 0) {
                    req_liberar(d);
                    return false;
                }
            }
            if (timeout < margen_minimo) {
                margen_minimo = timeout;
            }

            timeout = DIAL_TIMEOUT_ITERACIONES;

            // Espera a que CLK vuelva a bajar -- el dato se lee DESPUES de
            // este flanco de bajada (asi lo hacia el codigo original).
            while (gpio_get_level(d->pin_clk) == 1) {
                esp_rom_delay_us(DIAL_POLL_DELAY_US);
                if ((timeout--) == 0) {
                    req_liberar(d);
                    return false;
                }
            }
            if (timeout < margen_minimo) {
                margen_minimo = timeout;
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
    if (margen_minimo_out) {
        *margen_minimo_out = margen_minimo;
    }
    return true;
}

bool dial_caliper_leer_digitos(dial_caliper_t *d, char digitos[13], uint32_t timeout_ms)
{
    // El timeout real ahora es por semiflanco (DIAL_TIMEOUT_ITERACIONES x
    // DIAL_POLL_DELAY_US ~ 1s por semiflanco), igual que el .ino probado --
    // timeout_ms queda sin usar a proposito, se mantiene en la firma para no
    // romper a quien ya llama a esta funcion.
    return dial_caliper_leer_digitos_dbg(d, digitos, timeout_ms, NULL);
}

#define DIAL_CONSENSO_MAX_INTENTOS 5 // hasta 5 lecturas para lograr 2 consecutivas iguales

// Pausa entre pedidos de lectura consecutivos (REQ liberado -> REQ activado
// de nuevo). Confirmado con logs reales: pedir una lectura nueva
// INMEDIATAMENTE despues de terminar la anterior (sin ninguna pausa) hacia
// que el calibre devolviera datos de una transmision a medio terminar --
// cada digito afectado salia exactamente el DOBLE del valor correcto (bit
// leido con el peso corrido una posicion: 1->2, 2->4, 5->10, 4->8), lo que
// indica que agarrabamos la cola de la transmision vieja en vez de una
// nueva y prolija. El primer intento de cada dial_caliper_leer() (que llega
// despues de la pausa de 500ms entre lecturas de la tarea que llama)
// siempre daba bien; el error aparecia justo en los reintentos del
// consenso, que antes no tenian ninguna pausa entre ellos.
#define DIAL_REQ_COOLDOWN_MS 20

// El bit-banging por software es sensible a ruido en la linea (una lectura
// puntual corrupta se ve como un valor completamente distinto, no como un
// "ruidito" chico) -- por eso no alcanza con leer una sola vez. Se exigen
// DOS lecturas seguidas que coincidan exactamente antes de aceptar el valor;
// si una lectura no coincide con la anterior, esa pasa a ser la nueva
// "anterior" candidata (no se descarta todo el proceso, solo se sigue
// intentando) hasta DIAL_CONSENSO_MAX_INTENTOS lecturas en total.
bool dial_caliper_leer(dial_caliper_t *d, int32_t *valor_um, uint32_t timeout_ms)
{
    int32_t anterior = 0;
    bool hay_anterior = false;

    for (int intento = 0; intento < DIAL_CONSENSO_MAX_INTENTOS; intento++) {
        if (intento > 0) {
            vTaskDelay(pdMS_TO_TICKS(DIAL_REQ_COOLDOWN_MS)); // deja asentar al calibre antes de pedir de nuevo
        }

        char digitos[13];
        uint32_t margen_minimo = 0;
        if (!dial_caliper_leer_digitos_dbg(d, digitos, timeout_ms, &margen_minimo)) {
            ESP_LOGW(TAG, "pin_clk=%d intento %d: TIMEOUT (algun semiflanco nunca llego)",
                     (int)d->pin_clk, intento);
            hay_anterior = false; // un timeout de por medio invalida la comparacion
            continue;
        }

        // Los digitos 6..10 forman "DD.DDD" (mm con 3 decimales) -> ya es
        // resolucion de micrometros, sin reescalar.
        int32_t entero = (digitos[6] - '0') * 10 + (digitos[7] - '0');
        int32_t decimales = (digitos[8] - '0') * 100 + (digitos[9] - '0') * 10 + (digitos[10] - '0');
        int32_t actual = entero * 1000 + decimales;

        // Log DIAGNOSTICO: los 13 digitos crudos (no solo los 5 que se usan)
        // y el margen minimo (cuantas iteraciones le quedaban al semiflanco
        // mas ajustado antes de timeoutear -- bajo = paso muy justo, aunque
        // no haya llegado a timeoutear del todo). Se loguea DESPUES de
        // req_liberar(), fuera del sondeo, no afecta el timing.
        ESP_LOGI(TAG, "pin_clk=%d intento %d: digitos='%.13s' valor_um=%" PRId32 " margen_min=%" PRIu32,
                 (int)d->pin_clk, intento, digitos, actual, margen_minimo);

        if (hay_anterior && actual == anterior) {
            *valor_um = actual;
            return true;
        }

        anterior = actual;
        hay_anterior = true;
    }

    return false; // no hubo 2 lecturas consecutivas iguales en DIAL_CONSENSO_MAX_INTENTOS intentos
}

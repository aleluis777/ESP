// Driver bit-bang del protocolo de calibre/dial digital (REQ + CLK + DATA).
// A diferencia del HX711 (protocolo documentado y estandar), este es el
// protocolo generico que usan los calibres digitales baratos, reconstruido
// de memoria -- NO esta validado contra el hardware real todavia. Ver el
// comentario grande en dial_caliper.h antes de confiar en los valores que
// devuelve.
//
// Idea general: REQ es una salida nuestra que "despierta" al calibre (via
// un transistor en la placa). Una vez despierto, el calibre mismo genera
// los pulsos de CLK (nosotros solo escuchamos, no los generamos) y va
// poniendo cada bit en DATA; nosotros leemos DATA en cada flanco de CLK.

#include "dial_caliper.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

// ---------- Parametros a ajustar contra el hardware real (ver dial_caliper.h) ----------
#define DIAL_REQ_ACTIVO_ALTO       1     // 1: REQ en alto activa el transistor. 0: REQ en bajo lo activa.
#define DIAL_BIT_ORDEN_LSB_PRIMERO 1     // 1: primer bit clockeado = LSB. 0: primer bit = MSB.
#define DIAL_ESPERA_DESPERTAR_US   20000 // tiempo entre activar REQ y empezar a esperar el primer flanco
#define DIAL_ASENTAMIENTO_US       5     // pausa tras el flanco de CLK antes de leer DATA

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

bool dial_caliper_leer_crudo(dial_caliper_t *d, uint32_t *dato_crudo, uint32_t timeout_ms)
{
    // "Despierta" al calibre y le da tiempo a arrancar antes de esperar el
    // primer pulso de reloj (los calibres suelen tardar en salir del modo
    // ahorro de energia).
    req_activar(d);
    esp_rom_delay_us(DIAL_ESPERA_DESPERTAR_US);

    int64_t inicio = esp_timer_get_time();
    int64_t timeout_us = (int64_t)timeout_ms * 1000;
    uint32_t dato = 0;
    bool ok = true;

    // El calibre es quien genera los 24 pulsos de reloj (no nosotros, a
    // diferencia del HX711): en cada vuelta esperamos un flanco de subida,
    // leemos el bit, y esperamos a que baje de nuevo antes de ir por el
    // siguiente.
    for (int i = 0; i < 24 && ok; i++) {
        // Espera flanco de subida (CLK en reposo bajo).
        while (gpio_get_level(d->pin_clk) == 0) {
            if (esp_timer_get_time() - inicio > timeout_us) { ok = false; break; }
        }
        if (!ok) break;

        esp_rom_delay_us(DIAL_ASENTAMIENTO_US);
        uint32_t bit = gpio_get_level(d->pin_data) ? 1u : 0u;
#if DIAL_BIT_ORDEN_LSB_PRIMERO
        dato |= (bit << i);
#else
        dato = (dato << 1) | bit;
#endif

        // Espera a que el reloj vuelva a bajo antes del proximo flanco.
        while (gpio_get_level(d->pin_clk) == 1) {
            if (esp_timer_get_time() - inicio > timeout_us) { ok = false; break; }
        }
    }

    req_liberar(d);

    if (!ok) {
        return false;
    }
    *dato_crudo = dato;
    return true;
}

bool dial_caliper_leer(dial_caliper_t *d, int32_t *valor_centesimas_mm, uint32_t timeout_ms)
{
    uint32_t dato;
    if (!dial_caliper_leer_crudo(d, &dato, timeout_ms)) {
        return false;
    }

    // Layout asumido: bit 20 = signo, bits 0-19 = magnitud en centesimas de
    // mm. Ver el comentario grande en dial_caliper.h antes de confiar en
    // esto sin validarlo contra el calibre real.
    int32_t magnitud = (int32_t)(dato & 0xFFFFFu);
    bool negativo = (dato & 0x100000u) != 0;
    *valor_centesimas_mm = negativo ? -magnitud : magnitud;
    return true;
}

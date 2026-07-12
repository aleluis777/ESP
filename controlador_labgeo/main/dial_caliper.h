#pragma once

// Driver para los "diales" digitales (calibre con 3 lineas REQ/CLK/DATA +
// GND, REQ atado a un transistor que simula el boton de encendido/dato del
// calibre). Protocolo confirmado contra el codigo Arduino real que ya
// funcionaba (funcion dial() de labgeo2025.ino / LABGEO2.ino), portado tal
// cual -- no es un protocolo generico adivinado.
//
// Como es el protocolo real (distinto del "24 bits binarios" que se ve en
// otros calibres chinos):
//   - El calibre manda 13 "digitos" de 4 bits cada uno (52 flancos en
//     total). Cada grupo de 4 bits es un digito BCD (peso 1,2,4,8 en el
//     orden que se clockea, primero el bit menos significativo).
//   - El dato (DATA) se lee DESPUES de que el reloj (CLK) vuelve a bajo,
//     no cerca de cuando sube -- esto es lo que probablemente fallaba en
//     la version anterior de este driver.
//   - Los digitos que forman el numero final son los indices 6 a 10 (0
//     hasta 12 en total), con formato "DD.DDD" (2 enteros + 3 decimales) ->
//     osea que el valor nativo del calibre ya viene en resolucion de
//     micrometros (0.001mm), sin necesidad de reescalar.
//   - Esta version no extrae el signo (los digitos 0-5/11-12, que no se
//     usan para el valor, podrian tener esa info si hiciera falta negativos
//     mas adelante).
//
// Timing: el CLK lo genera el calibre, no el ESP32 -- este driver solo lo
// sondea (polling), asi que el ritmo de sondeo importa tanto como la logica
// de bits. Se usan los mismos numeros que el .ino ya probado y no un timeout
// generico por wall-clock: 100000 intentos por semiflanco con 10us entre
// cada intento (~1s maximo de espera por semiflanco), y ~2000 ciclos vacios
// de asentamiento antes de leer DATA tras el flanco de bajada. El parametro
// timeout_ms de las funciones de abajo queda sin usar por este motivo (se
// mantiene en la firma por compatibilidad).

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t pin_req;
    gpio_num_t pin_clk;
    gpio_num_t pin_data;
} dial_caliper_t;

void dial_caliper_init(dial_caliper_t *d, gpio_num_t pin_req, gpio_num_t pin_clk, gpio_num_t pin_data);

// Lee los 13 digitos BCD crudos (sin decodificar), un caracter '0'-'9' por
// posicion (igual que 'mydata' en el codigo Arduino de referencia). Util
// para depurar si el valor final no cierra: se pueden ver todos los
// digitos, no solo los 5 que se usan.
bool dial_caliper_leer_digitos(dial_caliper_t *d, char digitos[13], uint32_t timeout_ms);

// Lee y devuelve la posicion en micrometros (sin signo por ahora), tomando
// los digitos 6-10 ("DD.DDD" mm) del calibre.
bool dial_caliper_leer(dial_caliper_t *d, int32_t *valor_um, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

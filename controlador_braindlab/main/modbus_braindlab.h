#pragma once

// Maestro Modbus RTU por RS-485 hacia el medidor de energia JSY-MK-333G
// (ver E:\ESP\JSY-MK-333G_monofasico_modbus.md). Se leen los 3 voltajes y las 3 corrientes de los canales A/B/C, que en
// este equipo son las fases R/S/T.
//
// Hardware (HARDWARE.md §4.3, SP3485 U12): UART2, TX=GPIO33, RX=GPIO36,
// EN_485=GPIO32 (DE + /RE). El UART va en modo RS-485 half-duplex: el
// propio periferico maneja EN_485 (pin RTS) y lo baja recien cuando salio
// el ultimo bit -- no hay que hacer la secuencia a mano.
//
// No bloquea a nadie: tarea_modbus es la UNICA que habla con el medidor (y
// la unica que espera si tarda o no esta). El resto lee la ultima lectura
// buena con modbus_braindlab_leer(), que es una copia inmediata.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MODBUS_NUM_FASES 3 // R, S, T (canales A, B, C del medidor)

typedef struct {
    bool ok;                 // false si no hubo lectura buena en los ultimos MODBUS_VIGENCIA_MS
    float voltajes[MODBUS_NUM_FASES];   // R, S, T [V]
    float corrientes[MODBUS_NUM_FASES]; // R, S, T [A]
    uint32_t edad_ms;        // hace cuanto se leyo (si ok=false, la ultima lectura que hubo)
} modbus_lectura_t;

// Configura UART2 en modo RS-485 y lanza tarea_modbus. No espera al medidor:
// si no esta conectado devuelve ESP_OK igual y la lectura queda en ok=false.
esp_err_t modbus_braindlab_init(void);

// Copia la ultima lectura en 'lectura'. Nunca bloquea mas que un mutex de
// microsegundos -- seguro de llamar desde cualquier task.
void modbus_braindlab_leer(modbus_lectura_t *lectura);

#ifdef __cplusplus
}
#endif

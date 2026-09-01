#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Levanta el modulo W5500 (SPI, por POLLING -- el IRQ no esta cableado en
// esta placa, ver HARDWARE.md 4.1) y el stack de red (esp_netif) con IP
// estatica, usando el componente oficial "espressif/ethernet_init" (ver
// idf_component.yml). Llamar antes de servidor_web_init().
esp_err_t red_eth_init(void);

// Estado actual del link fisico (cable conectado y negociado del otro lado),
// segun el ultimo evento ETHERNET_EVENT_CONNECTED/DISCONNECTED visto por
// on_eth_event() -- no hace ninguna consulta nueva al chip, solo devuelve el
// ultimo estado conocido. false tambien antes de que red_eth_init() termine
// o si nunca hubo link.
bool red_eth_esta_conectado(void);

#ifdef __cplusplus
}
#endif

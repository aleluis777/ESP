#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Levanta el modulo W5500 (SPI) y el stack de red (esp_netif) con IP
// estatica, usando el componente oficial "espressif/ethernet_init" (ver
// idf_component.yml). Llamar antes de servidor_web_init().
esp_err_t red_eth_init(void);

#ifdef __cplusplus
}
#endif

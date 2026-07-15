#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Loguea (ESP_LOGI) cada ping ICMP (echo request) que le llega al
// controlador por Ethernet, con la IP de quien lo mando. No contesta nada --
// eso ya lo hace lwIP solo, esto es puramente un observador para ver en el
// monitor serie cuando alguien hace "ping <ip-del-controlador>" desde una PC.
// Llamar despues de que el netif de Ethernet este arriba (ver app_main.c).
esp_err_t ping_monitor_init(void);

#ifdef __cplusplus
}
#endif

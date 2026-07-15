// Observador de pings ICMP entrantes. No tiene nada que ver con el servidor
// HTTP (servidor_web.c) -- es su propio modulo porque trabaja un nivel mas
// abajo, directo sobre un socket RAW de lwIP, no sobre esp_http_server.
//
// Como funciona: un socket AF_INET/SOCK_RAW/IPPROTO_ICMP recibe una COPIA de
// todo paquete ICMP que le llega a la placa (ademas de que lwIP, por su
// cuenta y sin que este modulo haga nada, le sigue contestando el echo reply
// al que hizo ping -- este socket solo mira, no responde). El buffer que
// devuelve recvfrom() trae el paquete IP completo: primero la cabecera IP
// (con su largo variable, por eso IPH_HL_BYTES) y recien despues la cabecera
// ICMP (struct icmp_echo_hdr: type/code/checksum/id/seqno).

#include <errno.h>
#include "ping_monitor.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/prot/ip4.h"
#include "lwip/prot/icmp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "PING_MONITOR";

static void tarea_ping_monitor(void *arg)
{
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0) {
        ESP_LOGE(TAG, "No se pudo abrir el socket raw ICMP (errno %d)", errno);
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[64];
    while (1) {
        struct sockaddr_in origen;
        socklen_t origen_len = sizeof(origen);
        int leidos = recvfrom(sock, buf, sizeof(buf), 0,
                               (struct sockaddr *)&origen, &origen_len);
        if (leidos <= 0) {
            continue;
        }

        const struct ip_hdr *ip = (const struct ip_hdr *)buf;
        int ip_hdr_len = IPH_HL_BYTES(ip);
        if (leidos < ip_hdr_len + (int)sizeof(struct icmp_echo_hdr)) {
            continue; // paquete truncado o mas chico de lo esperado, se ignora
        }

        const struct icmp_echo_hdr *icmp = (const struct icmp_echo_hdr *)(buf + ip_hdr_len);
        if (icmp->type == ICMP_ECHO) { // type 8 = echo request, el "ping" en si
            ESP_LOGI(TAG, "Ping (echo request) <- %s  id=%u seq=%u",
                     inet_ntoa(origen.sin_addr),
                     lwip_ntohs(icmp->id), lwip_ntohs(icmp->seqno));
        }
    }
}

esp_err_t ping_monitor_init(void)
{
    if (xTaskCreate(tarea_ping_monitor, "ping_monitor", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear la tarea de monitoreo de ping");
        return ESP_FAIL;
    }
    return ESP_OK;
}

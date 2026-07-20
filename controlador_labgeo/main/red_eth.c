// Levanta el W5500 (Ethernet por SPI) y el stack de red (esp_netif) con IP
// estatica. El driver del chip en si (esp_eth_mac_new_w5500 / phy_new_w5500)
// no viene incluido en ESP-IDF desde la v5.x -- se usa el componente
// manejado "espressif/ethernet_init" (idf_component.yml), que es el que
// recomienda la guia de migracion oficial de Espressif para este caso.
//
// OJO: casi toda la configuracion especifica del W5500 (que pines son
// CS/RST/SCLK/MISO/MOSI, y que este en modo polling porque el INT no esta
// cableado) NO esta en este .c -- vive en sdkconfig.defaults como opciones
// de Kconfig del componente ethernet_init (CONFIG_ETHERNET_SPI_*). Si algun
// dia hay que cambiar esos pines, es ahi donde hay que tocar, no aca.
// ethernet_init_all() simplemente arma el driver ya configurado segun esas
// opciones -- este archivo solo lo llama y conecta el resultado a esp_netif.

#include <string.h>
#include "red_eth.h"
#include "ethernet_init.h"
#include "esp_eth.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "RED_ETH";

// Mismo pin que CONFIG_ETHERNET_SPI_PHY_RST0_GPIO en sdkconfig.defaults --
// esta constante es solo para el pulso de diagnostico de abajo, el manejo
// "real" del reset durante el funcionamiento normal lo hace el componente
// ethernet_init internamente segun ese Kconfig.
#define DIAG_W5500_RST_GPIO GPIO_NUM_5

// Datos confirmados en labgeo2025.ino (macArray / ipArray / gatewayArray / subnetArray).
static uint8_t s_mac[6] = { 0xDE, 0xED, 0xBA, 0x2E, 0x85, 0x38 };

// Pulso de reset manual y logueado, solo para diagnostico: nos deja ver en
// el monitor que el pin RST del W5500 responde (medible con multimetro/LED)
// ANTES de que ethernet_init_all() tome el control real del pin. El W5500
// resetea con RST en bajo (activo bajo); lo soltamos (alto) antes de seguir.
// Al final se le devuelve el pin "en blanco" con gpio_reset_pin() para que
// el componente lo configure el desde cero sin interferencias.
static void diag_pulso_reset_w5500(void)
{
    ESP_LOGI(TAG, "diag: configurando RST (GPIO%d) como salida", DIAG_W5500_RST_GPIO);
    gpio_reset_pin(DIAG_W5500_RST_GPIO);
    gpio_set_direction(DIAG_W5500_RST_GPIO, GPIO_MODE_OUTPUT);

    ESP_LOGI(TAG, "diag: RST en alto (reposo)");
    gpio_set_level(DIAG_W5500_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "diag: RST en bajo (reset asertado)");
    gpio_set_level(DIAG_W5500_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "diag: RST en alto de nuevo (reset liberado)");
    gpio_set_level(DIAG_W5500_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50)); // tiempo de arranque del W5500 tras soltar RST

    gpio_reset_pin(DIAG_W5500_RST_GPIO); // devuelve el pin en blanco
    ESP_LOGI(TAG, "diag: pulso de reset terminado, pin devuelto sin configurar");
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet: link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet: link down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet: iniciado");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGW(TAG, "Ethernet: detenido");
        break;
    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    // OJO: este evento (IP_EVENT_ETH_GOT_IP) solo lo dispara el cliente
    // DHCP al conseguir una IP. Como acá usamos IP estatica con el DHCP
    // apagado, este handler NUNCA se va a llamar -- es normal no verlo en
    // el log. La confirmacion real de la IP estatica esta en
    // verificar_ip_activa(), que se llama directo desde red_eth_init().
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "IP asignada por DHCP: " IPSTR, IP2STR(&event->ip_info.ip));
}

// Lee de vuelta la IP que esp_netif tiene activa AHORA MISMO (no lo que
// nosotros pedimos, sino lo que realmente quedo aplicado) y la loguea. Sirve
// para descartar que algo (por ejemplo, el manejo interno de "conectado" del
// glue de esp_eth) haya pisado la IP estatica que configuramos.
static void verificar_ip_activa(esp_netif_t *eth_netif, const char *momento)
{
    esp_netif_ip_info_t actual;
    if (esp_netif_get_ip_info(eth_netif, &actual) == ESP_OK) {
        ESP_LOGI(TAG, "[%s] IP activa: " IPSTR " / mascara " IPSTR " / gw " IPSTR,
                 momento, IP2STR(&actual.ip), IP2STR(&actual.netmask), IP2STR(&actual.gw));
    } else {
        ESP_LOGW(TAG, "[%s] No se pudo leer la IP activa", momento);
    }
}

esp_err_t red_eth_init(void)
{
    // esp_netif es el stack de red de ESP-IDF (equivalente a lo que hacia
    // Ethernet.h de Arduino); esp_event es el bus de eventos donde se
    // avisan cosas como "se conecto el cable" o "ya tengo IP".
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // El driver del W5500 usa el pin de INT (GPIO26, ver sdkconfig.defaults)
    // via gpio_isr_handler_add(), que requiere que el servicio de ISR de GPIO
    // ya este instalado -- si no, esa llamada falla callada (no chequea el
    // retorno) y la interrupcion nunca queda enganchada. ESP_ERR_INVALID_STATE
    // significa que ya estaba instalado por otra parte, no es un error real.
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_err);
    }

    diag_pulso_reset_w5500();

    // ethernet_init_all() lee la config de Kconfig (sdkconfig.defaults,
    // CONFIG_ETHERNET_SPI_*) y arma el driver del W5500 ya listo para usar
    // -- SPI, chip select, reset, todo. Si devuelve 0 modulos, algo esta
    // mal en esa config (o el chip no responde en el bus SPI).
    ESP_LOGI(TAG, "Llamando a ethernet_init_all()...");
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles = NULL;
    esp_err_t err = ethernet_init_all(&eth_handles, &eth_port_cnt);
    if (err != ESP_OK || eth_port_cnt == 0) {
        ESP_LOGE(TAG, "No se pudo inicializar el W5500 (%s)", esp_err_to_name(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ethernet_init_all() OK, %u modulo(s) detectado(s)", (unsigned)eth_port_cnt);

    // La MAC no la asigna el chip solo -- se la mandamos explicitamente para
    // que sea siempre la misma que usaba labgeo2025.ino (util si el router
    // tiene una reserva de IP fija por MAC). OJO: esto tiene que pasar ANTES
    // de esp_netif_attach() de abajo -- ese attach dispara internamente
    // esp_eth_post_attach() (ver esp_eth_netif_glue.c del propio ESP-IDF),
    // que LEE el MAC que tenga el chip en ese instante (ETH_CMD_G_MAC_ADDR) y
    // lo copia a esp_netif via esp_netif_set_mac(), una sola vez. Si el
    // ioctl de abajo se hiciera despues del attach (como estaba antes),
    // esp_netif se queda para siempre con el MAC auto-generado
    // (CONFIG_ETHERNET_SPI_AUTOCONFIG_MAC_ADDR0), mientras el chip ya
    // transmite tramas con el MAC fijo -- esa desincronizacion hace que lwIP
    // arme las respuestas ARP con un MAC que no es el que realmente sale en
    // la trama, y el ping deja de funcionar aunque el link y la IP esten OK.
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handles[0], ETH_CMD_S_MAC_ADDR, s_mac));

    // "Pega" el driver Ethernet (eth_handles[0]) a una interfaz de red
    // generica (esp_netif) -- recien con esto el resto de ESP-IDF (DHCP,
    // sockets, el servidor HTTP) puede usarlo como si fuera cualquier red.
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handles[0]);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, glue));

    // IP estatica (misma que labgeo2025.ino): 192.168.18.91 / 255.255.255.0 / gw 192.168.18.1
    // Hay que apagar el cliente DHCP antes de poder fijar la IP a mano.
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(eth_netif));
    esp_netif_ip_info_t ip_info = {
        .ip.addr = ESP_IP4TOADDR(192, 168, 5, 91),
        .gw.addr = ESP_IP4TOADDR(192, 168, 5, 1),
        .netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0),
    };
    ESP_ERROR_CHECK(esp_netif_set_ip_info(eth_netif, &ip_info));
    verificar_ip_activa(eth_netif, "justo despues de set_ip_info");

    // Estos dos handlers solo loguean lo que pasa (link up/down, IP
    // asignada) -- no cambian ningun comportamiento, son para poder ver en
    // el monitor serie si el Ethernet esta bien conectado.
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL));

    // Recien aca arranca de verdad el chip (antes solo estaba configurado,
    // no transmitiendo).
    ESP_ERROR_CHECK(esp_eth_start(eth_handles[0]));

    // Le da un momento al link para levantar y despues chequea de nuevo --
    // si esp_netif_action_connected() (que dispara internamente el glue al
    // detectar el link) llegara a resetear la IP por alguna razon, ahora se
    // veria distinto entre este log y el de arriba.
    vTaskDelay(pdMS_TO_TICKS(500));
    verificar_ip_activa(eth_netif, "500ms despues de esp_eth_start");

    return ESP_OK;
}

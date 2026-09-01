// Levanta el W5500 (Ethernet por SPI) y el stack de red (esp_netif) con IP
// estatica. Mismo patron que red_eth.c de controlador_labgeo: el driver del
// chip no viene incluido en ESP-IDF desde la v5.x, se usa el componente
// manejado "espressif/ethernet_init" (idf_component.yml).
//
// Diferencia clave con controlador_labgeo: en ESTA placa el pin IRQ del
// W5500 (pin 5 del modulo U6) esta SIN CONECTAR (ver HARDWARE.md 4.1), asi
// que el driver tiene que ir por POLLING -- eso ya esta resuelto en
// sdkconfig.defaults con CONFIG_ETHERNET_SPI_INT0_GPIO=-1, este .c no tiene
// que hacer nada especial al respecto, ethernet_init_all() lo lee solo.
// El resto de la config especifica (CS/RST/SCLK/MISO/MOSI) tambien vive en
// sdkconfig.defaults (CONFIG_ETHERNET_SPI_*) -- si hay que tocar pines, es
// ahi, no aca.

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

// Ultimo estado de link visto por on_eth_event() -- volatile porque se
// escribe desde la tarea del event loop de ESP-IDF y se lee desde
// tarea_climatizacion (app_main.c), sin ningun otro mecanismo de
// sincronizacion. Un bool de lectura/escritura simple no necesita mutex.
static volatile bool s_link_up = false;

// Mismo pin que CONFIG_ETHERNET_SPI_PHY_RST0_GPIO en sdkconfig.defaults --
// esta constante es solo para el pulso de diagnostico de abajo, el manejo
// "real" del reset durante el funcionamiento normal lo hace el componente
// ethernet_init internamente segun ese Kconfig.
#define DIAG_W5500_RST_GPIO GPIO_NUM_5

// MAC propia de este equipo (braindlab) -- localmente administrada (bit
// 0x02 en el primer byte), distinta de la que usa controlador_labgeo
// (DE:ED:BA:2E:85:38) para no chocar si ambos equipos comparten red. Si el
// sitio necesita una reserva de IP fija por MAC en el router, cambiar esto
// por la que corresponda.
static uint8_t s_mac[6] = { 0x02, 0xBD, 0x1A, 0xB1, 0x01, 0x01 };

// Pulso de reset manual y logueado, solo para diagnostico: deja ver en el
// monitor serie que el pin RST del W5500 responde ANTES de que
// ethernet_init_all() tome el control real del pin. El W5500 resetea con
// RST en bajo (activo bajo, ver HARDWARE.md); se le devuelve el pin "en
// blanco" con gpio_reset_pin() para que el componente lo configure desde
// cero sin interferencias.
static void diag_pulso_reset_w5500(void)
{
    ESP_LOGI(TAG, "diag: configurando RST (GPIO%d) como salida", DIAG_W5500_RST_GPIO);
    gpio_reset_pin(DIAG_W5500_RST_GPIO);
    gpio_set_direction(DIAG_W5500_RST_GPIO, GPIO_MODE_OUTPUT);

    gpio_set_level(DIAG_W5500_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "diag: RST en bajo (reset asertado)");
    gpio_set_level(DIAG_W5500_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    gpio_set_level(DIAG_W5500_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50)); // tiempo de arranque del W5500 tras soltar RST

    gpio_reset_pin(DIAG_W5500_RST_GPIO); // devuelve el pin en blanco
    ESP_LOGI(TAG, "diag: pulso de reset terminado, pin devuelto sin configurar");
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        s_link_up = true;
        ESP_LOGI(TAG, "Ethernet: link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        s_link_up = false;
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

// Con IP estatica y DHCP apagado este handler nunca se llama -- se registra
// igual por si algun dia se pasa a DHCP.
static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "IP asignada por DHCP: " IPSTR, IP2STR(&event->ip_info.ip));
}

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
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // El driver del W5500 instala su propio manejo de GPIO (aunque sea en
    // modo polling) via el servicio de ISR de GPIO -- si no esta instalado
    // todavia, lo instala aca. ESP_ERR_INVALID_STATE significa que ya lo
    // habia instalado otra parte, no es un error real.
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(isr_err);
    }

    diag_pulso_reset_w5500();

    ESP_LOGI(TAG, "Llamando a ethernet_init_all()...");
    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles = NULL;
    esp_err_t err = ethernet_init_all(&eth_handles, &eth_port_cnt);
    if (err != ESP_OK || eth_port_cnt == 0) {
        ESP_LOGE(TAG, "No se pudo inicializar el W5500 (%s)", esp_err_to_name(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ethernet_init_all() OK, %u modulo(s) detectado(s)", (unsigned)eth_port_cnt);

    // Mismo motivo que en controlador_labgeo: el MAC hay que fijarlo ANTES
    // de esp_netif_attach() (mas abajo), porque ese attach dispara
    // esp_eth_post_attach() que lee el MAC que tenga el chip en ese
    // instante y lo copia a esp_netif -- hacerlo despues deja a esp_netif
    // con el MAC auto-generado mientras el chip ya transmite con el fijo,
    // y el ping deja de responder por la desincronizacion.
    ESP_ERROR_CHECK(esp_eth_ioctl(eth_handles[0], ETH_CMD_S_MAC_ADDR, s_mac));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handles[0]);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, glue));

    // IP estatica -- mismos valores que RED_IP_DEFAULT/RED_GATEWAY_DEFAULT/
    // RED_MASCARA_DEFAULT en config_braindlab_defaults.h. Aplicar de verdad
    // lo que se guarda via /configurar_red (config_braindlab_guardar_red())
    // es un paso pendiente, igual que en controlador_labgeo.
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(eth_netif));
    esp_netif_ip_info_t ip_info = {
        .ip.addr = ESP_IP4TOADDR(192, 168, 5, 95),
        .gw.addr = ESP_IP4TOADDR(192, 168, 5, 1),
        .netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0),
    };
    ESP_ERROR_CHECK(esp_netif_set_ip_info(eth_netif, &ip_info));
    verificar_ip_activa(eth_netif, "justo despues de set_ip_info");

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL));

    ESP_ERROR_CHECK(esp_eth_start(eth_handles[0]));

    vTaskDelay(pdMS_TO_TICKS(500));
    verificar_ip_activa(eth_netif, "500ms despues de esp_eth_start");

    return ESP_OK;
}

bool red_eth_esta_conectado(void)
{
    return s_link_up;
}

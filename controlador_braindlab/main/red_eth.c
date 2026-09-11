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
#include "config_braindlab.h"
#include "config_braindlab_defaults.h" // RED_IP_DEFAULT/RED_GATEWAY_DEFAULT/RED_MASCARA_DEFAULT

static const char *TAG = "RED_ETH";

// Ultimo estado de link visto por on_eth_event() -- volatile porque se
// escribe desde la tarea del event loop de ESP-IDF y se lee desde
// tarea_climatizacion (app_main.c) y tarea_vigilancia_eth() (abajo), sin
// ningun otro mecanismo de sincronizacion. Un bool de lectura/escritura
// simple no necesita mutex.
static volatile bool s_link_up = false;

// Handles del driver W5500 + netif, guardados a nivel de modulo (en vez de
// locales de red_eth_init()) porque tarea_vigilancia_eth() los necesita
// para poder tirar todo abajo y reconstruirlo si el chip se cuelga (ver
// comentario de esa tarea, mas abajo).
static esp_eth_handle_t *s_eth_handles = NULL;
static uint8_t s_eth_port_cnt = 0;
static esp_netif_t *s_eth_netif = NULL;
static esp_eth_netif_glue_handle_t s_eth_glue = NULL;

// IP/mascara/gateway ya parseados, cargados de config.json UNA sola vez (ver
// cargar_ip_info() abajo) y cacheados aca -- iniciar_hardware_eth() la usa
// cada vez que corre (al arrancar, y de nuevo si tarea_vigilancia_eth() pide
// reiniciar_eth()) sin volver a tocar el archivo. config_braindlab_cargar_red()
// ya trae sus propios defaults (RED_IP_DEFAULT="192.168.5.95",
// RED_GATEWAY_DEFAULT="192.168.5.1", RED_MASCARA_DEFAULT="255.255.255.0" --
// ver config_braindlab_defaults.h) si el archivo no existe o le falta el
// campo, asi que no hace falta duplicarlos aca.
static esp_netif_ip_info_t s_ip_info;
static bool s_ip_info_cargada = false;

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

// Parsea un campo string ("192.168.5.95") de config_red_t a esp_ip4_addr_t.
// Si el string esta vacio o no es una IP valida (por ejemplo config.json
// corrupto o guardado a mano con algo raro), se queda con 'reserva' (el
// default duro de config_braindlab_defaults.h) y avisa por que.
static void parsear_campo_ip(const char *campo, const char *nombre_campo, esp_ip4_addr_t *destino,
                              const char *reserva)
{
    if (esp_netif_str_to_ip4(campo, destino) != ESP_OK) {
        ESP_LOGW(TAG, "config.json: \"%s\"=\"%s\" no es una IP valida, usando %s", nombre_campo, campo, reserva);
        esp_netif_str_to_ip4(reserva, destino); // el default siempre es valido, no hace falta chequear el resultado
    }
}

// Carga config_braindlab_cargar_red() UNA sola vez (config_braindlab_init()
// ya tiene que haber montado "www" para esta hora, se llama antes que
// red_eth_init() en app_main.c) y la deja parseada en s_ip_info -- llamadas
// siguientes (por ejemplo desde reiniciar_eth()) reusan el cache sin volver
// a tocar el archivo.
static void cargar_ip_info(void)
{
    if (s_ip_info_cargada) {
        return;
    }

    config_red_t red;
    config_braindlab_cargar_red(&red); // ya trae sus propios defaults si config.json no tiene la seccion "red"

    parsear_campo_ip(red.ip, "ip", &s_ip_info.ip, RED_IP_DEFAULT);
    parsear_campo_ip(red.gateway, "gateway", &s_ip_info.gw, RED_GATEWAY_DEFAULT);
    parsear_campo_ip(red.mascara, "mascara", &s_ip_info.netmask, RED_MASCARA_DEFAULT);

    ESP_LOGI(TAG, "Config de red cargada de config.json: ip=" IPSTR " gw=" IPSTR " mascara=" IPSTR,
             IP2STR(&s_ip_info.ip), IP2STR(&s_ip_info.gw), IP2STR(&s_ip_info.netmask));
    s_ip_info_cargada = true;
}

// Arma todo el driver W5500 + netif desde cero: pulso de reset, deteccion
// del chip, MAC fija, netif+glue, IP estatica y arranque. Separado de
// red_eth_init() porque tarea_vigilancia_eth() (mas abajo) necesita poder
// llamar esto de nuevo para recuperarse de un chip colgado, sin repetir la
// parte de infraestructura global (esp_netif_init/event loop/handlers) que
// solo puede correr una vez en toda la vida del programa. Guarda los
// handles en las variables de modulo (s_eth_handles/s_eth_netif/s_eth_glue)
// en vez de devolverlos, por eso mismo.
static esp_err_t iniciar_hardware_eth(void)
{
    // El driver del W5500 instala su propio manejo de GPIO (aunque sea en
    // modo polling) via el servicio de ISR de GPIO. Hay que instalarlo
    // aca (no solo en red_eth_init()) porque ethernet_deinit_all() lo
    // desinstala al limpiar -- si esta tarea se llama de nuevo desde
    // tarea_vigilancia_eth() despues de un reinicio, hace falta reinstalarlo.
    // ESP_ERR_INVALID_STATE significa que ya estaba instalado, no es un
    // error real.
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        return isr_err;
    }

    diag_pulso_reset_w5500();

    ESP_LOGI(TAG, "Llamando a ethernet_init_all()...");
    esp_err_t err = ethernet_init_all(&s_eth_handles, &s_eth_port_cnt);
    if (err != ESP_OK || s_eth_port_cnt == 0) {
        ESP_LOGE(TAG, "No se pudo inicializar el W5500 (%s)", esp_err_to_name(err));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ethernet_init_all() OK, %u modulo(s) detectado(s)", (unsigned)s_eth_port_cnt);

    // Mismo motivo que en controlador_labgeo: el MAC hay que fijarlo ANTES
    // de esp_netif_attach() (mas abajo), porque ese attach dispara
    // esp_eth_post_attach() que lee el MAC que tenga el chip en ese
    // instante y lo copia a esp_netif -- hacerlo despues deja a esp_netif
    // con el MAC auto-generado mientras el chip ya transmite con el fijo,
    // y el ping deja de responder por la desincronizacion.
    ESP_ERROR_CHECK(esp_eth_ioctl(s_eth_handles[0], ETH_CMD_S_MAC_ADDR, s_mac));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    s_eth_glue = esp_eth_new_netif_glue(s_eth_handles[0]);
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, s_eth_glue));

    // IP estatica, leida de config.json (seccion "red", ver
    // config_braindlab_cargar_red()) y cacheada en s_ip_info -- ver
    // cargar_ip_info(). Si config.json no tiene la seccion o no existe
    // todavia, config_braindlab_cargar_red() ya devuelve los defaults
    // (192.168.5.95 / 255.255.255.0 / 192.168.5.1).
    ESP_ERROR_CHECK(esp_netif_dhcpc_stop(s_eth_netif));
    cargar_ip_info();
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_eth_netif, &s_ip_info));
    verificar_ip_activa(s_eth_netif, "justo despues de set_ip_info");

    ESP_ERROR_CHECK(esp_eth_start(s_eth_handles[0]));

    vTaskDelay(pdMS_TO_TICKS(500));
    verificar_ip_activa(s_eth_netif, "500ms despues de esp_eth_start");

    return ESP_OK;
}

// Tira abajo el driver W5500 + netif actual (si quedo en pie algo utilizable)
// y llama a iniciar_hardware_eth() de nuevo para reconstruir todo de cero.
// Es el "martillo grande" que usa tarea_vigilancia_eth() cuando el link
// lleva mucho tiempo caido -- un corte de cable comun el propio driver ya
// lo recupera solo (disparando ETHERNET_EVENT_CONNECTED de nuevo) sin
// necesitar nada de esto; esta funcion es para cuando el chip mismo dejo de
// responder por SPI (se cuelga de vez en cuando en modulos W5500 sin IRQ,
// por polling).
static void reiniciar_eth(void)
{
    s_link_up = false; // evita que tarea_vigilancia_eth() lea un estado viejo mientras se reconstruye

    if (s_eth_handles) {
        esp_eth_stop(s_eth_handles[0]); // no ESP_ERROR_CHECK -- si el chip esta colgado, esto puede fallar, seguimos igual
    }
    if (s_eth_glue) {
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
    }
    if (s_eth_netif) {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
    }
    if (s_eth_handles) {
        ethernet_deinit_all(s_eth_handles); // libera el array y el bus SPI, resetea el contador interno del componente
        s_eth_handles = NULL;
        s_eth_port_cnt = 0;
    }

    esp_err_t err = iniciar_hardware_eth();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reiniciar_eth(): no se pudo reconstruir el driver (%s) -- se reintenta en la proxima vuelta de vigilancia",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "reiniciar_eth(): driver W5500 reconstruido OK");
    }
}

// Cada INTERVALO_S revisa el link (s_link_up, actualizado por on_eth_event())
// y que la IP estatica siga puesta de verdad (esp_netif_get_ip_info() no
// deberia perderla solo porque el link cae -- es estatica, no DHCP -- pero
// se pidio verificarla explicitamente como señal adicional, por si el netif
// quedo en un estado raro). Un corte de cable normal se recupera solo (el
// propio evento CONNECTED lo arregla) -- por eso NO se reacciona al primer
// chequeo en falso, solo despues de UMBRAL_S sostenidos sin recuperarse, que
// es la señal de que el chip probablemente se colgo de verdad.
static void tarea_vigilancia_eth(void *arg)
{
    const int INTERVALO_S = 10;
    const int UMBRAL_S = 60; // 6 chequeos seguidos en falso antes de reiniciar
    int segundos_sin_ok = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(INTERVALO_S * 1000));

        esp_netif_ip_info_t ip_actual;
        bool ip_ok = s_eth_netif && esp_netif_get_ip_info(s_eth_netif, &ip_actual) == ESP_OK &&
                     ip_actual.ip.addr != 0;

        if (s_link_up && ip_ok) {
            segundos_sin_ok = 0;
            continue;
        }

        segundos_sin_ok += INTERVALO_S;
        ESP_LOGW(TAG, "Vigilancia Ethernet: link=%s ip=%s -- %ds sin recuperarse",
                 s_link_up ? "up" : "down", ip_ok ? "ok" : "perdida/invalida", segundos_sin_ok);

        if (segundos_sin_ok >= UMBRAL_S) {
            ESP_LOGE(TAG, "Vigilancia Ethernet: %ds sin link/IP validos, reiniciando el driver W5500", segundos_sin_ok);
            reiniciar_eth();
            segundos_sin_ok = 0;
        }
    }
}

esp_err_t red_eth_init(void)
{
    // Infraestructura global -- solo puede correr una vez en toda la vida
    // del programa, por eso NO esta en iniciar_hardware_eth() (que
    // tarea_vigilancia_eth() vuelve a llamar si hace falta reiniciar).
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL));

    esp_err_t err = iniciar_hardware_eth();
    if (err != ESP_OK) {
        return err;
    }

    xTaskCreate(tarea_vigilancia_eth, "vigilancia_eth", 3072, NULL, 4, NULL);
    return ESP_OK;
}

bool red_eth_esta_conectado(void)
{
    return s_link_up;
}

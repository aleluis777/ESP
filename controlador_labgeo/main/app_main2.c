// ---------------------------------------------------------------------------
// PRUEBA: Dial 1 + Dial 2 (en secuencia, una sola tarea, nucleo 1) + red +
// servidor con WS + UART a la pantalla fisica + ahora tambien celda de
// carga (HX711) y guardado real de corridas (almacenamiento.c +
// programador_corrida.c). Con esto ya es basicamente el firmware completo,
// con los fixes de los diales que se confirmaron en los pasos anteriores
// (cooldown de 20ms entre reintentos de consenso, lectura en secuencia con
// backoff en vez de una tarea por dial, nucleo 1 separado de la red).
//
// Todavia sin los endpoints web de calibracion/red/avanzar_ensayo -- el
// arranque/parada de una corrida se maneja por UART (la pantalla fisica),
// que ya estaba conectado desde el paso anterior.
//
// Para volver al firmware real: en main/CMakeLists.txt, en la lista de SRCS,
// cambiar "app_main2.c" de nuevo por "app_main.c".
// ---------------------------------------------------------------------------

#include <inttypes.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "dial_caliper.h"
#include "hx711.h"
#include "config_labgeo.h"
#include "almacenamiento.h"
#include "programador_corrida.h"
#include "red_eth.h"
#include "servidor_web.h"
#include "uart_link.h"

static const char *TAG = "PRUEBA_DIAL_RED";

// Mismos pines que usa app_main.c (ver ese archivo si se mueven de nuevo).
#define PIN_DIAL1_REQ   GPIO_NUM_12  // AA1
#define PIN_DIAL1_CLK   GPIO_NUM_25  // P2
#define PIN_DIAL1_DATA  GPIO_NUM_27  // HUM

#define PIN_DIAL2_REQ   GPIO_NUM_2   // AA2
#define PIN_DIAL2_CLK   GPIO_NUM_26  // P6
#define PIN_DIAL2_DATA  GPIO_NUM_4   // AA4

#define PIN_HX711_DOUT  GPIO_NUM_22
#define PIN_HX711_SCK   GPIO_NUM_21

static dial_caliper_t s_dial1;
static dial_caliper_t s_dial2;

static hx711_t s_celda = {
    .dout = PIN_HX711_DOUT,
    .pd_sck = PIN_HX711_SCK,
    .gain = HX711_GAIN_A_128,
};
static config_calibracion_t s_cal; // calibracion cargada de /www/calibracion.json (o defaults, sin endpoint web todavia)
static programador_t s_prog = { .activa = false };

// Cache de cada sensor, actualizada por su propia tarea -- tarea_publicar()
// solo lee esto (nunca llama a dial_caliper_leer()/hx711_read_data()
// directamente), mismo criterio que el firmware real: ningun sensor atrasa
// a la publicacion, y publicar no le agrega latencia a ningun sensor.
static volatile int32_t s_dial1_um = 0;
static volatile int32_t s_dial2_um = 0;
static volatile int32_t s_celda_cruda = 0;
static volatile int32_t s_celda_cruda_prom = 0;

// ---------------------------------------------------------------------------
// Backoff por dial desconectado: si un dial no esta enchufado, cada intento
// agota los 5 reintentos de consenso (~1.5s en total) antes de rendirse --
// hecho en cada ciclo, eso atrasa al OTRO dial (que si esta conectado) al
// doble de lento sin necesidad. Despues de DIAL_FALLOS_PARA_SOSPECHAR fallos
// SEGUIDOS, se asume "probablemente desconectado" y se lo deja de intentar
// en cada ciclo -- se lo prueba de nuevo en dos etapas: primero cada
// DIAL_INTERVALO_INICIAL_MS (DIAL_REINTENTOS_INICIALES veces), y si sigue
// sin responder, se afloja para siempre a cada DIAL_INTERVALO_LENTO_MS. En
// cuanto un intento sale bien (se volvio a conectar), se resetea todo y
// vuelve al ritmo normal (un intento por ciclo) para ese dial.
// ---------------------------------------------------------------------------
#define DIAL_FALLOS_PARA_SOSPECHAR   3     // fallos seguidos (a ritmo normal) antes de entrar en backoff
#define DIAL_REINTENTOS_INICIALES    3     // cuantos reintentos a DIAL_INTERVALO_INICIAL_MS antes de aflojar mas
#define DIAL_INTERVALO_INICIAL_MS    5000  // ritmo apenas se sospecha desconectado
#define DIAL_INTERVALO_LENTO_MS      20000 // ritmo final, para siempre, si sigue sin responder

typedef struct {
    int fallos_seguidos;
    int reintentos_en_backoff;
    int64_t proximo_intento_us;
} dial_backoff_t;

static dial_backoff_t s_dial1_bk = { 0 };
static dial_backoff_t s_dial2_bk = { 0 };

// Intenta leer un dial, respetando el backoff si ya viene sospechado de
// desconectado. Devuelve true si actualizo 'cache_um' con una lectura nueva.
static bool intentar_leer_dial(dial_caliper_t *d, const char *nombre, dial_backoff_t *bk,
                                volatile int32_t *cache_um)
{
    int64_t ahora_us = esp_timer_get_time();

    if (bk->fallos_seguidos >= DIAL_FALLOS_PARA_SOSPECHAR && ahora_us < bk->proximo_intento_us) {
        return false; // sospechado de desconectado -- todavia no toca reintentar
    }

    int32_t valor_um;
    if (dial_caliper_leer(d, &valor_um, 0)) {
        *cache_um = valor_um;
        bk->fallos_seguidos = 0;
        bk->reintentos_en_backoff = 0;
        ESP_LOGI(TAG, "%s: %" PRId32 ".%03" PRId32 " mm  (valor_um=%" PRId32 ")",
                 nombre, valor_um / 1000, valor_um % 1000, valor_um);
        return true;
    }

    bk->fallos_seguidos++;

    if (bk->fallos_seguidos < DIAL_FALLOS_PARA_SOSPECHAR) {
        ESP_LOGW(TAG, "%s: timeout de lectura (sin consenso en los intentos)", nombre);
        return false;
    }

    // Ya estamos en backoff (o entrando recien ahora): primeros
    // DIAL_REINTENTOS_INICIALES a ritmo rapido, despues al lento para siempre.
    bk->reintentos_en_backoff++;
    uint32_t intervalo_ms = (bk->reintentos_en_backoff <= DIAL_REINTENTOS_INICIALES)
                                 ? DIAL_INTERVALO_INICIAL_MS
                                 : DIAL_INTERVALO_LENTO_MS;
    bk->proximo_intento_us = ahora_us + (int64_t)intervalo_ms * 1000;

    if (bk->fallos_seguidos == DIAL_FALLOS_PARA_SOSPECHAR) {
        ESP_LOGW(TAG, "%s: %d fallos seguidos -- se prueba cada %" PRIu32 "ms",
                 nombre, DIAL_FALLOS_PARA_SOSPECHAR, intervalo_ms);
    } else if (bk->reintentos_en_backoff == DIAL_REINTENTOS_INICIALES + 1) {
        ESP_LOGW(TAG, "%s: sigue sin responder despues de %d reintentos -- de ahora en mas se prueba cada %" PRIu32 "ms",
                 nombre, DIAL_REINTENTOS_INICIALES, intervalo_ms);
    }
    return false;
}

// Lee Dial 1 y despues Dial 2, siempre en ese orden, nunca en paralelo --
// una sola tarea, un solo sondeo activo a la vez.
static void tarea_diales(void *arg)
{
    while (1) {
        intentar_leer_dial(&s_dial1, "Dial 1", &s_dial1_bk, &s_dial1_um);
        intentar_leer_dial(&s_dial2, "Dial 2", &s_dial2_bk, &s_dial2_um);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// Cuantas lecturas entran al promedio movil que se muestra/pesa (mismo
// criterio que el firmware real).
#define CELDA_MUESTRAS_PROMEDIO 3

static void tarea_celda(void *arg)
{
    int32_t historial[CELDA_MUESTRAS_PROMEDIO] = { 0 };
    size_t idx = 0;
    size_t cuenta = 0;

    while (1) {
        bool listo = false;
        if (hx711_is_ready(&s_celda, &listo) == ESP_OK && listo) {
            int32_t crudo;
            if (hx711_read_data(&s_celda, &crudo) == ESP_OK) {
                s_celda_cruda = crudo;

                historial[idx] = crudo;
                idx = (idx + 1) % CELDA_MUESTRAS_PROMEDIO;
                if (cuenta < CELDA_MUESTRAS_PROMEDIO) {
                    cuenta++;
                }
                int64_t suma = 0;
                for (size_t i = 0; i < cuenta; i++) {
                    suma += historial[i];
                }
                s_celda_cruda_prom = (int32_t)(suma / (int64_t)cuenta);
            }
        }
        vTaskDelay(1); // no listo todavia (o celda desconectada) -- reintenta sin trabar a nadie
    }
}

// ---------------------------------------------------------------------------
// Estado del "ensayo" (0..4), igual semantica que el firmware real -- solo
// que ac's lo maneja UNICAMENTE el UART (no hay boton web todavia en este
// test).
// ---------------------------------------------------------------------------
typedef enum {
    ESTADO_INICIAL = 0,
    ESTADO_CORRIDA1_INICIADA = 1,
    ESTADO_CORRIDA1_FINALIZADA = 2,
    ESTADO_CORRIDA2_INICIADA = 3,
    ESTADO_CORRIDA2_FINALIZADA = 4,
} estado_ensayo_t;

static volatile estado_ensayo_t s_estado_ensayo = ESTADO_INICIAL;

// Calibra/publica cada 200ms: aplica la calibracion (defaults si nunca se
// guardo calibracion.json -- celda_pendiente=0 hace que peso_n quede en 0,
// no hay division por cero), alimenta el programador (que decide cuando
// guardar un punto), y manda todo por WS + UART.
static void tarea_publicar(void *arg)
{
    while (1) {
        int32_t dial1_crudo_um = s_dial1_um;
        int32_t dial2_crudo_um = s_dial2_um;
        int32_t celda_cruda = s_celda_cruda_prom;

        float dial1_um_cal = (float)dial1_crudo_um * s_cal.dial1_pendiente + s_cal.dial1_offset;
        float dial2_um_cal = (float)dial2_crudo_um * s_cal.dial2_pendiente + s_cal.dial2_offset;
        float dial1_mm = dial1_um_cal / 1000.0f;
        float dial2_mm = dial2_um_cal / 1000.0f;

        float peso_n = (s_cal.celda_pendiente != 0.0f)
                           ? ((float)celda_cruda - s_cal.celda_offset) / s_cal.celda_pendiente
                           : 0.0f;

        int32_t dial1_um = (int32_t)dial1_um_cal;
        int32_t dial2_um = (int32_t)dial2_um_cal;
        int32_t peso_mN = (int32_t)(peso_n * 1000.0f);

        uint32_t tiempo_ms = 0;
        programador_actualizar(&s_prog, dial1_um, dial2_um, peso_mN, &tiempo_ms);

        char json[192];
        snprintf(json, sizeof(json),
                 "{\"corrida\":%u,\"activa\":%s,\"dial1_mm\":%.4f,\"dial2_mm\":%.4f,"
                 "\"peso_N\":%.3f,\"tiempo_ms\":%" PRIu32 ",\"estado\":%d}",
                 (unsigned)s_prog.run_id, s_prog.activa ? "true" : "false",
                 dial1_mm, dial2_mm, peso_n, tiempo_ms, (int)s_estado_ensayo);
        servidor_web_enviar_ws(json);

        uart_link_enviar_sensor_update(s_prog.run_id, dial1_um, dial2_um, peso_mN, tiempo_ms,
                                        (uint8_t)s_estado_ensayo);

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ---------------------------------------------------------------------------
// Callbacks UART (pantalla fisica) -- ahora conectados de verdad al
// programador y al almacenamiento (antes solo logueaban).
// ---------------------------------------------------------------------------
static void on_uart_start(uint8_t run_id)
{
    programador_iniciar(&s_prog, run_id);
    s_estado_ensayo = (run_id == 1) ? ESTADO_CORRIDA1_INICIADA : ESTADO_CORRIDA2_INICIADA;
    ESP_LOGI(TAG, "UART START: corrida %u, nuevo estado=%d", (unsigned)run_id, (int)s_estado_ensayo);
}

static void on_uart_stop(uint8_t run_id)
{
    programador_detener(&s_prog);
    // s_prog.run_id (no el 'run_id' de la trama) porque programador_detener()
    // no lo borra -- el estado queda bien aunque la pantalla mande STOP sin
    // run_id valido.
    s_estado_ensayo = (s_prog.run_id == 1) ? ESTADO_CORRIDA1_FINALIZADA : ESTADO_CORRIDA2_FINALIZADA;
    ESP_LOGI(TAG, "UART STOP: corrida %u, nuevo estado=%d", (unsigned)run_id, (int)s_estado_ensayo);
}

// Reenvia cada bloque que arma almacenamiento_leer_corrida() directo por
// UART -- ya viene empaquetado en el formato de 12 bytes/punto del
// protocolo, no hay que reinterpretar nada.
static void uart_chunk_cb(const uint8_t *puntos_buf, uint8_t count, bool es_ultimo, void *ctx)
{
    uint8_t run_id = *(uint8_t *)ctx;
    uart_link_enviar_run_chunk(run_id, puntos_buf, count, es_ultimo ? 1 : 0);
}

static void on_uart_request_run(uint8_t run_id)
{
    ESP_LOGI(TAG, "UART REQUEST_RUN: corrida %u", (unsigned)run_id);
    almacenamiento_leer_corrida(run_id, uart_chunk_cb, &run_id);
}

void app_main(void)
{
    // 1) Calibracion primero: monta SPIFFS "www" y carga calibracion.json (o
    // los defaults si no existe/esta corrupto) -- tiene que pasar antes de
    // arrancar los sensores.
    config_labgeo_init();
    config_labgeo_cargar(&s_cal);

    // 2) Almacenamiento: confirma que "www" ya este montada -- ahi vive
    // corridas.csv.
    almacenamiento_init();

    // 3) Sensores: no dependen de la red.
    esp_err_t err_celda = hx711_init(&s_celda);
    if (err_celda != ESP_OK) {
        ESP_LOGW(TAG, "hx711_init() fallo (%s) -- revisar cableado/alimentacion de la celda",
                 esp_err_to_name(err_celda));
    }
    dial_caliper_init(&s_dial1, PIN_DIAL1_REQ, PIN_DIAL1_CLK, PIN_DIAL1_DATA);
    dial_caliper_init(&s_dial2, PIN_DIAL2_REQ, PIN_DIAL2_CLK, PIN_DIAL2_DATA);
    ESP_LOGI(TAG, "Dial 1 (REQ=%d CLK=%d DATA=%d) y Dial 2 (REQ=%d CLK=%d DATA=%d) listos",
             PIN_DIAL1_REQ, PIN_DIAL1_CLK, PIN_DIAL1_DATA, PIN_DIAL2_REQ, PIN_DIAL2_CLK, PIN_DIAL2_DATA);

    // Nucleo 1 a proposito, NO el 0: app_main()/red_eth_init() corren en el
    // nucleo 0 (CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0=y), y ahi tambien vive el
    // manejo de la interrupcion del W5500 -- confirmado que compartir
    // nucleo con eso corrompia el sondeo. Una sola tarea para los dos
    // diales (ver tarea_diales) -- nunca hay dos sondeos activos al mismo
    // tiempo, asi que no importa que compartan nucleo entre si.
    xTaskCreatePinnedToCore(tarea_diales, "tarea_diales", 3072, NULL, 5, NULL, 1);
    xTaskCreate(tarea_celda, "tarea_celda", 3072, NULL, 5, NULL);

    // 4) Callbacks UART ANTES de uart_link_init(), mismo criterio que el
    // resto del proyecto: evita una ventana donde llegue un comando de la
    // pantalla y no haya nadie escuchando.
    uart_link_callbacks_t uart_cbs = {
        .on_start = on_uart_start,
        .on_stop = on_uart_stop,
        .on_request_run = on_uart_request_run,
    };
    uart_link_set_callbacks(uart_cbs);
    uart_link_init();

    esp_err_t err = red_eth_init();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "red_eth_init() OK -- probar ping a la IP configurada");

        err = servidor_web_init();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "servidor_web_init() OK -- conectar a ws://<ip>/ws para ver los sensores en vivo");
        } else {
            ESP_LOGE(TAG, "servidor_web_init() fallo (%s)", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "red_eth_init() fallo (%s)", esp_err_to_name(err));
    }

    // tarea_publicar (WS + UART + programador) arranca siempre, ande o no la
    // red -- el UART a la pantalla fisica no depende de que el Ethernet este
    // disponible.
    xTaskCreate(tarea_publicar, "tarea_publicar", 4096, NULL, 5, NULL);
}

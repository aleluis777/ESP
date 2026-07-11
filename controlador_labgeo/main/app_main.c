// Punto de entrada del firmware. Este archivo es el unico que conoce a
// todos los demas modulos y los conecta entre si -- cada modulo (hx711,
// dial_caliper, uart_link, etc.) no sabe nada de los otros. Ver README.md
// para el mapa completo del flujo de datos.

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "config_labgeo.h"
#include "almacenamiento.h"
#include "programador_corrida.h"
#include "hx711.h"
#include "dial_caliper.h"
#include "uart_link.h"
#include "red_eth.h"
#include "servidor_web.h"

static const char *TAG = "APP";

// ---------- Pines (los mismos que usaba LABGEO2.ino, hardware ya cableado) ----------
#define PIN_HX711_DOUT  GPIO_NUM_22
#define PIN_HX711_SCK   GPIO_NUM_21

#define PIN_DIAL1_REQ   GPIO_NUM_15
#define PIN_DIAL1_CLK   GPIO_NUM_34
#define PIN_DIAL1_DATA  GPIO_NUM_39

#define PIN_DIAL2_REQ   GPIO_NUM_25
#define PIN_DIAL2_CLK   GPIO_NUM_14
#define PIN_DIAL2_DATA  GPIO_NUM_13

// Handles de cada sensor y estado global. Son 'static' a nivel de archivo
// (no locales a una funcion) porque tanto app_main() como tarea_sensores()
// y los callbacks de UART necesitan acceder a los mismos objetos.
static hx711_t s_celda;
static dial_caliper_t s_dial1;
static dial_caliper_t s_dial2;
static config_calibracion_t s_cal;          // calibracion cargada desde NVS
static programador_t s_prog = { .activa = false }; // que corrida esta corriendo (si hay alguna)

// Contexto que necesita chunk_cb() para saber a que corrida pertenece cada
// bloque de puntos que va leyendo del archivo (almacenamiento_leer_corrida
// no sabe nada de UART, por eso se le pasa este 'ctx' generico).
typedef struct {
    uint8_t run_id;
} chunk_ctx_t;

// Callback que le pasamos a almacenamiento_leer_corrida(): por cada bloque
// de hasta 16 puntos que lee del archivo en SPIFFS, lo reenvia tal cual por
// UART como una trama RUN_CHUNK. Asi el archivo nunca se carga entero en
// RAM, se va streameando en pedacitos.
static void chunk_cb(const uint8_t *buf, uint8_t count, bool es_ultimo, void *ctx)
{
    chunk_ctx_t *c = (chunk_ctx_t *)ctx;
    uart_link_enviar_run_chunk(c->run_id, buf, count, es_ultimo ? 1 : 0);
}

// ---------- Callbacks de comandos que llegan de la pantalla ----------
// Estos 3 se registran con uart_link_set_callbacks() en app_main() y los
// termina llamando uart_link.c cuando reconoce una trama valida (CRC ok).
// app_main.c no sabe nada del formato de las tramas, solo reacciona.

// Llega CMD_START: le decimos al programador que arranque el cronometro y
// el archivo de esa corrida (programador_iniciar llama internamente a
// almacenamiento_iniciar_corrida, que trunca/crea el archivo).
static void on_start(uint8_t run_id)
{
    programador_iniciar(&s_prog, run_id);
}

// Llega CMD_STOP: para el cronometro. Lo que ya se guardo en el archivo
// queda como esta, no se borra ni se toca.
static void on_stop(uint8_t run_id)
{
    (void)run_id; // no hace falta: s_prog ya sabe cual es la corrida activa
    programador_detener(&s_prog);
}

// Llega CMD_REQUEST_RUN: lee el archivo de esa corrida entero (en bloques,
// via chunk_cb) y lo va mandando por UART como una serie de RUN_CHUNK.
static void on_request_run(uint8_t run_id)
{
    chunk_ctx_t ctx = { .run_id = run_id };
    almacenamiento_leer_corrida(run_id, chunk_cb, &ctx);
}

// ---------- Loop de sensores: lee, calibra, alimenta el programador y manda SENSOR_UPDATE ----------
// Corre en su propia tarea de FreeRTOS (ver xTaskCreate en app_main), separada
// de la tarea que atiende el UART y de la que atiende el servidor web, para
// que ninguna bloquee a las otras.
static void tarea_sensores(void *arg)
{
    while (1) {
        // 1) Leer los 3 sensores. Cada lectura tiene timeout de 50ms: si el
        // sensor no responde (no conectado, cable suelto, etc.) la funcion
        // devuelve false pero el loop sigue igual, no se traba esperando.
        int32_t dial1_crudo = 0, dial2_crudo = 0, celda_cruda = 0;
        bool ok1 = dial_caliper_leer(&s_dial1, &dial1_crudo, 50);
        bool ok2 = dial_caliper_leer(&s_dial2, &dial2_crudo, 50);
        bool ok3 = hx711_leer(&s_celda, &celda_cruda, 50);

        if (!ok1) ESP_LOGW(TAG, "Dial 1: timeout de lectura");
        if (!ok2) ESP_LOGW(TAG, "Dial 2: timeout de lectura");
        if (!ok3) {
            ESP_LOGW(TAG, "Celda de carga: timeout de lectura");
        } else {
            // Log de diagnostico: el valor crudo (sin calibrar) del HX711,
            // util para verificar que el sensor responde y que el numero
            // cambia al aplicar peso.
            ESP_LOGI(TAG, "Celda de carga: crudo=%" PRId32, celda_cruda);
        }

        // 2) Calibrar: pasar de unidades crudas del sensor a unidades de
        // ingenieria (mm, N) aplicando pendiente/offset guardados en NVS.
        // dial_caliper_leer da centesimas de mm; se pasa a mm, se calibra y
        // se manda por UART en micrometros (unidad del protocolo).
        float dial1_mm = (dial1_crudo / 100.0f) * s_cal.dial1_pendiente + s_cal.dial1_offset;
        float dial2_mm = (dial2_crudo / 100.0f) * s_cal.dial2_pendiente + s_cal.dial2_offset;
        // Misma formula que usaba el .ino viejo: celda = (crudo - offset) / pendiente.
        float peso_N   = (s_cal.celda_pendiente != 0.0f)
                              ? (celda_cruda - s_cal.celda_offset) / s_cal.celda_pendiente
                              : 0.0f;

        // 3) Pasar de unidades de ingenieria (float) a las unidades enteras
        // que usa el protocolo (ver protocolo_labgeo.h): micrometros y
        // milinewtons, para no mandar floats por el cable.
        int32_t dial1_um = (int32_t)(dial1_mm * 1000.0f);
        int32_t dial2_um = (int32_t)(dial2_mm * 1000.0f);
        int32_t peso_mN  = (int32_t)(peso_N * 1000.0f);

        // 4) Alimentar el cronograma de la corrida activa. Internamente
        // decide si "toca" un checkpoint (por tiempo en Corrida 1, por
        // desplazamiento del Dial 2 en Corrida 2) y si toca, guarda el
        // punto en el archivo de esa corrida.
        uint32_t tiempo_ms = 0;
        programador_actualizar(&s_prog, dial1_um, dial2_um, peso_mN, &tiempo_ms);

        // 5) Solo se manda SENSOR_UPDATE mientras hay una corrida activa (ver
        // PROTOCOLO_UART.md); en reposo la pantalla no necesita nada nuevo.
        if (s_prog.activa) {
            uart_link_enviar_sensor_update(s_prog.run_id, dial1_um, dial2_um, peso_mN, tiempo_ms);
        }

        // 6) El WebSocket si manda siempre (activa o no) -- es el canal
        // generico para monitoreo remoto desde el navegador, no solo para
        // la corrida en curso. Si no hay clientes conectados a /ws, esta
        // llamada simplemente no hace nada (ver servidor_web.c).
        char json[160];
        snprintf(json, sizeof(json),
                 "{\"corrida\":%u,\"activa\":%s,\"dial1_mm\":%.3f,\"dial2_mm\":%.3f,"
                 "\"peso_N\":%.3f,\"tiempo_ms\":%" PRIu32 "}",
                 (unsigned)s_prog.run_id, s_prog.activa ? "true" : "false",
                 dial1_mm, dial2_mm, peso_N, tiempo_ms);
        servidor_web_enviar_ws(json);

        vTaskDelay(pdMS_TO_TICKS(200)); // ~5 Hz
    }
}

void app_main(void)
{
    // 1) Calibracion: levanta NVS y carga pendiente/offset de cada sensor.
    // Si nunca se calibro nada, config_labgeo_cargar() deja valores
    // neutros (pendiente=1, offset=0) para que el sistema no rompa.
    ESP_ERROR_CHECK(config_labgeo_init());
    config_labgeo_cargar(&s_cal);

    // 2) Almacenamiento: monta SPIFFS (particion "storage"), donde van a
    // vivir los archivos corrida1.dat / corrida2.dat con el historial.
    ESP_ERROR_CHECK(almacenamiento_init());

    // 3) Deja los pines de cada sensor configurados (entrada/salida).
    // Todavia no lee nada, eso pasa recien en tarea_sensores().
    hx711_init(&s_celda, PIN_HX711_DOUT, PIN_HX711_SCK);
    dial_caliper_init(&s_dial1, PIN_DIAL1_REQ, PIN_DIAL1_CLK, PIN_DIAL1_DATA);
    dial_caliper_init(&s_dial2, PIN_DIAL2_REQ, PIN_DIAL2_CLK, PIN_DIAL2_DATA);

    // 4) Registra los callbacks ANTES de abrir el UART, para que no se
    // pueda perder ningun comando que llegue apenas se prende el enlace.
    uart_link_callbacks_t cbs = {
        .on_start = on_start,
        .on_stop = on_stop,
        .on_request_run = on_request_run,
    };
    uart_link_set_callbacks(cbs);
    uart_link_init();

    // 5) Ethernet + servidor web: si el W5500 no levanta (cable
    // desconectado, config mala, etc.) no es fatal -- sensores y UART
    // siguen funcionando igual, solo no hay WebSocket disponible.
    if (red_eth_init() == ESP_OK) {
        servidor_web_init();
    } else {
        ESP_LOGE(TAG, "Ethernet no disponible, el servidor web no se inicia");
    }

    // 6) Recien aca arranca el loop que lee sensores en repeticion, como
    // tarea aparte para no bloquear el resto de app_main().
    xTaskCreate(tarea_sensores, "tarea_sensores", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Controlador LabGeo listo (sensores + UART + Ethernet/WS).");
}

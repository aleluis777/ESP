// ---------------------------------------------------------------------------
// VERSION DE DIAGNOSTICO -- no es el firmware normal, pero ya tiene todos
// los sensores + el guardado real de corridas + UART hacia la pantalla
// fisica conectados (falta solo el observador de ping, a proposito -- no
// hace falta todavia).
//
// Confirmado hasta ahora: hardware/IRQ del W5500 OK, red_eth_init() OK (ping
// responde), servidor_web_init() OK, celda de carga + calibracion via
// config.json. Este paso agrega:
//   - Los dos diales (dial_caliper.c).
//   - almacenamiento.c (guarda cada corrida en /spiffs/corridaN.dat).
//   - programador_corrida.c (decide CUANDO se guarda un punto).
//   - El boton unico de la web (estado 0..4) ahora dispara de verdad
//     programador_iniciar()/programador_detener(), no es solo cosmetico.
//   - uart_link.c conectado: START/STOP/REQUEST_RUN de la pantalla fisica
//     ya disparan lo mismo que el boton de la web (on_uart_start/on_uart_stop
//     actualizan s_estado_ensayo), y SENSOR_UPDATE ahora manda tambien el
//     byte de estado 0..4 (ver protocolo_labgeo.h) para que la pantalla
//     muestre el boton correcto sin rearmar su propia maquina de estados.
//
// OJO unidades: dial_caliper_leer() devuelve la posicion ya en MICROMETROS
// reales (ver dial_caliper.h, el propio parametro se llama valor_um) -- NO
// en "centesimas de mm" como asumia la formula vieja de app_main copy.c
// (dial_crudo/100.0f). Por eso la calibracion ac's se aplica directo en
// micrometros y recien se pasa a mm para mostrar, en vez de copiar esa
// formula vieja (quedaria 10x mal con el driver actual).
// ---------------------------------------------------------------------------

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "red_eth.h"
#include "servidor_web.h"
#include "hx711.h"
#include "dial_caliper.h"
#include "config_labgeo.h"
#include "almacenamiento.h"
#include "programador_corrida.h"
#include "uart_link.h"

static const char *TAG = "DIAG_RED_ETH";

// Mismos pines que usaba LABGEO2.ino / el firmware real (app_main copy.c).
#define PIN_HX711_DOUT  GPIO_NUM_22
#define PIN_HX711_SCK   GPIO_NUM_21

#define PIN_DIAL1_REQ   GPIO_NUM_12  // AA1
#define PIN_DIAL1_CLK   GPIO_NUM_25  // P2
#define PIN_DIAL1_DATA  GPIO_NUM_27  // HUM

#define PIN_DIAL2_REQ   GPIO_NUM_2   // AA2
#define PIN_DIAL2_CLK   GPIO_NUM_26  // P6
#define PIN_DIAL2_DATA  GPIO_NUM_4   // AA4 (antes P7/GPIO15 -- se libero para el buzzer, ver abajo)

// Buzzer: GPIO15 (P7).
#define PIN_BUZZER GPIO_NUM_15  // P7

// Celda de carga: ya no es nuestro driver bit-bang propio -- se reemplazo
// por el componente esp-idf-lib/hx711 (ver main/idf_component.yml), un
// driver probado y mantenido. HX711_GAIN_A_128 = canal A, ganancia 128 (1
// pulso extra despues de los 24 bits de dato) -- mismo modo que usaba
// nuestro driver viejo.
static hx711_t s_celda = {
    .dout = PIN_HX711_DOUT,
    .pd_sck = PIN_HX711_SCK,
    .gain = HX711_GAIN_A_128,
};
static dial_caliper_t s_dial1;
static dial_caliper_t s_dial2;
static config_calibracion_t s_cal; // calibracion cargada de /www/calibracion.json
static programador_t s_prog = { .activa = false };

// ---------------------------------------------------------------------------
// Ultimo valor conocido de cada dial, actualizado por su propia tarea.
// dial_caliper_leer() IGNORA el timeout_ms que se le pasa (esta documentado
// en dial_caliper.c: "timeout_ms queda sin usar a proposito") y usa un
// timeout interno de ~1s POR SEMIFLANCO con espera activa (sin vTaskDelay,
// no le cede el CPU a nadie) -- si un dial no responde, puede trabar la
// tarea que lo llama durante ~1s de corrido. Si esa misma tarea fuera la que
// tambien manda el WS, el socket se atrasa en bloque cada vez que un dial
// tarda o esta desconectado (era exactamente lo que se estaba viendo).
//
// Por eso cada dial vive en su PROPIA tarea, que se puede bloquear todo lo
// que el driver necesite sin afectar a nadie mas. tarea_sensores() (WS, cada
// ~200ms fijo) solo lee estas variables cacheadas, nunca llama a
// dial_caliper_leer() directamente ni espera por ningun dial.
//
// int32_t es atomico en un solo word de 32 bits en el ESP32 -- alcanza con
// 'volatile' para este uso (una tarea escribe, otra lee, sin necesidad de
// mutex) sin arriesgar una lectura a medio escribir.
// ---------------------------------------------------------------------------
static volatile int32_t s_dial1_um_crudo = 0;
static volatile int32_t s_dial2_um_crudo = 0;
static volatile int32_t s_celda_cruda = 0;      // ultima lectura instantanea (la usa la calibracion)
static volatile int32_t s_celda_cruda_prom = 0; // promedio movil de las ultimas 3 lecturas (lo que se muestra/pesa)

// Buzzer simple (on/off, sin PWM/tono -- alcanza para un beep de aviso).
// Bloquea la tarea que lo llama por 'duracion_ms' -- se usa desde el arranque
// (una sola vez) y desde on_avanzar_ensayo() (que corre en la tarea del
// servidor HTTP al llegar el POST), asi que un beep corto no afecta a nadie
// mas que a esa peticion puntual.
static void buzzer_beep(uint32_t duracion_ms)
{
    gpio_set_level(PIN_BUZZER, 1);
    vTaskDelay(pdMS_TO_TICKS(duracion_ms));
    gpio_set_level(PIN_BUZZER, 0);
}

// Lee el Dial 1 en loop, sin ningun timeout que nos importe respetar (esta
// tarea no le manda nada a nadie, puede bloquearse el tiempo que haga falta).
// Solo actualiza el valor cacheado cuando la lectura sale bien -- si el dial
// no responde, se queda mostrando el ultimo valor valido en vez de saltar a 0.
static void tarea_dial1(void *arg)
{
    while (1) {
        int32_t crudo;
        if (dial_caliper_leer(&s_dial1, &crudo, 0)) {
            s_dial1_um_crudo = crudo;
        } else {
            ESP_LOGW(TAG, "Dial 1: timeout de lectura (sin cambios, se mantiene el ultimo valor)");
            vTaskDelay(pdMS_TO_TICKS(100)); // dial desconectado -- no reintentar sin pausa (satura el nucleo)
        }
    }
}

static void tarea_dial2(void *arg)
{
    while (1) {
        int32_t crudo;
        if (dial_caliper_leer(&s_dial2, &crudo, 0)) {
            s_dial2_um_crudo = crudo;
        } else {
            ESP_LOGW(TAG, "Dial 2: timeout de lectura (sin cambios, se mantiene el ultimo valor)");
            vTaskDelay(pdMS_TO_TICKS(100)); // dial desconectado -- no reintentar sin pausa (satura el nucleo)
        }
    }
}

// Cuantas lecturas entran al promedio movil que se muestra/pesa (no confundir
// con CALIBRACION_LECTURAS_PROMEDIO, que es para los clicks de calibrar).
#define CELDA_MUESTRAS_PROMEDIO 3

// Mismo tratamiento que los diales, pero con el primitivo realmente
// no-bloqueante que da la libreria (hx711_is_ready() solo lee el pin una vez
// y vuelve al toque, a diferencia de hx711_wait() que bloquea hasta
// 'timeout_ms' esperando). Si no hay dato listo, cede CPU con vTaskDelay(1)
// y reintenta -- nunca bloquea a nadie, ni siquiera hasta 50ms como antes.
static void tarea_celda(void *arg)
{
    int32_t historial[CELDA_MUESTRAS_PROMEDIO] = {0};
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
// Estado del boton unico de la web (0..4). Ahora SI dispara el programador
// de verdad en cada transicion de inicio/fin de corrida -- ya no es solo
// cosmetico como en el paso anterior.
//
//   0 = inicial              -> "Iniciar Corrida 1"   (sin accion todavia)
//   1 = corrida 1 iniciada   -> "Detener Corrida 1"   (programador_iniciar(1))
//   2 = corrida 1 finalizada -> "Iniciar Corrida 2"   (programador_detener())
//   3 = corrida 2 iniciada   -> "Detener Corrida 2"   (programador_iniciar(2))
//   4 = corrida 2 finalizada -> "Reiniciar Todo"       (programador_detener())
//   (click en 4 vuelve a 0, sin accion -- el programador ya quedo detenido)
// ---------------------------------------------------------------------------
typedef enum {
    ESTADO_INICIAL = 0,
    ESTADO_CORRIDA1_INICIADA = 1,
    ESTADO_CORRIDA1_FINALIZADA = 2,
    ESTADO_CORRIDA2_INICIADA = 3,
    ESTADO_CORRIDA2_FINALIZADA = 4,
} estado_ensayo_t;

static volatile estado_ensayo_t s_estado_ensayo = ESTADO_INICIAL;

// POST /avanzar_ensayo: un solo boton en la web llama siempre a este mismo
// endpoint -- ac's decidimos que transicion corresponde segun el estado
// actual, y ahora si arrancamos/paramos el programador de verdad.
static void on_avanzar_ensayo(void)
{
    buzzer_beep(100); // aviso corto en cada click del boton, sin importar la transicion

    switch (s_estado_ensayo) {
    case ESTADO_INICIAL:
        programador_iniciar(&s_prog, 1);
        s_estado_ensayo = ESTADO_CORRIDA1_INICIADA;
        break;
    case ESTADO_CORRIDA1_INICIADA:
        programador_detener(&s_prog);
        s_estado_ensayo = ESTADO_CORRIDA1_FINALIZADA;
        break;
    case ESTADO_CORRIDA1_FINALIZADA:
        programador_iniciar(&s_prog, 2);
        s_estado_ensayo = ESTADO_CORRIDA2_INICIADA;
        break;
    case ESTADO_CORRIDA2_INICIADA:
        programador_detener(&s_prog);
        s_estado_ensayo = ESTADO_CORRIDA2_FINALIZADA;
        break;
    case ESTADO_CORRIDA2_FINALIZADA:
        s_estado_ensayo = ESTADO_INICIAL; // el programador ya esta detenido, nada mas que hacer
        break;
    }
    ESP_LOGI(TAG, "avanzar_ensayo: nuevo estado=%d", (int)s_estado_ensayo);
}

// ---------------------------------------------------------------------------
// Callbacks UART (pantalla fisica): a diferencia del boton unico de la web
// (que no manda run_id, solo "avanzar"), START/STOP de la pantalla ya traen
// el run_id explicito (1 o 2) -- de todas formas actualizamos
// s_estado_ensayo para que el boton de la web quede sincronizado con lo que
// hizo la pantalla, y viceversa (la pantalla se entera del estado por el
// byte nuevo que se agrego a SENSOR_UPDATE, ver protocolo_labgeo.h).
// ---------------------------------------------------------------------------
static void on_uart_start(uint8_t run_id)
{
    buzzer_beep(100);
    programador_iniciar(&s_prog, run_id);
    s_estado_ensayo = (run_id == 1) ? ESTADO_CORRIDA1_INICIADA : ESTADO_CORRIDA2_INICIADA;
    ESP_LOGI(TAG, "UART START: corrida %u, nuevo estado=%d", (unsigned)run_id, (int)s_estado_ensayo);
}

static void on_uart_stop(uint8_t run_id)
{
    buzzer_beep(100);
    programador_detener(&s_prog);
    // Se usa s_prog.run_id (no el 'run_id' que llego en la trama) porque
    // programador_detener() no lo borra -- asi el estado queda bien aunque
    // la pantalla mande STOP sin run_id valido.
    s_estado_ensayo = (s_prog.run_id == 1) ? ESTADO_CORRIDA1_FINALIZADA : ESTADO_CORRIDA2_FINALIZADA;
    ESP_LOGI(TAG, "UART STOP: corrida %u, nuevo estado=%d", (unsigned)run_id, (int)s_estado_ensayo);
}

// Reenvia cada bloque que arma almacenamiento_leer_corrida() directo por
// UART -- ya viene empaquetado en el formato de 12 bytes/punto del
// protocolo (ver almacenamiento.h), no hay que reinterpretar nada.
static void uart_chunk_cb(const uint8_t *puntos_buf, uint8_t count, bool es_ultimo, void *ctx)
{
    uint8_t run_id = *(uint8_t *)ctx;
    uart_link_enviar_run_chunk(run_id, puntos_buf, count, es_ultimo ? 1 : 0);
}

// CMD_REQUEST_RUN: la pantalla pide ver la grafica de una corrida guardada
// (run_id 1 o 2) -- se busca la ultima sesion de ese tipo en corridas.csv y
// se manda entera por UART en bloques (RUN_CHUNK), via uart_chunk_cb().
static void on_uart_request_run(uint8_t run_id)
{
    ESP_LOGI(TAG, "UART REQUEST_RUN: corrida %u", (unsigned)run_id);
    almacenamiento_leer_corrida(run_id, uart_chunk_cb, &run_id);
}

// ---------------------------------------------------------------------------
// Loop de sensores (WS a ritmo fijo ~5Hz, pase lo que pase con los sensores):
// los 3 sensores (celda + 2 diales) viven en sus propias tareas (ver
// tarea_celda()/tarea_dial1()/tarea_dial2()) y solo actualizan una cache --
// esta tarea NUNCA llama a un driver de sensor directamente, solo lee las
// variables cacheadas (nunca bloquea), calibra, alimenta el programador, y
// manda todo por WS a un ritmo fijo de ~200ms.
// ---------------------------------------------------------------------------
static void tarea_sensores(void *arg)
{
    while (1) {
        int32_t dial1_crudo_um = s_dial1_um_crudo; // ultimos valores cacheados, sin bloquear
        int32_t dial2_crudo_um = s_dial2_um_crudo;
        int32_t celda_cruda = s_celda_cruda_prom; // promedio de las ultimas 3 lecturas, no la instantanea

        // Calibracion: los diales ya vienen en MICROMETROS reales del driver
        // (ver comentario arriba) -- la calibracion se aplica en ese mismo
        // dominio, y recien se pasa a mm para mostrar/mandar por WS.
        float dial1_um_cal = (float)dial1_crudo_um * s_cal.dial1_pendiente + s_cal.dial1_offset;
        float dial2_um_cal = (float)dial2_crudo_um * s_cal.dial2_pendiente + s_cal.dial2_offset;
        float dial1_mm = dial1_um_cal / 1000.0f;
        float dial2_mm = dial2_um_cal / 1000.0f;

        // Celda: misma formula que labgeo2025.ino (celda = (crudo-offset)/pendiente).
        float peso_n = (s_cal.celda_pendiente != 0.0f)
                           ? ((float)celda_cruda - s_cal.celda_offset) / s_cal.celda_pendiente
                           : 0.0f;

        int32_t dial1_um = (int32_t)dial1_um_cal;
        int32_t dial2_um = (int32_t)dial2_um_cal;
        int32_t peso_mN  = (int32_t)(peso_n * 1000.0f);

        // El programador decide si "toca" un checkpoint (por tiempo en
        // Corrida 1, por desplazamiento del Dial 2 en Corrida 2) y, si toca,
        // llama a almacenamiento_agregar_punto() el mismo.
        uint32_t tiempo_ms = 0;
        programador_actualizar(&s_prog, dial1_um, dial2_um, peso_mN, &tiempo_ms);

        // Crudo y calibrado juntos de los 3 sensores -- util para ver de un
        // vistazo si un valor calibrado raro viene del sensor (crudo tambien
        // raro) o de la calibracion (crudo normal, calibrado disparatado).
        ESP_LOGI(TAG,
                 "dial1: crudo=%" PRId32 "um cal=%.4fmm | dial2: crudo=%" PRId32 "um cal=%.4fmm | "
                 "celda: crudo=%" PRId32 " peso=%.3fN | tiempo=%" PRIu32 "ms activa=%d run=%u",
                 dial1_crudo_um, dial1_mm, dial2_crudo_um, dial2_mm,
                 celda_cruda, peso_n, tiempo_ms, s_prog.activa, (unsigned)s_prog.run_id);

        char json[192];
        snprintf(json, sizeof(json),
                 "{\"corrida\":%u,\"activa\":%s,\"dial1_mm\":%.4f,\"dial2_mm\":%.4f,"
                 "\"peso_N\":%.3f,\"tiempo_ms\":%" PRIu32 ",\"estado\":%d}",
                 (unsigned)s_prog.run_id, s_prog.activa ? "true" : "false",
                 dial1_mm, dial2_mm, peso_n, tiempo_ms, (int)s_estado_ensayo);
        servidor_web_enviar_ws(json);

        // Misma info que el WS, para que la pantalla fisica quede sincronizada
        // con el mismo estado del boton unico (ver protocolo_labgeo.h).
        uart_link_enviar_sensor_update(s_prog.run_id, dial1_um, dial2_um, peso_mN, tiempo_ms,
                                        (uint8_t)s_estado_ensayo);

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// Cuantas lecturas se promedian para cada paso de calibracion -- el HX711
// tiene ruido de muestra a muestra, y calibrar con una sola lectura instantanea
// arrastra ese ruido a offset/pendiente para siempre. Promediar unas pocas
// (nada de tiempo real de por medio en un click de calibracion) lo estabiliza.
#define CALIBRACION_LECTURAS_PROMEDIO 5

// Timeout total por si tarea_celda no esta actualizando el cache (celda
// desconectada) -- sin esto, un click de calibracion con la celda apagada
// se quedaria esperando para siempre.
#define CALIBRACION_TIMEOUT_MS 2000

// Promedia 'muestras' lecturas SIN tocar el HX711 directamente -- toma el
// valor cacheado por tarea_celda (la unica tarea que habla con el hardware)
// y espera a que cambie antes de tomar cada muestra, para asegurarse de que
// sea una conversion nueva y no la misma repetida. Antes, on_calibrar_cero()/
// on_calibrar_maximo() llamaban a hx711_read_average() por su cuenta, lo que
// hacia que esta tarea HTTP y tarea_celda pelearan por la misma conversion
// del HX711 al mismo tiempo (la seccion critica de la libreria evita que se
// corrompan los pulsos de reloj entre si, pero no evita que una tarea le
// "robe" a la otra la conversion que estaba esperando, o que ambas lean la
// misma conversion ya vencida) -- de ahi que calibrar funcionara a veces si
// y a veces no.
static bool leer_celda_promedio_cache(size_t muestras, int32_t *promedio)
{
    int64_t suma = 0;
    for (size_t i = 0; i < muestras; i++) {
        int32_t anterior = s_celda_cruda;
        uint32_t espera_ms = 0;
        while (s_celda_cruda == anterior) {
            vTaskDelay(pdMS_TO_TICKS(10));
            espera_ms += 10;
            if (espera_ms >= CALIBRACION_TIMEOUT_MS) {
                return false;
            }
        }
        suma += s_celda_cruda;
    }
    *promedio = (int32_t)(suma / (int64_t)muestras);
    return true;
}

// POST /calibrar_cero: sin peso sobre la celda, el promedio de varias
// lecturas ES el offset. Hay que llamar esto ANTES de /calibrar_maximo.
static void on_calibrar_cero(void)
{
    int32_t crudo = 0;
    if (!leer_celda_promedio_cache(CALIBRACION_LECTURAS_PROMEDIO, &crudo)) {
        ESP_LOGW(TAG, "calibrar_cero: timeout leyendo la celda, no se guarda nada");
        return;
    }

    s_cal.celda_offset = (float)crudo;
    if (config_labgeo_guardar(&s_cal) == ESP_OK) {
        ESP_LOGI(TAG, "calibrar_cero: offset=%.0f guardado en calibracion.json", s_cal.celda_offset);
    } else {
        ESP_LOGE(TAG, "calibrar_cero: no se pudo guardar calibracion.json");
    }
}

// POST /calibrar_maximo: con el peso de referencia 'peso_n' ya puesto,
// calcula la pendiente contra el offset que ya deberia estar guardado por
// /calibrar_cero. Si peso_n es 0 no hace nada (evita dividir por cero).
static void on_calibrar_maximo(float peso_n)
{
    if (peso_n == 0.0f) {
        ESP_LOGW(TAG, "calibrar_maximo: peso_n=0, no se puede calcular la pendiente");
        return;
    }

    int32_t crudo = 0;
    if (!leer_celda_promedio_cache(CALIBRACION_LECTURAS_PROMEDIO, &crudo)) {
        ESP_LOGW(TAG, "calibrar_maximo: timeout leyendo la celda, no se guarda nada");
        return;
    }

    s_cal.celda_pendiente = ((float)crudo - s_cal.celda_offset) / peso_n;
    if (config_labgeo_guardar(&s_cal) == ESP_OK) {
        ESP_LOGI(TAG, "calibrar_maximo: pendiente=%.4f guardada (crudo=%" PRId32 ", peso_n=%.2f, offset=%.0f)",
                 s_cal.celda_pendiente, crudo, peso_n, s_cal.celda_offset);
    } else {
        ESP_LOGE(TAG, "calibrar_maximo: no se pudo guardar calibracion.json");
    }
}

// POST /configurar_red: por ahora SOLO guarda ip/gateway/mascara en
// sistema.json -- todavia no los aplica al W5500 (red_eth.c sigue usando sus
// valores fijos hasta que hagamos el paso 3, pendiente a proposito).
static void on_configurar_red(const char *ip, const char *gateway, const char *mascara)
{
    config_red_t red;
    strncpy(red.ip, ip, sizeof(red.ip) - 1);
    red.ip[sizeof(red.ip) - 1] = '\0';
    strncpy(red.gateway, gateway, sizeof(red.gateway) - 1);
    red.gateway[sizeof(red.gateway) - 1] = '\0';
    strncpy(red.mascara, mascara, sizeof(red.mascara) - 1);
    red.mascara[sizeof(red.mascara) - 1] = '\0';

    if (config_labgeo_guardar_red(&red) != ESP_OK) {
        ESP_LOGE(TAG, "configurar_red: no se pudo guardar sistema.json");
    }
    // El log de exito (con los valores) ya lo hace config_labgeo_guardar_red().
}

void app_main(void)
{
    ESP_LOGI(TAG, "Diagnostico: red_eth + servidor_web + 3 sensores + almacenamiento + programador");

    // 1) Calibracion primero: monta SPIFFS "www" y carga calibracion.json (o
    // los defaults de config_labgeo_defaults.h si no existe/esta corrupto).
    // Tiene que pasar antes de arrancar los sensores, y no depende de la red.
    config_labgeo_init();
    config_labgeo_cargar(&s_cal);

    // 2) Almacenamiento: confirma que "www" ya este montada (la monto
    // config_labgeo_init() arriba) -- ahi vive corridas.csv, un solo archivo
    // para todas las corridas (ver almacenamiento.h).
    almacenamiento_init();

    // 3) Sensores + buzzer: tampoco dependen de la red -- se inicializan y
    // arrancan su tarea siempre, este o no este el Ethernet disponible.
    // hx711_init() puede devolver ESP_ERR_TIMEOUT si la celda no esta
    // conectada/alimentada todavia -- no es fatal, se loguea y se sigue
    // igual (tarea_sensores ya maneja los timeouts de lectura despues).
    esp_err_t err_celda = hx711_init(&s_celda);
    if (err_celda != ESP_OK) {
        ESP_LOGW(TAG, "hx711_init() fallo (%s) -- revisar cableado/alimentacion de la celda",
                 esp_err_to_name(err_celda));
    }
    dial_caliper_init(&s_dial1, PIN_DIAL1_REQ, PIN_DIAL1_CLK, PIN_DIAL1_DATA);
    dial_caliper_init(&s_dial2, PIN_DIAL2_REQ, PIN_DIAL2_CLK, PIN_DIAL2_DATA);

    gpio_reset_pin(PIN_BUZZER);
    gpio_set_direction(PIN_BUZZER, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BUZZER, 0);

    // Los 3 sensores en sus propias tareas (ver comentario junto a
    // s_dial1_um_crudo arriba): asi ninguno atrasa al WS. Prioridad 5, igual
    // que tarea_sensores -- ninguna es mas urgente que otra.
    //
    xTaskCreate(tarea_dial1, "tarea_dial1", 3072, NULL, 5, NULL);
    xTaskCreate(tarea_dial2, "tarea_dial2", 3072, NULL, 5, NULL);
    xTaskCreate(tarea_celda, "tarea_celda", 3072, NULL, 5, NULL);
    xTaskCreate(tarea_sensores, "tarea_sensores", 4096, NULL, 5, NULL);

    // 4) Registrar los callbacks ANTES de levantar el servidor web, mismo
    // criterio que uart_link_set_callbacks().
    servidor_web_callbacks_t web_cbs = {
        .on_calibrar_cero = on_calibrar_cero,
        .on_calibrar_maximo = on_calibrar_maximo,
        .on_configurar_red = on_configurar_red,
        .on_avanzar_ensayo = on_avanzar_ensayo,
    };
    servidor_web_set_callbacks(web_cbs);

    // Callbacks UART (pantalla fisica) -- mismo criterio: registrar ANTES de
    // uart_link_init(), para que no haya ventana donde llegue un comando y
    // no haya nadie escuchando.
    uart_link_callbacks_t uart_cbs = {
        .on_start = on_uart_start,
        .on_stop = on_uart_stop,
        .on_request_run = on_uart_request_run,
    };
    uart_link_set_callbacks(uart_cbs);
    uart_link_init();

    esp_err_t err = red_eth_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "red_eth_init() fallo (%s) -- no tiene sentido levantar el servidor web sin red", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "red_eth_init() OK");
        err = servidor_web_init();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "servidor_web_init() OK -- probar http://192.168.18.91/");
        } else {
            ESP_LOGE(TAG, "servidor_web_init() fallo (%s)", esp_err_to_name(err));
        }
    }

    buzzer_beep(200); // aviso de "equipo listo", una sola vez al terminar el arranque

    ESP_LOGI(TAG, "Listo (UART a la pantalla fisica conectado; falta solo ping_monitor, a proposito).");
}

// Esqueleto inicial de controlador_braindlab. Arma config + salidas (reles)
// + la maquina de estados de climatizacion en un unico task. Ethernet
// (W5500, red_eth.c) + servidor web (servidor_web.c) ya estan conectados:
// sirven el sitio de ../web desde SPIFFS "www" y publican el estado de
// climatizacion por WebSocket cada 2s (mismo patron que controlador_labgeo)
// -- ver publicar_estado_ws() abajo.
// RTC (DS1307, rtc_braindlab.c), el sensor de gabinete (AM2301A,
// sensor_gestor.c) y los 4 sensores de temperatura (ADS1115 "U2",
// sensores_temp.c) ya estan leyendo de verdad -- fecha_hora/temp_gestor/
// humedad_gestor/temperaturas en el JSON del WS son datos reales, no
// simulados.
//
// PENDIENTE (no implementado todavia, ver conversacion / HARDWARE.md):
//   - MicroSD por SPI (comparte bus con el W5500) -- falta definir que pin
//     sacrificar para su /CS, no queda ninguno libre.
//   - UART hacia la pantalla braindlab (GPIO26=TX, GPIO39=RX -- GPIO25 quedo
//     libre para el IRQ del W5500 en vez de esto, ver conversacion) ya
//     implementado (uart_pantalla.c/protocolo_braindlab.h), mismo protocolo
//     binario que controlador_labgeo<->blink pero con el payload propio de
//     climatizacion. Falta escribir PROTOCOLO_UART_BRAINDLAB.md.
//   - RS-485 / Modbus hacia el medidor de energia (GPIO33=TX, GPIO36=RX,
//     GPIO32=EN_485) -- bloqueado por falta de modelo/mapa de registros del
//     medidor.
//   - Rotacion semanal de la reserva (climatizacion_rotar_reserva() no se
//     llama todavia desde ningun lado) -- ya hay RTC disponible para
//     disparar esto, falta la logica de "cambio de semana".
//   - Aplicar de verdad la config de red guardada (config_braindlab_guardar_red())
//     al W5500 -- red_eth_init() todavia usa la IP fija de
//     config_braindlab_defaults.h, igual que controlador_labgeo.

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "config_braindlab.h"
#include "salidas_braindlab.h"
#include "climatizacion.h"
#include "sensores_temp.h"
#include "red_eth.h"
#include "servidor_web.h"
#include "rtc_braindlab.h"
#include "sensor_gestor.h"
#include "uart_pantalla.h"

static const char *TAG = "APP_MAIN";

// Override manual de AA1-4/bypass pedido por POST /control_aire y
// /control_bypass (ver servidor_web.c). Se escribe desde el task del
// servidor HTTP y se lee desde tarea_climatizacion -- volatile alcanza,
// mismo criterio que s_link_up en red_eth.c (bools sueltos, sin mutex).
// Persiste hasta que climatizacion_actualizar() cruce a un ciclo distinto
// del que tenia cuando se pidio el override (ver tarea_climatizacion), o
// hasta que llegue POST /control_automatico.
static volatile bool s_aire_manual_activo[SALIDAS_NUM_AIRES] = { false, false, false, false };
static volatile bool s_aire_manual_valor[SALIDAS_NUM_AIRES]  = { false, false, false, false };
static volatile bool s_bypass_manual_activo = false;
static volatile bool s_bypass_manual_valor  = false;
static volatile bool s_at_manual_activo = false;
static volatile bool s_at_manual_valor  = false;

// true si algun override manual (AA1-4, bypass o AT) esta vigente -- lo usan
// publicar_estado_ws() (WS) y uart_pantalla_enviar_estado() (UART), mismo
// dato por los dos canales.
static bool hay_algun_override_manual(void)
{
    if (s_bypass_manual_activo || s_at_manual_activo) {
        return true;
    }
    for (int i = 0; i < SALIDAS_NUM_AIRES; i++) {
        if (s_aire_manual_activo[i]) {
            return true;
        }
    }
    return false;
}

// Arma el mismo JSON que manda tarea_climatizacion por WS a cada vuelta del
// loop (ver servidor_web.c) -- lo lee script.js en ../web/script.js.
// 'salida_aplicada'/'bypass_aplicado' son los valores REALES que se
// mandaron a los reles esta vuelta (mezcla de automatico + override manual),
// no el estado.salida_aire crudo -- asi el dashboard siempre muestra lo que
// esta pasando de verdad en el hardware.
static void publicar_estado_ws(const float temperaturas[4], const clima_estado_t *estado,
                                const bool salida_aplicada[4], bool bypass_aplicado, bool alarma_at_aplicada,
                                const char *fecha_hora, bool gestor_ok, float temp_gestor, float humedad_gestor)
{
    bool modo_manual = hay_algun_override_manual();

    char json[560];
    snprintf(json, sizeof(json),
             "{\"ciclo\":%d,"
             "\"temperaturas\":[%.1f,%.1f,%.1f,%.1f],"
             "\"salida_aire\":[%s,%s,%s,%s],"
             "\"aire_manual\":[%s,%s,%s,%s],"
             "\"indice_reserva\":%u,"
             "\"alarma_at\":%s,"
             "\"alarma_at_manual\":%s,"
             "\"bypass_solicitado\":%s,"
             "\"bypass_manual\":%s,"
             "\"bypass_activo\":%s,"
             "\"modo_manual\":%s,"
             "\"eth_conectado\":%s,"
             "\"uptime_s\":%lld,"
             "\"fecha_hora\":\"%s\","
             "\"gestor_ok\":%s,"
             "\"temp_gestor\":%.1f,"
             "\"humedad_gestor\":%.1f}",
             (int)estado->ciclo,
             temperaturas[0], temperaturas[1], temperaturas[2], temperaturas[3],
             salida_aplicada[0] ? "true" : "false",
             salida_aplicada[1] ? "true" : "false",
             salida_aplicada[2] ? "true" : "false",
             salida_aplicada[3] ? "true" : "false",
             s_aire_manual_activo[0] ? "true" : "false",
             s_aire_manual_activo[1] ? "true" : "false",
             s_aire_manual_activo[2] ? "true" : "false",
             s_aire_manual_activo[3] ? "true" : "false",
             (unsigned)estado->indice_reserva,
             alarma_at_aplicada ? "true" : "false",
             s_at_manual_activo ? "true" : "false",
             bypass_aplicado ? "true" : "false",
             s_bypass_manual_activo ? "true" : "false",
             salidas_leer_bypass_activo() ? "true" : "false",
             modo_manual ? "true" : "false",
             red_eth_esta_conectado() ? "true" : "false",
             esp_timer_get_time() / 1000000LL,
             fecha_hora,
             gestor_ok ? "true" : "false",
             temp_gestor, humedad_gestor);
    servidor_web_enviar_ws(json);
}

static void tarea_climatizacion(void *arg)
{
    config_climatizacion_t cfg;
    config_braindlab_cargar_climatizacion(&cfg);

    clima_estado_t estado;
    climatizacion_iniciar(&estado);
    clima_ciclo_t ciclo_previo = estado.ciclo;

    // Por debajo de cualquier temp_min razonable -- si sensores_temp_leer()
    // fallara en la primerisima vuelta (antes de tener un valor real
    // guardado), climatizacion_actualizar() se queda en REPOSO en vez de
    // accionar reles con memoria de stack sin inicializar.
    float temperaturas[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    // Ultimo valor bueno del AM2301A (sensor_gestor.c) -- se actualiza solo
    // si la lectura de esta vuelta sale bien; si falla (es normal que un
    // sensor de un hilo falle de vez en cuando), se sigue publicando el
    // ultimo valor conocido con gestor_ok=false en vez de un numero
    // inventado o cortar el resto del loop.
    float temp_gestor = 0.0f, humedad_gestor = 0.0f;
    bool gestor_ok = false;

    while (1) {
        sensores_temp_leer(temperaturas);
        climatizacion_actualizar(temperaturas, &cfg, &estado);

        float temp_gestor_leida, humedad_gestor_leida;
        gestor_ok = sensor_gestor_leer(&temp_gestor_leida, &humedad_gestor_leida) == ESP_OK;
        if (gestor_ok) {
            temp_gestor = temp_gestor_leida;
            humedad_gestor = humedad_gestor_leida;
        }

        struct tm ahora = { 0 };
        char fecha_hora[24] = "----";
        bool rtc_ok = rtc_braindlab_leer(&ahora) == ESP_OK;
        if (rtc_ok) {
            strftime(fecha_hora, sizeof(fecha_hora), "%Y-%m-%d %H:%M:%S", &ahora);
        } else {
            // Antes esto fallaba en silencio (rtc_braindlab_leer() solo
            // devuelve el error, no loguea) -- a diferencia de
            // sensores_temp_leer()/sensor_gestor_leer(), que si avisan cada
            // vez que fallan. Se agrega el mismo criterio aca.
            ESP_LOGW(TAG, "No se pudo leer el RTC -- se publica fecha_hora=\"----\"");
        }

        // Se cruzo un setpoint (cambio de ciclo) -- la automatica retoma el
        // control de todo, se cancela cualquier override manual vigente.
        if (estado.ciclo != ciclo_previo) {
            for (int i = 0; i < SALIDAS_NUM_AIRES; i++) {
                s_aire_manual_activo[i] = false;
            }
            s_bypass_manual_activo = false;
            s_at_manual_activo = false;
            ciclo_previo = estado.ciclo;
        }

        bool salida_aplicada[SALIDAS_NUM_AIRES];
        for (int i = 0; i < SALIDAS_NUM_AIRES; i++) {
            salida_aplicada[i] = s_aire_manual_activo[i] ? s_aire_manual_valor[i] : estado.salida_aire[i];
            salidas_set_aire(i, salida_aplicada[i]);
        }

        bool alarma_at_aplicada = s_at_manual_activo ? s_at_manual_valor : estado.alarma_at;
        salidas_set_alarma_at(alarma_at_aplicada);

        bool bypass_aplicado = s_bypass_manual_activo ? s_bypass_manual_valor : estado.bypass_solicitado;
        salidas_set_bypass_solicitado(bypass_aplicado);

        publicar_estado_ws(temperaturas, &estado, salida_aplicada, bypass_aplicado, alarma_at_aplicada,
                           fecha_hora, gestor_ok, temp_gestor, humedad_gestor);

        // Mismo estado, pero por UART hacia la pantalla braindlab (ver
        // uart_pantalla.c/protocolo_braindlab.h) -- anio=0 si no hay RTC
        // disponible esta vuelta, mismo criterio que fecha_hora="----" arriba.
        bool aire_manual_actual[SALIDAS_NUM_AIRES];
        for (int i = 0; i < SALIDAS_NUM_AIRES; i++) {
            aire_manual_actual[i] = s_aire_manual_activo[i];
        }
        uart_pantalla_enviar_estado(
            (uint8_t)estado.ciclo, temperaturas, salida_aplicada, aire_manual_actual,
            estado.indice_reserva, alarma_at_aplicada, s_at_manual_activo,
            bypass_aplicado, s_bypass_manual_activo, salidas_leer_bypass_activo(),
            hay_algun_override_manual(), red_eth_esta_conectado(),
            gestor_ok, temp_gestor, humedad_gestor,
            (uint32_t)(esp_timer_get_time() / 1000000LL),
            rtc_ok ? (uint16_t)(ahora.tm_year + 1900) : 0,
            rtc_ok ? (uint8_t)(ahora.tm_mon + 1) : 0,
            rtc_ok ? (uint8_t)ahora.tm_mday : 0,
            rtc_ok ? (uint8_t)ahora.tm_hour : 0,
            rtc_ok ? (uint8_t)ahora.tm_min : 0,
            rtc_ok ? (uint8_t)ahora.tm_sec : 0);

        if (red_eth_esta_conectado()) {
            ESP_LOGI(TAG, "Ethernet: conectado");
        } else {
            ESP_LOGW(TAG, "Ethernet: cable desconectado (sin link)");
        }

        // TODO: reemplazar por el disparo real de rotacion semanal (RTC,
        // igual que semana/semana_last en neuvov2.ino) cuando el modulo de
        // red/RTC este listo. Por ahora climatizacion_rotar_reserva() no se
        // llama desde ningun lado.

        vTaskDelay(pdMS_TO_TICKS(2000)); // mismo periodo que neuvov2.ino (2s)
    }
}

// POST /configurar_red: por ahora SOLO guarda ip/gateway/mascara en
// config.json -- todavia no los aplica al W5500 (red_eth.c sigue usando sus
// valores fijos hasta que se implemente ese paso, igual que controlador_labgeo).
static void on_configurar_red(const char *ip, const char *gateway, const char *mascara)
{
    config_red_t red;
    strncpy(red.ip, ip, sizeof(red.ip) - 1);
    red.ip[sizeof(red.ip) - 1] = '\0';
    strncpy(red.gateway, gateway, sizeof(red.gateway) - 1);
    red.gateway[sizeof(red.gateway) - 1] = '\0';
    strncpy(red.mascara, mascara, sizeof(red.mascara) - 1);
    red.mascara[sizeof(red.mascara) - 1] = '\0';

    if (config_braindlab_guardar_red(&red) != ESP_OK) {
        ESP_LOGE(TAG, "configurar_red: no se pudo guardar config.json");
    }
    // El log de exito (con los valores) ya lo hace config_braindlab_guardar_red().
}

// POST /configurar_climatizacion: guarda los 7 parametros en config.json.
// tarea_climatizacion recien los relee la proxima vez que arranca (no hay
// aplicacion en caliente todavia -- mismo criterio "solo guardar" que
// on_configurar_red/on_configurar_equipo en controlador_labgeo).
static void on_configurar_climatizacion(uint8_t cantidad_aires, float temp_min, float temp_max, float temp_at,
                                         float temp_bypass, uint8_t fails_max_bypass, bool rotar_reserva)
{
    config_climatizacion_t cfg = {
        .cantidad_aires = cantidad_aires,
        .temp_min = temp_min,
        .temp_max = temp_max,
        .temp_at = temp_at,
        .temp_bypass = temp_bypass,
        .fails_max_bypass = fails_max_bypass,
        .rotar_reserva = rotar_reserva,
    };

    if (config_braindlab_guardar_climatizacion(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "configurar_climatizacion: no se pudo guardar config.json");
    }
}

// POST /control_aire: fuerza a mano AA(indice+1) hasta que la automatica
// cruce a otro ciclo (ver s_aire_manual_activo arriba y tarea_climatizacion).
static void on_control_aire(uint8_t indice, bool encendido)
{
    if (indice >= SALIDAS_NUM_AIRES) {
        ESP_LOGW(TAG, "control_aire: indice %u fuera de rango, ignorado", indice);
        return;
    }
    s_aire_manual_valor[indice] = encendido;
    s_aire_manual_activo[indice] = true;
    ESP_LOGI(TAG, "control_aire: AA%u forzado a mano a %s", (unsigned)indice + 1, encendido ? "ON" : "OFF");
}

// POST /control_bypass: fuerza a mano bypass_solicitado (BP_S) hasta que la
// automatica cruce a otro ciclo.
static void on_control_bypass(bool solicitado)
{
    s_bypass_manual_valor = solicitado;
    s_bypass_manual_activo = true;
    ESP_LOGI(TAG, "control_bypass: bypass_solicitado forzado a mano a %s", solicitado ? "ON" : "OFF");
}

// POST /control_at: fuerza a mano OUT_AT hasta que la automatica cruce a
// otro ciclo.
static void on_control_at(bool activa)
{
    s_at_manual_valor = activa;
    s_at_manual_activo = true;
    ESP_LOGI(TAG, "control_at: OUT_AT forzado a mano a %s", activa ? "ON" : "OFF");
}

// POST /configurar_rtc: ajusta el DS1307. rtc_braindlab_ajustar() se
// encarga de calcular tm_wday (dia de la semana) via mktime(), no hace
// falta que este callback lo sepa.
static void on_configurar_rtc(uint16_t anio, uint8_t mes, uint8_t dia, uint8_t hora, uint8_t minuto,
                               uint8_t segundo)
{
    struct tm tiempo = {
        .tm_year = anio - 1900,
        .tm_mon = mes - 1,
        .tm_mday = dia,
        .tm_hour = hora,
        .tm_min = minuto,
        .tm_sec = segundo,
    };

    if (rtc_braindlab_ajustar(&tiempo) != ESP_OK) {
        ESP_LOGE(TAG, "configurar_rtc: no se pudo ajustar el DS1307");
    }
    // El log de exito (con la fecha/hora aplicada) ya lo hace rtc_braindlab_ajustar().
}

// POST /control_automatico: cancela cualquier override vigente, la
// automatica retoma el control de todo en el proximo ciclo.
static void on_control_automatico(void)
{
    for (int i = 0; i < SALIDAS_NUM_AIRES; i++) {
        s_aire_manual_activo[i] = false;
    }
    s_bypass_manual_activo = false;
    s_at_manual_activo = false;
    ESP_LOGI(TAG, "control_automatico: overrides manuales cancelados");
}

void app_main(void)
{
    ESP_LOGI(TAG, "controlador_braindlab -- esqueleto: config + salidas + climatizacion (sensores simulados)");

    ESP_ERROR_CHECK(config_braindlab_init());
    ESP_ERROR_CHECK(salidas_init());

    // No ESP_ERROR_CHECK -- ahora que lee de verdad el ADS1115 por I2C
    // (antes era simulado y siempre devolvia ESP_OK, un ESP_ERROR_CHECK aca
    // era inofensivo), si el chip no responde (todavia sin cablear, por
    // ejemplo) seguimos igual: climatizacion_actualizar() se queda en
    // REPOSO con temperaturas[]=0.0 en vez de reiniciar todo el equipo.
    esp_err_t err_sensores = sensores_temp_init();
    if (err_sensores != ESP_OK) {
        ESP_LOGW(TAG, "sensores_temp_init() fallo (%s) -- sigue sin ADS1115, climatizacion se queda en reposo",
                 esp_err_to_name(err_sensores));
    }

    // No ESP_ERROR_CHECK -- si el DS1307 no responde (I2C sin cablear
    // todavia, etc.) seguimos igual, tarea_climatizacion se da cuenta sola
    // (rtc_braindlab_leer() devuelve error) y publica fecha_hora="----".
    esp_err_t err_rtc = rtc_braindlab_init();
    if (err_rtc != ESP_OK) {
        ESP_LOGW(TAG, "rtc_braindlab_init() fallo (%s) -- sigue sin RTC, se publica fecha_hora=\"----\"",
                 esp_err_to_name(err_rtc));
    }

    xTaskCreate(tarea_climatizacion, "tarea_climatizacion", 4096, NULL, 5, NULL);

    // Registrar los callbacks ANTES de levantar el servidor web, mismo
    // criterio que controlador_labgeo.
    servidor_web_callbacks_t web_cbs = {
        .on_configurar_red = on_configurar_red,
        .on_configurar_climatizacion = on_configurar_climatizacion,
        .on_control_aire = on_control_aire,
        .on_control_bypass = on_control_bypass,
        .on_control_at = on_control_at,
        .on_control_automatico = on_control_automatico,
        .on_configurar_rtc = on_configurar_rtc,
    };
    servidor_web_set_callbacks(web_cbs);

    // Mismos callbacks que el servidor web -- la pantalla braindlab es un
    // cliente mas del mismo mecanismo de override manual, solo que por UART
    // en vez de HTTP (ver uart_pantalla.c/protocolo_braindlab.h).
    uart_pantalla_callbacks_t uart_cbs = {
        .on_control_aire = on_control_aire,
        .on_control_bypass = on_control_bypass,
        .on_control_at = on_control_at,
        .on_control_automatico = on_control_automatico,
    };
    uart_pantalla_set_callbacks(uart_cbs);
    uart_pantalla_init();

    // Red + servidor web al final: no bloquean el arranque de la
    // climatizacion (esa tarea ya esta corriendo arriba) ni dependen de
    // ella. Si el W5500 no responde, red_eth_init() devuelve error, no
    // aborta -- seguimos sin red antes que dejar el equipo sin climatizar.
    esp_err_t err_red = red_eth_init();
    if (err_red != ESP_OK) {
        ESP_LOGE(TAG, "red_eth_init() fallo (%s) -- sigue sin red, la climatizacion no depende de esto",
                 esp_err_to_name(err_red));
    } else {
        esp_err_t err_web = servidor_web_init();
        if (err_web == ESP_OK) {
            ESP_LOGI(TAG, "servidor_web_init() OK -- probar http://192.168.5.95/");
        } else {
            ESP_LOGE(TAG, "servidor_web_init() fallo (%s)", esp_err_to_name(err_web));
        }
    }

    // Si este arranque viene de un firmware recien subido por POST /ota (ver
    // servidor_web.c) y llego hasta aca sin resetearse solo, se confirma
    // como valido -- CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y (sdkconfig.defaults)
    // hace que si NUNCA se llama esto, el bootloader vuelva solo al firmware
    // anterior en el proximo reset en vez de reintentar uno roto para
    // siempre. Mismo criterio que controlador_labgeo.
    esp_ota_mark_app_valid_cancel_rollback();
}

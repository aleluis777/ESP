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
//   - MicroSD por SPI (comparte bus con el W5500, /CS=GPIO0) ya registra una
//     fila por minuto en /sd/LOG/AAAA/MM/DD.CSV (registro_sd.c). Falta: los
//     resumenes por hora/dia para graficar mes/anio y el endpoint para
//     pedir los datos desde la web/pantalla.
//   - UART hacia la pantalla braindlab (GPIO26=TX, GPIO39=RX -- GPIO25 quedo
//     libre para el IRQ del W5500 en vez de esto, ver conversacion) ya
//     implementado (uart_pantalla.c/protocolo_braindlab.h), mismo protocolo
//     binario que controlador_labgeo<->blink pero con el payload propio de
//     climatizacion. Falta escribir PROTOCOLO_UART_BRAINDLAB.md.
//   - RS-485 / Modbus hacia el medidor JSY-MK-333G (GPIO33=TX, GPIO36=RX,
//     GPIO32=EN_485) ya implementado (modbus_braindlab.c): voltajes y
//     corrientes R/S/T, van al WS, a la SD y a la pantalla por UART. Falta
//     SNMP.
//   - Rotacion semanal de la reserva (climatizacion_rotar_reserva() no se
//     llama todavia desde ningun lado) -- ya hay RTC disponible para
//     disparar esto, falta la logica de "cambio de semana".

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
#include "protocolo_braindlab.h"
#include "snmp_braindlab.h"
#include "registro_sd.h"
#include "modbus_braindlab.h"
#include "auth_web.h"

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

// Constantes de calibracion (ver config_calibracion_t) -- cargadas del
// config.json UNA sola vez al arrancar, despues viven solo en RAM: asi
// tarea_climatizacion no toca el JSON en cada vuelta del loop (cada 2s), y
// on_calibrar_sensor() puede actualizarlas en caliente sin reiniciar (ver
// mas abajo) con solo escribir estas variables -- la proxima vuelta del
// loop ya las usa. volatile + floats sueltos, mismo criterio "sin mutex"
// que los overrides manuales de arriba: cada campo se escribe/lee entero de
// una, no hace falta mas para este uso.
static volatile float s_calib_ntc[4];
static volatile float s_calib_temp_gestor;
static volatile float s_calib_humedad_gestor;

// Ultimas lecturas CRUDAS (sin calibrar) de cada sensor, actualizadas por
// tarea_climatizacion cada 2s -- la unica que le pega al hardware
// (sensores_temp_leer() no es segura para llamarse desde dos tareas a la
// vez, ver sensores_temp.h). on_calibrar_sensor() (task del servidor HTTP)
// lee de aca en vez de volver a tocar el ADS1115/AM2301A: la constante
// nueva sale de restar el valor de referencia contra este dato crudo, no
// hace falta ninguna resta extra del lado de la calibracion. Los usos que
// SI necesitan el valor calibrado (climatizacion_actualizar(), el JSON del
// WS, el UART a la pantalla) le suman s_calib_ntc/s_calib_temp_gestor/
// s_calib_humedad_gestor recien en el momento de usarlo -- asi una
// calibracion nueva se nota en la proxima vuelta del loop, no hace falta
// reiniciar.
static volatile float s_ultimas_temperaturas_crudas[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
static volatile float s_ultimo_temp_gestor_crudo = 0.0f;
static volatile float s_ultimo_humedad_gestor_crudo = 0.0f;

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
                                const char *fecha_hora, bool gestor_ok, float temp_gestor, float humedad_gestor,
                                const modbus_lectura_t *energia, bool rtc_ok, bool rtc_detenido, bool adc_ok,
                                const bool ntc_ok[4])
{
    bool modo_manual = hay_algun_override_manual();

    char json[860];
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
             "\"humedad_gestor\":%.1f,"
             "\"energia_ok\":%s,"
             "\"voltajes\":[%.1f,%.1f,%.1f],"
             "\"corrientes\":[%.2f,%.2f,%.2f],"
             "\"sd_estado\":%d,"
             "\"rtc_ok\":%s,"
             "\"rtc_detenido\":%s,"
             "\"adc_ok\":%s,"
             "\"ntc_ok\":[%s,%s,%s,%s]}",
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
             temp_gestor, humedad_gestor,
             energia->ok ? "true" : "false",
             energia->voltajes[0], energia->voltajes[1], energia->voltajes[2],
             energia->corrientes[0], energia->corrientes[1], energia->corrientes[2],
             (int)registro_sd_estado(),
             rtc_ok ? "true" : "false",
             rtc_detenido ? "true" : "false",
             adc_ok ? "true" : "false",
             ntc_ok[0] ? "true" : "false",
             ntc_ok[1] ? "true" : "false",
             ntc_ok[2] ? "true" : "false",
             ntc_ok[3] ? "true" : "false");
    servidor_web_enviar_ws(json);
}

static void tarea_climatizacion(void *arg)
{
    config_climatizacion_t cfg;
    config_braindlab_cargar_climatizacion(&cfg);

    config_calibracion_t calib_inicial;
    config_braindlab_cargar_calibracion(&calib_inicial);
    s_calib_ntc[0] = calib_inicial.t1;
    s_calib_ntc[1] = calib_inicial.t2;
    s_calib_ntc[2] = calib_inicial.t3;
    s_calib_ntc[3] = calib_inicial.t4;
    s_calib_temp_gestor = calib_inicial.temp_gestor;
    s_calib_humedad_gestor = calib_inicial.humedad_gestor;

    clima_estado_t estado;
    climatizacion_iniciar(&estado);
    clima_ciclo_t ciclo_previo = estado.ciclo;
    int ultimo_minuto_registrado = -1; // registro_sd: una fila por minuto

    // Crudas: por debajo de cualquier temp_min razonable -- si
    // sensores_temp_leer() fallara en la primerisima vuelta (antes de tener
    // un valor real guardado), climatizacion_actualizar() se queda en
    // REPOSO en vez de accionar reles con memoria de stack sin inicializar.
    // Sin calibrar (constante en 0): la calibracion se suma recien mas
    // abajo, en el momento de usar el valor -- ver comentario junto a
    // s_ultimas_temperaturas_crudas arriba.
    static const float sin_calibrar[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float temperaturas_crudas[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float temperaturas[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // calibradas, para climatizacion/WS/UART

    // Ultimo valor bueno CRUDO del AM2301A (sensor_gestor.c) -- se actualiza
    // solo si la lectura de esta vuelta sale bien; si falla (es normal que
    // un sensor de un hilo falle de vez en cuando), se sigue publicando el
    // ultimo valor conocido con gestor_ok=false en vez de un numero
    // inventado o cortar el resto del loop.
    float temp_gestor_crudo = 0.0f, humedad_gestor_crudo = 0.0f;
    bool gestor_ok = false;

    while (1) {
        // adc_ok: el ADS1115 respondio por I2C. ntc_ok[i]: ademas el NTC de
        // ese canal esta conectado (sin NTC la entrada queda en ~3V3 y se
        // rechaza -- no es falla del ADS, ver sensores_temp.h).
        // sensores_temp_ok (SNMP): todo bien, chip y los 4 NTC.
        bool ntc_ok[4];
        bool adc_ok = sensores_temp_leer(temperaturas_crudas, sin_calibrar, ntc_ok) == ESP_OK;
        bool sensores_temp_ok = adc_ok && ntc_ok[0] && ntc_ok[1] && ntc_ok[2] && ntc_ok[3];
        for (int i = 0; i < 4; i++) {
            s_ultimas_temperaturas_crudas[i] = temperaturas_crudas[i];
            temperaturas[i] = temperaturas_crudas[i] + s_calib_ntc[i];
        }
        climatizacion_actualizar(temperaturas, &cfg, &estado);

        float temp_gestor_leido, humedad_gestor_leido;
        gestor_ok = sensor_gestor_leer(&temp_gestor_leido, &humedad_gestor_leido, 0.0f, 0.0f) == ESP_OK;
        if (gestor_ok) {
            temp_gestor_crudo = temp_gestor_leido;
            humedad_gestor_crudo = humedad_gestor_leido;
            s_ultimo_temp_gestor_crudo = temp_gestor_crudo;
            s_ultimo_humedad_gestor_crudo = humedad_gestor_crudo;
        }
        float temp_gestor = temp_gestor_crudo + s_calib_temp_gestor;
        float humedad_gestor = humedad_gestor_crudo + s_calib_humedad_gestor;

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

        // Ultima lectura del medidor -- copia inmediata, nunca espera al
        // RS-485 (eso lo hace solo tarea_modbus, ver modbus_braindlab.h).
        modbus_lectura_t energia;
        modbus_braindlab_leer(&energia);

        publicar_estado_ws(temperaturas, &estado, salida_aplicada, bypass_aplicado, alarma_at_aplicada,
                           fecha_hora, gestor_ok, temp_gestor, humedad_gestor, &energia,
                           rtc_ok, rtc_braindlab_detenido(), adc_ok, ntc_ok);

        // Estado de cada modulo en tiempo real, para la pantalla (bitmask,
        // ver BRAINDLAB_FALLA_* en protocolo_braindlab.h). La web recibe lo
        // mismo pero como campos sueltos en el JSON.
        uint8_t fallas = 0;
        if (!rtc_ok || rtc_braindlab_detenido())       fallas |= BRAINDLAB_FALLA_RTC;
        if (!adc_ok)                                   fallas |= BRAINDLAB_FALLA_ADC;
        if (!gestor_ok)                                fallas |= BRAINDLAB_FALLA_GESTOR;
        if (!red_eth_esta_conectado())                 fallas |= BRAINDLAB_FALLA_ETH;
        if (!energia.ok)                               fallas |= BRAINDLAB_FALLA_MODBUS;
        if (registro_sd_estado() != REGISTRO_SD_OK)    fallas |= BRAINDLAB_FALLA_SD;

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
            rtc_ok ? (uint8_t)ahora.tm_sec : 0,
            energia.ok, energia.voltajes, energia.corrientes,
            (uint8_t)registro_sd_estado(), fallas);

        // Misma foto para el agente SNMP (GET) -- ademas detecta ahi los
        // eventos que generan traps (ver snmp_braindlab.c). No bloquea.
        snmp_braindlab_estado_t estado_snmp = {
            .ciclo = (uint8_t)estado.ciclo,
            .sensores_temp_ok = sensores_temp_ok,
            .gestor_ok = gestor_ok,
            .temp_gestor = temp_gestor,
            .humedad_gestor = humedad_gestor,
            .rtc_ok = rtc_ok,
            .alarma_at = alarma_at_aplicada,
            .at_manual = s_at_manual_activo,
            .bypass_solicitado = bypass_aplicado,
            .bypass_manual = s_bypass_manual_activo,
            .bypass_activo = salidas_leer_bypass_activo(),
            .modo_manual = hay_algun_override_manual(),
            .indice_reserva = estado.indice_reserva,
            .bypass_auto_habilitado = estado.bypass_auto_habilitado,
            .cantidad_aires = cfg.cantidad_aires,
            .temp_min = cfg.temp_min,
            .temp_max = cfg.temp_max,
            .temp_at = cfg.temp_at,
            .temp_bypass = cfg.temp_bypass,
            .calibracion = { s_calib_ntc[0], s_calib_ntc[1], s_calib_ntc[2], s_calib_ntc[3],
                             s_calib_temp_gestor, s_calib_humedad_gestor },
        };
        for (int i = 0; i < 4; i++) {
            estado_snmp.temperaturas[i] = temperaturas[i];
            estado_snmp.salida_aire[i] = salida_aplicada[i];
            estado_snmp.aire_manual[i] = aire_manual_actual[i];
        }
        snmp_braindlab_publicar_estado(&estado_snmp);

        // Historico en la MicroSD: una fila por minuto (el loop corre cada
        // 2s, se registra la primera vuelta de cada minuto nuevo). Sin RTC
        // no se registra -- no hay como saber en que archivo de dia va.
        if (rtc_ok && ahora.tm_min != ultimo_minuto_registrado) {
            ultimo_minuto_registrado = ahora.tm_min;
            registro_sd_muestra_t muestra = {
                .fecha_hora = ahora,
                .temp_gestor = temp_gestor,
                .humedad_gestor = humedad_gestor,
                .gestor_ok = gestor_ok,
                .alarma_at = alarma_at_aplicada,
                .bypass_activo = salidas_leer_bypass_activo(),
                .ciclo = (uint8_t)estado.ciclo,
                .energia_ok = energia.ok,
            };
            for (int i = 0; i < 4; i++) {
                muestra.temperaturas[i] = temperaturas[i];
                muestra.temp_ok[i] = adc_ok && ntc_ok[i];
                muestra.salida_aire[i] = salida_aplicada[i];
            }
            for (int i = 0; i < MODBUS_NUM_FASES; i++) {
                muestra.voltajes[i] = energia.voltajes[i];
                muestra.corrientes[i] = energia.corrientes[i];
            }
            registro_sd_encolar(&muestra);
        }

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

// POST /calibrar_sensor: NO toca el ADS1115/AM2301A directamente -- lee la
// ULTIMA lectura CRUDA ya cacheada por tarea_climatizacion
// (s_ultimas_temperaturas_crudas/s_ultimo_temp_gestor_crudo/
// s_ultimo_humedad_gestor_crudo, actualizadas cada 2s), calcula
// constante = valor_referencia - crudo, la guarda en config.json Y
// actualiza la variable en RAM correspondiente (s_calib_ntc[i]/
// s_calib_temp_gestor/s_calib_humedad_gestor) -- tarea_climatizacion las
// suma recien al usar el valor (ver el loop arriba), asi que la proxima
// vuelta (maximo 2s) ya publica con la constante nueva, sin reiniciar.
static void on_calibrar_sensor(const char *sensor, float valor_referencia)
{
    float crudo;

    if (strcmp(sensor, "t1") == 0)      crudo = s_ultimas_temperaturas_crudas[0];
    else if (strcmp(sensor, "t2") == 0) crudo = s_ultimas_temperaturas_crudas[1];
    else if (strcmp(sensor, "t3") == 0) crudo = s_ultimas_temperaturas_crudas[2];
    else if (strcmp(sensor, "t4") == 0) crudo = s_ultimas_temperaturas_crudas[3];
    else if (strcmp(sensor, "temp_gestor") == 0)    crudo = s_ultimo_temp_gestor_crudo;
    else if (strcmp(sensor, "humedad_gestor") == 0) crudo = s_ultimo_humedad_gestor_crudo;
    else {
        ESP_LOGW(TAG, "calibrar_sensor: \"%s\" no reconocido (t1-t4/temp_gestor/humedad_gestor)", sensor);
        return;
    }

    float nueva_constante = valor_referencia - crudo;
    uint8_t indice_sensor; // mismo orden que config_calibracion_t, para el trap SNMP

    config_calibracion_t calib;
    config_braindlab_cargar_calibracion(&calib); // conserva la calibracion de los demas sensores
    if (strcmp(sensor, "t1") == 0)               { calib.t1 = nueva_constante; s_calib_ntc[0] = nueva_constante; indice_sensor = 0; }
    else if (strcmp(sensor, "t2") == 0)          { calib.t2 = nueva_constante; s_calib_ntc[1] = nueva_constante; indice_sensor = 1; }
    else if (strcmp(sensor, "t3") == 0)          { calib.t3 = nueva_constante; s_calib_ntc[2] = nueva_constante; indice_sensor = 2; }
    else if (strcmp(sensor, "t4") == 0)          { calib.t4 = nueva_constante; s_calib_ntc[3] = nueva_constante; indice_sensor = 3; }
    else if (strcmp(sensor, "temp_gestor") == 0) { calib.temp_gestor = nueva_constante; s_calib_temp_gestor = nueva_constante; indice_sensor = 4; }
    else                                          { calib.humedad_gestor = nueva_constante; s_calib_humedad_gestor = nueva_constante; indice_sensor = 5; }

    if (config_braindlab_guardar_calibracion(&calib) == ESP_OK) {
        ESP_LOGI(TAG, "calibrar_sensor: %s crudo=%.2f referencia=%.2f -> constante=%+.2f guardada y aplicada",
                 sensor, crudo, valor_referencia, nueva_constante);
    } else {
        ESP_LOGE(TAG, "calibrar_sensor: no se pudo guardar la nueva constante de %s (queda aplicada en RAM igual)", sensor);
    }
    snmp_braindlab_notificar_calibracion(indice_sensor, nueva_constante);
}

// POST /sd_formatear (la confirmacion ya la valido servidor_web.c). 60 s
// de margen: formatear una tarjeta grande por SPI tarda varios segundos.
static esp_err_t on_formatear_sd(void)
{
    return registro_sd_formatear(60000);
}

void app_main(void)
{
    ESP_LOGI(TAG, "controlador_braindlab -- esqueleto: config + salidas + climatizacion");

    ESP_ERROR_CHECK(config_braindlab_init());
    ESP_ERROR_CHECK(salidas_init());
    salidas_buzzer_pitido(150); // pitido de arranque: el modulo prendio

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

    // MicroSD: ANTES de red_eth_init() (inicializa el bus SPI compartido,
    // ver registro_sd.h) y antes de tarea_climatizacion (que le encola las
    // muestras). Sin tarjeta seguimos igual, el modulo reintenta solo.
    esp_err_t err_sd = registro_sd_init();
    if (err_sd != ESP_OK) {
        ESP_LOGW(TAG, "registro_sd_init() fallo (%s) -- sigue sin SD, se reintenta cada minuto",
                 esp_err_to_name(err_sd));
    }

    // Medidor de energia por RS-485: no espera al medidor, si no esta
    // conectado la lectura queda en ok=false y tarea_modbus reintenta sola.
    esp_err_t err_mb = modbus_braindlab_init();
    if (err_mb != ESP_OK) {
        ESP_LOGE(TAG, "modbus_braindlab_init() fallo (%s) -- sigue sin medidor", esp_err_to_name(err_mb));
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
        .on_calibrar_sensor = on_calibrar_sensor,
        .on_formatear_sd = on_formatear_sd,
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
    // Login de la web: contrasena en NVS (ver auth_web.h). Sin NVS la web
    // queda cerrada (rechaza todo) en vez de abierta sin contrasena.
    esp_err_t err_auth = auth_web_init();
    if (err_auth != ESP_OK) {
        ESP_LOGE(TAG, "auth_web_init() fallo (%s) -- la web va a rechazar todos los logins",
                 esp_err_to_name(err_auth));
    }

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

        // SNMP: otro cliente mas, mismos callbacks que la web (un SET hace
        // exactamente lo mismo que el POST equivalente). Independiente del
        // servidor web: si uno falla, el otro sigue.
        snmp_braindlab_callbacks_t snmp_cbs = {
            .on_control_aire = on_control_aire,
            .on_control_bypass = on_control_bypass,
            .on_control_at = on_control_at,
            .on_control_automatico = on_control_automatico,
            .on_calibrar_sensor = on_calibrar_sensor,
        };
        snmp_braindlab_set_callbacks(snmp_cbs);
        esp_err_t err_snmp = snmp_braindlab_init();
        if (err_snmp != ESP_OK) {
            ESP_LOGE(TAG, "snmp_braindlab_init() fallo (%s)", esp_err_to_name(err_snmp));
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

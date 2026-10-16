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

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_system.h"
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
#include "eventos.h"

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

// Sensores 1..6 instalados y sus nombres (config.json, seccion "sensores").
// Los cambia on_configurar_sitio() en caliente desde el servidor web;
// tarea_climatizacion solo copia instalado[] a cada vuelta.
static config_sensores_t s_sensores;
static char s_nombre_equipo[32]; // snmp.nombre (sysName = titulo del panel y de la pantalla)
static portMUX_TYPE s_lock_sensores = portMUX_INITIALIZER_UNLOCKED;

// Nombres del sitio hacia la pantalla UART: los manda tarea_climatizacion
// (la unica que escribe en ese UART) al arrancar, apenas se guardan desde
// la web (este flag) y cada NOMBRES_PANTALLA_CADA vueltas, por si la
// pantalla se reinicio y se los perdio (el protocolo no tiene ACK).
static volatile bool s_nombres_pantalla_pendientes = true;
#define NOMBRES_PANTALLA_CADA 15 // vueltas de 2 s = ~30 s

static void enviar_nombres_pantalla(void)
{
    config_sensores_t sens;
    char equipo[sizeof(s_nombre_equipo)];
    taskENTER_CRITICAL(&s_lock_sensores);
    sens = s_sensores;
    memcpy(equipo, s_nombre_equipo, sizeof(equipo));
    taskEXIT_CRITICAL(&s_lock_sensores);

    uart_pantalla_enviar_nombre(BRAINDLAB_NOMBRE_EQUIPO, 0, equipo);
    for (int i = 0; i < 4; i++) {
        uart_pantalla_enviar_nombre(BRAINDLAB_NOMBRE_AIRE, (uint8_t)i, sens.nombre_aire[i]);
    }
    for (int i = 0; i < CONFIG_NUM_SENSORES; i++) {
        uart_pantalla_enviar_nombre(BRAINDLAB_NOMBRE_SENSOR, (uint8_t)i, sens.nombre[i]);
    }
}

static void leer_sensores_instalados(bool instalado[CONFIG_NUM_SENSORES])
{
    taskENTER_CRITICAL(&s_lock_sensores);
    memcpy(instalado, s_sensores.instalado, sizeof(s_sensores.instalado));
    taskEXIT_CRITICAL(&s_lock_sensores);
}

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
                                const bool ntc_ok[4], uint8_t cantidad_aires,
                                const bool sensor_instalado[CONFIG_NUM_SENSORES])
{
    bool modo_manual = hay_algun_override_manual();

    char json[960];
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
             "\"ntc_ok\":[%s,%s,%s,%s],"
             "\"n_aires\":%u,"
             "\"sens_inst\":[%d,%d,%d,%d,%d,%d]}",
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
             ntc_ok[3] ? "true" : "false",
             (unsigned)cantidad_aires,
             sensor_instalado[0], sensor_instalado[1], sensor_instalado[2],
             sensor_instalado[3], sensor_instalado[4], sensor_instalado[5]);
    servidor_web_enviar_ws(json);
}

// Compara la semana actual (martes 00:00 a lunes 23:59) con la de la ultima
// rotacion guardada en config.json:
//   - primera vez (nunca roto): solo guarda la semana, no rota;
//   - semana nueva: el que descansa pasa al siguiente (N -> 1), se guarda
//     en config.json y se REINICIA el equipo para arrancar limpio con la
//     nueva reserva (no importa en que ciclo estaba);
//   - reloj atrasado (semana anterior a la guardada, p.ej. se corrigio la
//     hora): se guarda la semana sin rotar.
// Con 1 aire o rotar_reserva=false nunca rota (solo lleva la semana al dia).
static void revisar_rotacion_semanal(config_climatizacion_t *cfg, const struct tm *ahora, bool *fallo)
{
    char semana[11];
    climatizacion_semana_rotacion(ahora, semana);
    int orden = strcmp(semana, cfg->ultima_rotacion);
    if (orden == 0) {
        return; // misma semana
    }

    bool rotar = orden > 0 && cfg->ultima_rotacion[0] != '\0' && cfg->rotar_reserva && cfg->cantidad_aires > 1;
    uint8_t anterior = cfg->aire_alternar;
    if (rotar) {
        cfg->aire_alternar = climatizacion_siguiente_alternar(cfg->aire_alternar, cfg->cantidad_aires);
    }
    strncpy(cfg->ultima_rotacion, semana, sizeof(cfg->ultima_rotacion) - 1);
    cfg->ultima_rotacion[sizeof(cfg->ultima_rotacion) - 1] = '\0';

    // Se guarda sobre lo que hay en config.json (no sobre 'cfg', que es la
    // copia del arranque): si desde la web se cambiaron otros parametros sin
    // reiniciar, no se pisan. Solo se tocan los dos campos de la rotacion.
    config_climatizacion_t en_archivo;
    config_braindlab_cargar_climatizacion(&en_archivo);
    if (rotar) {
        en_archivo.aire_alternar = cfg->aire_alternar <= en_archivo.cantidad_aires ? cfg->aire_alternar : 1;
    }
    memcpy(en_archivo.ultima_rotacion, cfg->ultima_rotacion, sizeof(en_archivo.ultima_rotacion));

    if (config_braindlab_guardar_climatizacion(&en_archivo) != ESP_OK) {
        ESP_LOGE(TAG, "Rotacion semanal: no se pudo guardar config.json -- no se rota ni se reinicia");
        *fallo = true;
        return;
    }
    if (!rotar) {
        ESP_LOGI(TAG, "Rotacion semanal: semana %s registrada (sin rotar)", semana);
        return;
    }
    ESP_LOGW(TAG, "Rotacion semanal (semana %s): descansaba AA%u, ahora descansa AA%u -- reiniciando",
             semana, anterior, cfg->aire_alternar);
    char motivo[64];
    snprintf(motivo, sizeof(motivo), "rotacion semanal (ahora descansa AA%u)", cfg->aire_alternar);
    eventos_motivo_reinicio(motivo);
    vTaskDelay(pdMS_TO_TICKS(1000)); // que el log y el archivo terminen de salir
    esp_restart();
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
    climatizacion_iniciar(&cfg, &estado);
    bool rotacion_pendiente_fallo = false; // si no se pudo guardar, no reintentar cada 2 s
    clima_ciclo_t ciclo_previo = estado.ciclo;
    int ultimo_minuto_registrado = -1; // registro_sd: una fila por minuto
    // Cable de red para el registro de eventos: se empieza a seguir a los
    // 30 s del arranque (el link tarda en levantar, no es un evento) y un
    // cambio cuenta si se sostiene 2 vueltas seguidas.
    int red_registrada = -1; // -1 = todavia no se sigue, 0/1 = ultimo estado registrado
    int red_vueltas_distinto = 0;
    unsigned vueltas_nombres = 0;      // ver NOMBRES_PANTALLA_CADA

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
        // Solo cuentan los sensores instalados (Configuracion > Sensores): uno
        // no instalado no genera alarma ni participa del control.
        bool instalado[CONFIG_NUM_SENSORES];
        leer_sensores_instalados(instalado);
        bool hay_ntc_instalado = false;
        bool ntc_instalados_ok = true;
        for (int i = 0; i < 4; i++) {
            if (instalado[i]) {
                hay_ntc_instalado = true;
                ntc_instalados_ok = ntc_instalados_ok && ntc_ok[i];
            }
        }
        bool sensores_temp_ok = !hay_ntc_instalado || (adc_ok && ntc_instalados_ok);
        for (int i = 0; i < 4; i++) {
            s_ultimas_temperaturas_crudas[i] = temperaturas_crudas[i];
            temperaturas[i] = temperaturas_crudas[i] + s_calib_ntc[i];
        }
        bool validas[4];
        for (int i = 0; i < 4; i++) {
            validas[i] = ntc_ok[i] && instalado[i]; // instalado y leido bien en esta vuelta
        }
        climatizacion_actualizar(temperaturas, validas, &cfg, &estado);

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
        if (rtc_ok && !rtc_braindlab_detenido() && ahora.tm_year + 1900 >= 2024) {
            eventos_actualizar_hora(&ahora); // fecha de los eventos (y de los que esperaban hora)
        }
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
            static const char *const nombres_ciclo[] = { "Reposo", "Normal", "Alta temperatura", "Bypass" };
            float maxima = 0.0f;
            bool hay_maxima = false;
            for (int i = 0; i < 4; i++) {
                if (validas[i] && (!hay_maxima || temperaturas[i] > maxima)) {
                    maxima = temperaturas[i];
                    hay_maxima = true;
                }
            }
            eventos_registrar(estado.ciclo >= CLIMA_ALTA_TEMP ? EVENTO_AVISO : EVENTO_INFO, EVENTO_CONTROL,
                              "automatica", "Ciclo: %s -> %s (maxima %.1f C)",
                              nombres_ciclo[ciclo_previo & 3], nombres_ciclo[estado.ciclo & 3],
                              hay_maxima ? maxima : 0.0f);
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
                           rtc_ok, rtc_braindlab_detenido(), adc_ok, ntc_ok, cfg.cantidad_aires, instalado);

        // Estado de cada modulo en tiempo real, para la pantalla (bitmask,
        // ver BRAINDLAB_FALLA_* en protocolo_braindlab.h). La web recibe lo
        // mismo pero como campos sueltos en el JSON.
        uint8_t fallas = 0;
        if (!rtc_ok || rtc_braindlab_detenido())       fallas |= BRAINDLAB_FALLA_RTC;
        if (!adc_ok && hay_ntc_instalado)              fallas |= BRAINDLAB_FALLA_ADC;
        if (!gestor_ok && instalado[4])                fallas |= BRAINDLAB_FALLA_GESTOR;
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
        bool ntc_leido[4]; // sin ADS1115 ningun NTC tiene lectura valida
        for (int i = 0; i < 4; i++) {
            ntc_leido[i] = adc_ok && ntc_ok[i];
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
            (uint8_t)registro_sd_estado(), fallas,
            ntc_leido, instalado, cfg.cantidad_aires);

        if (s_nombres_pantalla_pendientes || ++vueltas_nombres >= NOMBRES_PANTALLA_CADA) {
            s_nombres_pantalla_pendientes = false;
            vueltas_nombres = 0;
            enviar_nombres_pantalla();
        }

        // Misma foto para el agente SNMP (GET) -- ademas detecta ahi los
        // eventos que generan traps (ver snmp_braindlab.c). No bloquea.
        snmp_braindlab_estado_t estado_snmp = {
            .ciclo = (uint8_t)estado.ciclo,
            .sensores_temp_ok = sensores_temp_ok,
            .gestor_ok = gestor_ok || !instalado[4], // no instalado: sin alarma de gabinete
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
            .energia_ok = energia.ok,
        };
        for (int i = 0; i < MODBUS_NUM_FASES && i < 3; i++) {
            estado_snmp.voltajes[i] = energia.voltajes[i];
            estado_snmp.corrientes[i] = energia.corrientes[i];
        }
        for (int i = 0; i < 4; i++) {
            estado_snmp.ntc_ok[i] = ntc_ok[i];
            estado_snmp.fails[i] = estado.fails[i];
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

        bool red_ok = red_eth_esta_conectado();
        if (red_ok) {
            ESP_LOGI(TAG, "Ethernet: conectado");
        } else {
            ESP_LOGW(TAG, "Ethernet: cable desconectado (sin link)");
        }
        if (red_registrada < 0) {
            if (esp_timer_get_time() >= 30LL * 1000000LL) {
                red_registrada = red_ok;
                if (!red_ok) {
                    eventos_registrar(EVENTO_AVISO, EVENTO_CONTROL, "sistema", "Sin cable de red al arrancar");
                }
            }
        } else if (red_ok != red_registrada) {
            if (++red_vueltas_distinto >= 2) {
                red_registrada = red_ok;
                red_vueltas_distinto = 0;
                eventos_registrar(red_ok ? EVENTO_OK : EVENTO_AVISO, EVENTO_CONTROL, "sistema",
                                  red_ok ? "Cable de red conectado" : "Cable de red desconectado");
            }
        } else {
            red_vueltas_distinto = 0;
        }

        // Rotacion semanal del aire que descansa (lunes -> martes 00:00).
        // Solo con un RTC confiable: si el reloj no responde, esta congelado
        // o tiene una fecha imposible, no se rota (mejor una semana de mas
        // con el mismo aire que rotar por una hora basura).
        if (rtc_ok && !rtc_braindlab_detenido() && ahora.tm_year + 1900 >= 2024 && !rotacion_pendiente_fallo) {
            revisar_rotacion_semanal(&cfg, &ahora, &rotacion_pendiente_fallo);
        }

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
                                         float temp_bypass, uint8_t fails_max_bypass, bool rotar_reserva,
                                         uint8_t aire_alternar)
{
    // Se parte de lo guardado para no perder ultima_rotacion (la web no la
    // manda) ni aire_alternar si no vino (0).
    config_climatizacion_t cfg;
    config_braindlab_cargar_climatizacion(&cfg);
    cfg.cantidad_aires = cantidad_aires;
    cfg.temp_min = temp_min;
    cfg.temp_max = temp_max;
    cfg.temp_at = temp_at;
    cfg.temp_bypass = temp_bypass;
    cfg.fails_max_bypass = fails_max_bypass;
    cfg.rotar_reserva = rotar_reserva;
    if (aire_alternar >= 1 && aire_alternar <= cantidad_aires) {
        cfg.aire_alternar = aire_alternar;
    } else if (cfg.aire_alternar > cantidad_aires) {
        cfg.aire_alternar = 1;
    }

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

// POST /configurar_sitio: nombre del equipo (= sysName de SNMP), nombres de
// los aires y nombres/instalado de los sensores 1..6 (ya validados por
// servidor_web.c). Se guarda y se aplica en caliente: el control, las
// alarmas, el panel y SNMP lo usan desde la proxima vuelta.
static esp_err_t on_configurar_sitio(const char *equipo, const char *const aires[4],
                                     const char *const nombres[CONFIG_NUM_SENSORES],
                                     const bool instalado[CONFIG_NUM_SENSORES])
{
    config_sensores_t nuevo;
    for (int i = 0; i < CONFIG_NUM_SENSORES; i++) {
        strlcpy(nuevo.nombre[i], nombres[i], sizeof(nuevo.nombre[i]));
        nuevo.instalado[i] = instalado[i];
    }
    nuevo.instalado[CONFIG_NUM_SENSORES - 1] = false; // sensor 6: sin hardware todavia
    for (int i = 0; i < 4; i++) {
        strlcpy(nuevo.nombre_aire[i], aires[i], sizeof(nuevo.nombre_aire[i]));
    }

    taskENTER_CRITICAL(&s_lock_sensores);
    s_sensores = nuevo;
    strlcpy(s_nombre_equipo, equipo, sizeof(s_nombre_equipo));
    taskEXIT_CRITICAL(&s_lock_sensores);
    s_nombres_pantalla_pendientes = true;
    snmp_braindlab_set_sensores(&nuevo);

    esp_err_t err = config_braindlab_guardar_sensores(&nuevo);

    // El nombre del equipo vive en la seccion "snmp" (sysName).
    config_snmp_t snmp;
    config_braindlab_cargar_snmp(&snmp);
    if (strcmp(snmp.nombre, equipo) != 0) {
        strlcpy(snmp.nombre, equipo, sizeof(snmp.nombre));
        bool reiniciar; // solo aplica a communities, aca no cambian
        if (snmp_braindlab_configurar(&snmp, &reiniciar) != ESP_OK) {
            err = ESP_FAIL;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "configurar_sitio: no se pudo guardar config.json (queda aplicado en RAM igual)");
    }
    return err;
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
    // Queda en memoria hasta que tarea_climatizacion lea el RTC (ahi se
    // fecha y se escribe en la SD).
    eventos_registrar_arranque(esp_app_get_description()->version);

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

    config_braindlab_cargar_sensores(&s_sensores);
    snmp_braindlab_set_sensores(&s_sensores);
    config_snmp_t snmp_inicial;
    config_braindlab_cargar_snmp(&snmp_inicial);
    strlcpy(s_nombre_equipo, snmp_inicial.nombre, sizeof(s_nombre_equipo));

    // 8192: el JSON del WS (960 B) + snprintf con floats + la foto SNMP + los
    // eventos (eventos_registrar() formatea en la pila de quien lo llama).
    xTaskCreate(tarea_climatizacion, "tarea_climatizacion", 8192, NULL, 5, NULL);

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
        .on_configurar_sitio = on_configurar_sitio,
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
    // tarea_climatizacion ya pudo dar su primera vuelta antes de que el UART
    // existiera: se vuelven a mandar los nombres en la proxima.
    s_nombres_pantalla_pendientes = true;

    // Red + servidor web al final: no bloquean el arranque de la
    // climatizacion (esa tarea ya esta corriendo arriba) ni dependen de
    // ella. Si el W5500 no responde, red_eth_init() devuelve error, no
    // aborta -- seguimos sin red antes que dejar el equipo sin climatizar.
    // Login de la web: contrasena en NVS (ver auth_web.h). Sin NVS la web
    // queda cerrada (rechaza todo) en vez de abierta sin contrasena.
    esp_err_t err_auth = auth_web_init();
    // auth_web_init() deja la NVS lista: ahi se guarda la version para
    // detectar una actualizacion de firmware.
    eventos_revisar_version(esp_app_get_description()->version);
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
            ESP_LOGI(TAG, "servidor_web_init() OK -- probar http://192.168.0.91/");
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

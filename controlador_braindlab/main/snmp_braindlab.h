#pragma once

// Agente SNMP v1/v2c del controlador (GET/GETNEXT/GETBULK/SET + traps), usando
// el agente que ya trae lwIP dentro de ESP-IDF (ver
// components/snmp_lwip/CMakeLists.txt). El arbol de OIDs esta documentado en
// ../BRAINDTIC-MIB.txt -- cargar ese archivo en el gestor (iReasoning,
// Zabbix, PRTG, snmptrapd...) para ver nombres en vez de numeros.
//
// Es un cliente mas del mismo mecanismo que usan la web y la pantalla UART:
//   - GET nunca toca hardware: devuelve la ultima foto que publico
//     tarea_climatizacion (snmp_braindlab_publicar_estado()).
//   - SET nunca acciona reles directamente: llama a los mismos callbacks que
//     POST /control_aire, /control_bypass, /calibrar_sensor, etc. (ver
//     app_main.c), y la proxima vuelta de tarea_climatizacion lo aplica.
// Si el agente SNMP falla o se cuelga, la climatizacion sigue igual.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Foto del estado que publica tarea_climatizacion a cada vuelta del loop
// (cada 2s). Mismos valores que van por WS/UART.
typedef struct {
    uint8_t ciclo;                  // clima_ciclo_t (0=reposo 1=normal 2=alta temp 3=bypass)
    float   temperaturas[4];        // T1..T4 calibradas, °C
    bool    sensores_temp_ok;       // false si sensores_temp_leer() fallo esta vuelta
    bool    gestor_ok;              // false si el AM2301A fallo esta vuelta
    float   temp_gestor;            // gabinete, calibrada, °C (ultimo valor bueno)
    float   humedad_gestor;         // gabinete, calibrada, % (ultimo valor bueno)
    bool    rtc_ok;
    bool    salida_aire[4];         // AA1..AA4 REALMENTE aplicadas (automatico + manual)
    bool    aire_manual[4];         // override manual vigente en AA1..AA4
    bool    alarma_at;              // OUT_AT aplicada
    bool    at_manual;
    bool    bypass_solicitado;      // BP_S aplicado
    bool    bypass_manual;
    bool    bypass_activo;          // BPS_STATUS (estado FISICO del bypass)
    bool    modo_manual;            // algun override manual vigente
    uint8_t indice_reserva;         // 0..N-1
    bool    bypass_auto_habilitado;
    uint8_t cantidad_aires;
    float   temp_min;
    float   temp_max;
    float   temp_at;
    float   temp_bypass;
    float   calibracion[6];         // constantes t1,t2,t3,t4,temp_gestor,humedad_gestor
} snmp_braindlab_estado_t;

// Mismas firmas que servidor_web_callbacks_t -- app_main.c pasa las mismas
// funciones on_control_* / on_calibrar_sensor que usa la web.
typedef struct {
    void (*on_control_aire)(uint8_t indice, bool encendido);
    void (*on_control_bypass)(bool solicitado);
    void (*on_control_at)(bool activa);
    void (*on_control_automatico)(void);
    void (*on_calibrar_sensor)(const char *sensor, float valor_referencia);
} snmp_braindlab_callbacks_t;

// Registrar ANTES de snmp_braindlab_init().
void snmp_braindlab_set_callbacks(snmp_braindlab_callbacks_t cbs);

// Carga la seccion "snmp" de config.json, registra las MIBs, arranca el
// agente (tarea "snmp_netconn", puerto UDP 161) y la tarea que envia los
// traps (puerto 162 del gestor). Llamar despues de red_eth_init() OK.
esp_err_t snmp_braindlab_init(void);

// Llamar desde tarea_climatizacion a cada vuelta. Guarda la foto para los GET
// y detecta los eventos que generan traps (alta temperatura, bypass, fallas
// de sensores, modo manual...). Es barata y nunca bloquea: si la cola de
// traps esta llena, el evento se descarta con un log (las alarmas vigentes
// igual se reenvian al volver el link, ver snmp_braindlab.c). Se puede
// llamar antes de snmp_braindlab_init() (o si la red no levanto): solo
// guarda la foto.
void snmp_braindlab_publicar_estado(const snmp_braindlab_estado_t *estado);

// Trap "calibracionModificada" -- llamar despues de aplicar una constante
// nueva (venga de la web, de la pantalla o de SNMP). indice_sensor: 0..3 =
// T1..T4, 4 = temp_gestor, 5 = humedad_gestor.
void snmp_braindlab_notificar_calibracion(uint8_t indice_sensor, float constante);

#ifdef __cplusplus
}
#endif

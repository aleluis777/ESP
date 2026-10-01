// Ver snmp_braindlab.h. Arbol de OIDs (detalle y tipos en ../BRAINDTIC-MIB.txt):
//
//   1.3.6.1.2.1.1                 system (MIB-2) -- sysDescr, sysObjectID, sysUpTime, sysName
//   <BRAINDTIC>          raiz de la empresa (ver OID_BRAINDTIC mas abajo)
//   <BRAINDTIC>.1        productos
//   <BRAINDTIC>.1.1      controlador_braindlab = <BASE> (este equipo)
//   <BRAINDTIC>.1.2...   reservados para otros equipos (labgeo, ...)
//   <BRAINDTIC>.2        reservado: objetos comunes a todos los equipos
//   <BASE>.1.x.0   entradas       RO  ciclo, T1-T4, temp maxima, gabinete, bypass fisico...
//   <BASE>.2.x.0   salidas        RW  AA1-AA4, alarma AT, bypass, modo manual
//   <BASE>.3.x.0   calibracion    RO constantes / RW valor de referencia
//   <BASE>.4.x.0   configuracion  RO umbrales vigentes de la climatizacion
//   <BASE>.5.x.0   traps          RW destinos de trap (IP + habilitado)
//   <BASE>.6.x.0   alarmas        RO alarmas vigentes (0/1) -- para polling
//   <BASE>.0.N     notificaciones (traps v2c, N = TRAP_* abajo)
//
// Temperaturas/humedad van en DECIMAS (INTEGER): 253 = 25.3 °C. SNMP no
// tiene tipo float.
//
// Hilos:
//   - tarea_climatizacion: snmp_braindlab_publicar_estado() -- guarda la
//     foto y detecta eventos (encola traps, nunca bloquea).
//   - "snmp_netconn" (tarea del agente de lwIP): corre los get/set de abajo.
//   - "snmp_traps" (propia): la UNICA que llama a las funciones de traps de
//     lwIP (snmp_send_trap*, snmp_trap_dst_*), porque no son reentrantes.
//     Todo lo demas le pide cosas por la cola s_cola.

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/ip_addr.h"
#include "lwip/apps/snmp.h"
#include "lwip/apps/snmp_core.h"
#include "lwip/apps/snmp_scalar.h"
#include "config_braindlab.h"
#include "climatizacion.h"
#include "red_eth.h"
#include "snmp_braindlab.h"

static const char *TAG = "SNMP_BRAINDLAB";

// Raiz de BraindTIC -- DEMO. 1.3.6.1.4.1.8072.9999.9999 es el
// "netSnmpPlaypen" de Net-SNMP: una rama reservada A PROPOSITO para
// pruebas, que no choca con ningun fabricante real. Cuando IANA asigne el
// Private Enterprise Number (PEN) de BraindTIC, cambiar SOLO estas dos
// lineas (y el ::= de braindtic en BRAINDTIC-MIB.txt):
//     #define OID_BRAINDTIC     1, 3, 6, 1, 4, 1, <PEN>
//     #define OID_BRAINDTIC_LEN 7
// El resto del arbol (.1.1 = este equipo, ramas, traps) no cambia.
#define OID_BRAINDTIC     1, 3, 6, 1, 4, 1, 8072, 9999, 9999
#define OID_BRAINDTIC_LEN 9

// <BRAINDTIC>.1 (productos) .1 (controlador_braindlab)
#define OID_BASE     OID_BRAINDTIC, 1, 1
#define OID_BASE_LEN (OID_BRAINDTIC_LEN + 2)

// SNMP_VERSION_2c esta definido en snmp_msg.h, que es un header privado del
// agente de lwIP (no esta en el include path publico).
#define VERSION_TRAP_V2C 1

// Idem: declarado en snmp_msg.h. Lo asigna la tarea "snmp_netconn" apenas
// crea su socket UDP; mandar un trap antes de eso usa un puntero NULL
// adentro de lwIP (crash). tarea_traps espera a que deje de ser NULL.
extern void *snmp_traps_handle;

// Misma cuenta que MIB2_COPY_SYSUPTIME_TO en components/snmp_lwip/snmp_lwip_uptime.h.
#define UPTIME_CENTESIMAS() ((u32_t)(esp_timer_get_time() / 10000))

static const u32_t s_oid_base[] = { OID_BASE };
static const struct snmp_obj_id s_enterprise_oid = { OID_BASE_LEN, { OID_BASE } };

// Ramas bajo <BASE> (ver arbol arriba).
enum {
    RAMA_ENTRADAS = 1,
    RAMA_SALIDAS = 2,
    RAMA_CALIBRACION = 3,
    RAMA_CONFIGURACION = 4,
    RAMA_TRAPS = 5,
    RAMA_ALARMAS = 6,
};

// ---------------------------------------------------------------------------
// Alarmas y traps
// ---------------------------------------------------------------------------

// Cada alarma es un bit de s_alarmas y una hoja <BASE>.6.(indice+1).0.
enum {
    ALARMA_ALTA_TEMP = 0,       // ciclo >= CLIMA_ALTA_TEMP
    ALARMA_BYPASS_ACTIVO,       // BPS_STATUS: bypass fisicamente activo
    ALARMA_BYPASS_FALLA,        // BP_S pedido != BPS_STATUS (el bypass no obedece)
    ALARMA_SENSORES_TEMP,       // sensores_temp_leer() falla (ADS1115 / NTC)
    ALARMA_SENSOR_GABINETE,     // AM2301A sin respuesta
    ALARMA_RTC,                 // DS1307 sin respuesta
    ALARMA_MODO_MANUAL,         // algun override manual vigente
    ALARMA_BYPASS_AUTO_DESHAB,  // se supero fails_max_bypass
    NUM_ALARMAS
};

// Numero de trap v2c: el snmpTrapOID sale como <BASE>.0.N.
enum {
    TRAP_ALTA_TEMP = 1,
    TRAP_ALTA_TEMP_NORMAL = 2,
    TRAP_BYPASS_ACTIVADO = 3,
    TRAP_BYPASS_DESACTIVADO = 4,
    TRAP_BYPASS_FALLA = 5,
    TRAP_BYPASS_FALLA_NORMAL = 6,
    TRAP_SENSORES_TEMP_FALLA = 7,
    TRAP_SENSORES_TEMP_NORMAL = 8,
    TRAP_SENSOR_GABINETE_FALLA = 9,
    TRAP_SENSOR_GABINETE_NORMAL = 10,
    TRAP_RTC_FALLA = 11,
    TRAP_RTC_NORMAL = 12,
    TRAP_MODO_MANUAL_ACTIVADO = 13,
    TRAP_MODO_MANUAL_DESACTIVADO = 14,
    TRAP_BYPASS_AUTO_DESHABILITADO = 15,
    TRAP_CALIBRACION_MODIFICADA = 16,
};

// Por alarma: trap al activarse / al normalizarse (0 = no hay), y cuantas
// vueltas seguidas (de 2s) tiene que sostenerse la condicion antes de
// cambiar de estado. Filtra los sensores que fallan de a ratos (el AM2301A
// falla una lectura cada tanto, es normal -- ver sensor_gestor.c) y el
// transitorio del bypass al arrancar (salidas_init() deja BP_S en HIGH hasta
// la primera vuelta). La alta temperatura no necesita filtro: la maquina de
// estados ya tiene histeresis (solo vuelve al bajar de temp_min).
static const struct {
    uint8_t trap_on;
    uint8_t trap_off;
    uint8_t vueltas_on;
    uint8_t vueltas_off;
} s_def_alarmas[NUM_ALARMAS] = {
    [ALARMA_ALTA_TEMP]          = { TRAP_ALTA_TEMP, TRAP_ALTA_TEMP_NORMAL, 1, 1 },
    [ALARMA_BYPASS_ACTIVO]      = { TRAP_BYPASS_ACTIVADO, TRAP_BYPASS_DESACTIVADO, 2, 2 },
    [ALARMA_BYPASS_FALLA]       = { TRAP_BYPASS_FALLA, TRAP_BYPASS_FALLA_NORMAL, 3, 3 },
    [ALARMA_SENSORES_TEMP]      = { TRAP_SENSORES_TEMP_FALLA, TRAP_SENSORES_TEMP_NORMAL, 3, 3 },
    [ALARMA_SENSOR_GABINETE]    = { TRAP_SENSOR_GABINETE_FALLA, TRAP_SENSOR_GABINETE_NORMAL, 5, 3 },
    [ALARMA_RTC]                = { TRAP_RTC_FALLA, TRAP_RTC_NORMAL, 3, 3 },
    [ALARMA_MODO_MANUAL]        = { TRAP_MODO_MANUAL_ACTIVADO, TRAP_MODO_MANUAL_DESACTIVADO, 1, 1 },
    [ALARMA_BYPASS_AUTO_DESHAB] = { TRAP_BYPASS_AUTO_DESHABILITADO, 0, 1, 1 },
};

#define MAX_VARBINDS_TRAP 3

typedef enum {
    MSG_TRAP,              // mandar el trap 'trap' con sus varbinds
    MSG_APLICAR_DESTINOS,  // s_cfg cambio (SET en la rama .5): reaplicar destinos
} tipo_msg_t;

typedef struct {
    uint8_t tipo;          // tipo_msg_t
    uint8_t trap;
    uint8_t num_vb;
    struct {
        uint8_t rama;
        uint8_t hoja;
        s32_t   valor;     // todos los varbinds de traps son INTEGER
    } vb[MAX_VARBINDS_TRAP];
} msg_t;

// ---------------------------------------------------------------------------
// Estado compartido
// ---------------------------------------------------------------------------

// Protege s_estado, s_alarmas y s_cfg. Spinlock (seccion critica corta, solo
// copias de structs) -- NUNCA hacer I/O (SPIFFS, red, logs) con el tomado.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static snmp_braindlab_estado_t s_estado;
static uint8_t s_alarmas;          // bit i = alarma i vigente (ya filtrada)
static config_snmp_t s_cfg;

static snmp_braindlab_callbacks_t s_cbs;
static QueueHandle_t volatile s_cola = NULL;

// lwIP guarda el PUNTERO a las communities (no las copia) -- tienen que
// vivir para siempre y no cambiar mientras el agente corre.
static char s_community_lectura[32];
static char s_community_escritura[32];
static char s_community_trap[32];

// Solo los usa snmp_braindlab_publicar_estado() (tarea_climatizacion).
static uint8_t s_contador_alarmas[NUM_ALARMAS];

static void leer_estado(snmp_braindlab_estado_t *estado, uint8_t *alarmas)
{
    taskENTER_CRITICAL(&s_lock);
    if (estado) {
        *estado = s_estado;
    }
    if (alarmas) {
        *alarmas = s_alarmas;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static void leer_cfg(config_snmp_t *cfg)
{
    taskENTER_CRITICAL(&s_lock);
    *cfg = s_cfg;
    taskEXIT_CRITICAL(&s_lock);
}

static s32_t a_decimas(float valor)
{
    return isfinite(valor) ? (s32_t)lroundf(valor * 10.0f) : 0;
}

static float temperatura_maxima(const snmp_braindlab_estado_t *e)
{
    float maxima = e->temperaturas[0];
    for (int i = 1; i < 4; i++) {
        if (e->temperaturas[i] > maxima) {
            maxima = e->temperaturas[i];
        }
    }
    return maxima;
}

static void encolar(const msg_t *msg)
{
    QueueHandle_t cola = s_cola;
    if (cola == NULL) {
        return; // snmp_braindlab_init() todavia no corrio (o la red no levanto)
    }
    if (xQueueSend(cola, msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Cola de traps llena, se descarta el trap %u", msg->trap);
    }
}

static void agregar_vb(msg_t *msg, uint8_t rama, uint8_t hoja, s32_t valor)
{
    if (msg->num_vb < MAX_VARBINDS_TRAP) {
        msg->vb[msg->num_vb].rama = rama;
        msg->vb[msg->num_vb].hoja = hoja;
        msg->vb[msg->num_vb].valor = valor;
        msg->num_vb++;
    }
}

// Arma el trap de activacion/normalizacion de 'alarma'. El primer varbind es
// siempre la propia alarma (<BASE>.6.x.0 = 0/1); despues, los datos que
// sirven para entender el evento sin tener que hacer un GET aparte.
// Devuelve false si esa transicion no tiene trap.
static bool armar_trap_alarma(int alarma, bool activa, const snmp_braindlab_estado_t *e, msg_t *msg)
{
    memset(msg, 0, sizeof(*msg));
    msg->tipo = MSG_TRAP;
    msg->trap = activa ? s_def_alarmas[alarma].trap_on : s_def_alarmas[alarma].trap_off;
    if (msg->trap == 0) {
        return false;
    }

    agregar_vb(msg, RAMA_ALARMAS, (uint8_t)(alarma + 1), activa ? 1 : 0);
    switch (alarma) {
    case ALARMA_ALTA_TEMP:
        agregar_vb(msg, RAMA_ENTRADAS, 1, e->ciclo);                            // cicloClimatizacion
        agregar_vb(msg, RAMA_ENTRADAS, 6, a_decimas(temperatura_maxima(e)));    // temperaturaMaxima
        break;
    case ALARMA_BYPASS_ACTIVO:
    case ALARMA_BYPASS_FALLA:
        agregar_vb(msg, RAMA_SALIDAS, 6, e->bypass_solicitado ? 1 : 0);         // bypassSolicitado
        agregar_vb(msg, RAMA_ENTRADAS, 9, e->bypass_activo ? 1 : 0);            // bypassEstadoFisico
        break;
    case ALARMA_SENSOR_GABINETE:
        agregar_vb(msg, RAMA_ENTRADAS, 7, a_decimas(e->temp_gestor));           // ultimo valor bueno
        break;
    default:
        break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// API publica (tarea_climatizacion / app_main)
// ---------------------------------------------------------------------------

void snmp_braindlab_set_callbacks(snmp_braindlab_callbacks_t cbs)
{
    s_cbs = cbs;
}

void snmp_braindlab_publicar_estado(const snmp_braindlab_estado_t *estado)
{
    bool condicion[NUM_ALARMAS] = {
        [ALARMA_ALTA_TEMP]          = estado->ciclo >= CLIMA_ALTA_TEMP,
        [ALARMA_BYPASS_ACTIVO]      = estado->bypass_activo,
        [ALARMA_BYPASS_FALLA]       = estado->bypass_solicitado != estado->bypass_activo,
        [ALARMA_SENSORES_TEMP]      = !estado->sensores_temp_ok,
        [ALARMA_SENSOR_GABINETE]    = !estado->gestor_ok,
        [ALARMA_RTC]                = !estado->rtc_ok,
        [ALARMA_MODO_MANUAL]        = estado->modo_manual,
        [ALARMA_BYPASS_AUTO_DESHAB] = !estado->bypass_auto_habilitado,
    };

    uint8_t alarmas;
    leer_estado(NULL, &alarmas);
    uint8_t cambiaron = 0;

    for (int i = 0; i < NUM_ALARMAS; i++) {
        bool vigente = (alarmas >> i) & 1;
        if (condicion[i] == vigente) {
            s_contador_alarmas[i] = 0;
            continue;
        }
        uint8_t necesarias = condicion[i] ? s_def_alarmas[i].vueltas_on : s_def_alarmas[i].vueltas_off;
        if (++s_contador_alarmas[i] >= necesarias) {
            s_contador_alarmas[i] = 0;
            alarmas ^= (uint8_t)(1u << i);
            cambiaron |= (uint8_t)(1u << i);
        }
    }

    taskENTER_CRITICAL(&s_lock);
    s_estado = *estado;
    s_alarmas = alarmas;
    taskEXIT_CRITICAL(&s_lock);

    for (int i = 0; i < NUM_ALARMAS; i++) {
        if ((cambiaron >> i) & 1) {
            bool activa = (alarmas >> i) & 1;
            ESP_LOGI(TAG, "Alarma %d %s", i + 1, activa ? "ACTIVADA" : "normalizada");
            msg_t msg;
            if (armar_trap_alarma(i, activa, estado, &msg)) {
                encolar(&msg);
            }
        }
    }
}

void snmp_braindlab_notificar_calibracion(uint8_t indice_sensor, float constante)
{
    if (indice_sensor > 5) {
        return;
    }
    msg_t msg = { .tipo = MSG_TRAP, .trap = TRAP_CALIBRACION_MODIFICADA };
    agregar_vb(&msg, RAMA_CALIBRACION, (uint8_t)(indice_sensor + 1), a_decimas(constante));
    encolar(&msg);
}

// ---------------------------------------------------------------------------
// MIB-2 system (1.3.6.1.2.1.1) -- lo minimo para que cualquier gestor
// reconozca el equipo (snmpwalk ... system, auto-descubrimiento, etc.).
// ---------------------------------------------------------------------------

static const char s_sys_descr[] = "controlador_braindlab - gestor de A/A (ESP32 + W5500)";
static const char s_sys_name[]  = "controlador_braindlab";

static s16_t system_get(const struct snmp_scalar_array_node_def *def, void *value)
{
    switch (def->oid) {
    case 1: // sysDescr
        memcpy(value, s_sys_descr, sizeof(s_sys_descr) - 1);
        return (s16_t)(sizeof(s_sys_descr) - 1);
    case 2: // sysObjectID
        memcpy(value, s_oid_base, sizeof(s_oid_base));
        return (s16_t)sizeof(s_oid_base);
    case 3: // sysUpTime
        *(u32_t *)value = UPTIME_CENTESIMAS();
        return sizeof(u32_t);
    case 5: // sysName
        memcpy(value, s_sys_name, sizeof(s_sys_name) - 1);
        return (s16_t)(sizeof(s_sys_name) - 1);
    default:
        return 0;
    }
}

static const struct snmp_scalar_array_node_def s_defs_system[] = {
    { 1, SNMP_ASN1_TYPE_OCTET_STRING, SNMP_NODE_INSTANCE_READ_ONLY },
    { 2, SNMP_ASN1_TYPE_OBJECT_ID,    SNMP_NODE_INSTANCE_READ_ONLY },
    { 3, SNMP_ASN1_TYPE_TIMETICKS,    SNMP_NODE_INSTANCE_READ_ONLY },
    { 5, SNMP_ASN1_TYPE_OCTET_STRING, SNMP_NODE_INSTANCE_READ_ONLY },
};

// ---------------------------------------------------------------------------
// <BASE>.1 entradas (RO)
// ---------------------------------------------------------------------------

static s16_t entradas_get(const struct snmp_scalar_array_node_def *def, void *value)
{
    snmp_braindlab_estado_t e;
    leer_estado(&e, NULL);
    s32_t *v = (s32_t *)value;

    switch (def->oid) {
    case 1:  *v = e.ciclo; break;                                   // cicloClimatizacion
    case 2:
    case 3:
    case 4:
    case 5:  *v = a_decimas(e.temperaturas[def->oid - 2]); break;   // temperatura1..4
    case 6:  *v = a_decimas(temperatura_maxima(&e)); break;         // temperaturaMaxima
    case 7:  *v = a_decimas(e.temp_gestor); break;                  // temperaturaGabinete
    case 8:  *v = a_decimas(e.humedad_gestor); break;               // humedadGabinete
    case 9:  *v = e.bypass_activo ? 1 : 0; break;                   // bypassEstadoFisico (BPS_STATUS)
    case 10: *v = e.indice_reserva + 1; break;                      // unidadReserva (1..N)
    case 11: *v = e.bypass_auto_habilitado ? 1 : 0; break;          // bypassAutoHabilitado
    case 12: *v = e.sensores_temp_ok ? 1 : 0; break;                // sensoresTempOk
    case 13: *v = e.gestor_ok ? 1 : 0; break;                       // sensorGabineteOk
    case 14: *v = e.rtc_ok ? 1 : 0; break;                          // rtcOk
    default: return 0;
    }
    return sizeof(s32_t);
}

static const struct snmp_scalar_array_node_def s_defs_entradas[] = {
    { 1,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 2,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 3,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 4,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 5,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 6,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 7,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 8,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 9,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 10, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 11, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 12, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 13, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 14, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
};

// ---------------------------------------------------------------------------
// <BASE>.2 salidas (RW) -- el SET usa los mismos callbacks que POST
// /control_aire, /control_at, /control_bypass y /control_automatico: el
// override dura hasta que la climatizacion cambie de ciclo o hasta
// modoManual=0, igual que desde la web.
// ---------------------------------------------------------------------------

static s16_t salidas_get(const struct snmp_scalar_array_node_def *def, void *value)
{
    snmp_braindlab_estado_t e;
    leer_estado(&e, NULL);
    s32_t *v = (s32_t *)value;

    switch (def->oid) {
    case 1:
    case 2:
    case 3:
    case 4:  *v = e.salida_aire[def->oid - 1] ? 1 : 0; break;       // aa1..aa4 (aplicada)
    case 5:  *v = e.alarma_at ? 1 : 0; break;                       // alarmaAltaTemp (OUT_AT)
    case 6:  *v = e.bypass_solicitado ? 1 : 0; break;               // bypassSolicitado (BP_S)
    case 7:  *v = e.modo_manual ? 1 : 0; break;                     // modoManual
    case 8:
    case 9:
    case 10:
    case 11: *v = e.aire_manual[def->oid - 8] ? 1 : 0; break;       // aa1Manual..aa4Manual
    case 12: *v = e.at_manual ? 1 : 0; break;                       // alarmaAltaTempManual
    case 13: *v = e.bypass_manual ? 1 : 0; break;                   // bypassManual
    default: return 0;
    }
    return sizeof(s32_t);
}

static snmp_err_t salidas_set_test(const struct snmp_scalar_array_node_def *def, u16_t len, void *value)
{
    LWIP_UNUSED_ARG(len);
    s32_t v = *(s32_t *)value;

    if (def->oid == 7) {
        // modoManual: solo se puede APAGAR (volver a automatico). Para
        // entrar en manual se escribe la salida puntual que se quiere forzar.
        if (v != 0) {
            return SNMP_ERR_WRONGVALUE;
        }
        return s_cbs.on_control_automatico ? SNMP_ERR_NOERROR : SNMP_ERR_RESOURCEUNAVAILABLE;
    }
    if (v != 0 && v != 1) {
        return SNMP_ERR_WRONGVALUE;
    }
    if ((def->oid <= 4 && !s_cbs.on_control_aire) ||
        (def->oid == 5 && !s_cbs.on_control_at) ||
        (def->oid == 6 && !s_cbs.on_control_bypass)) {
        return SNMP_ERR_RESOURCEUNAVAILABLE;
    }
    return SNMP_ERR_NOERROR;
}

static snmp_err_t salidas_set_value(const struct snmp_scalar_array_node_def *def, u16_t len, void *value)
{
    LWIP_UNUSED_ARG(len);
    bool v = *(s32_t *)value != 0;

    switch (def->oid) {
    case 1:
    case 2:
    case 3:
    case 4: s_cbs.on_control_aire((uint8_t)(def->oid - 1), v); break;
    case 5: s_cbs.on_control_at(v); break;
    case 6: s_cbs.on_control_bypass(v); break;
    case 7: s_cbs.on_control_automatico(); break;
    default: return SNMP_ERR_NOTWRITABLE;
    }
    ESP_LOGI(TAG, "SET salidas.%lu = %d", (unsigned long)def->oid, v);
    return SNMP_ERR_NOERROR;
}

static const struct snmp_scalar_array_node_def s_defs_salidas[] = {
    { 1,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
    { 2,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
    { 3,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
    { 4,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
    { 5,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
    { 6,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
    { 7,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
    { 8,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 9,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 10, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 11, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 12, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 13, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
};

// ---------------------------------------------------------------------------
// <BASE>.3 calibracion
//   .1-.6   constante vigente (RO, decimas) de t1,t2,t3,t4,temp_gestor,humedad_gestor
//   .7-.12  referencia (RW, decimas): GET = lectura calibrada actual; SET =
//           "el valor real es este" -> mismo calculo que POST /calibrar_sensor
//           (constante = referencia - crudo), se guarda en config.json.
// ---------------------------------------------------------------------------

static const char *const s_nombres_sensor[6] = { "t1", "t2", "t3", "t4", "temp_gestor", "humedad_gestor" };

static s16_t calibracion_get(const struct snmp_scalar_array_node_def *def, void *value)
{
    snmp_braindlab_estado_t e;
    leer_estado(&e, NULL);
    s32_t *v = (s32_t *)value;

    if (def->oid >= 1 && def->oid <= 6) {
        *v = a_decimas(e.calibracion[def->oid - 1]);
    } else if (def->oid >= 7 && def->oid <= 10) {
        *v = a_decimas(e.temperaturas[def->oid - 7]);
    } else if (def->oid == 11) {
        *v = a_decimas(e.temp_gestor);
    } else if (def->oid == 12) {
        *v = a_decimas(e.humedad_gestor);
    } else {
        return 0;
    }
    return sizeof(s32_t);
}

static snmp_err_t calibracion_set_test(const struct snmp_scalar_array_node_def *def, u16_t len, void *value)
{
    LWIP_UNUSED_ARG(len);
    s32_t v = *(s32_t *)value;

    if (!s_cbs.on_calibrar_sensor) {
        return SNMP_ERR_RESOURCEUNAVAILABLE;
    }
    if (def->oid == 12) {
        return (v >= 0 && v <= 1000) ? SNMP_ERR_NOERROR : SNMP_ERR_WRONGVALUE;      // 0..100 %
    }
    return (v >= -400 && v <= 1250) ? SNMP_ERR_NOERROR : SNMP_ERR_WRONGVALUE;       // -40..125 °C
}

static snmp_err_t calibracion_set_value(const struct snmp_scalar_array_node_def *def, u16_t len, void *value)
{
    LWIP_UNUSED_ARG(len);
    if (def->oid < 7 || def->oid > 12) {
        return SNMP_ERR_NOTWRITABLE;
    }
    float referencia = (float)*(s32_t *)value / 10.0f;
    // on_calibrar_sensor() (app_main.c) guarda, aplica y dispara el trap
    // calibracionModificada -- igual que si viniera de la web.
    s_cbs.on_calibrar_sensor(s_nombres_sensor[def->oid - 7], referencia);
    return SNMP_ERR_NOERROR;
}

static const struct snmp_scalar_array_node_def s_defs_calibracion[] = {
    { 1,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 2,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 3,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 4,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 5,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 6,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 7,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
    { 8,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
    { 9,  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
    { 10, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
    { 11, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
    { 12, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
};

// ---------------------------------------------------------------------------
// <BASE>.4 configuracion (RO) -- los umbrales que tarea_climatizacion tiene
// EN USO (cargados al arrancar). Un POST /configurar_climatizacion guarda
// valores nuevos en config.json pero recien se ven aca despues de reiniciar,
// porque recien ahi se aplican.
// ---------------------------------------------------------------------------

static s16_t configuracion_get(const struct snmp_scalar_array_node_def *def, void *value)
{
    snmp_braindlab_estado_t e;
    leer_estado(&e, NULL);
    s32_t *v = (s32_t *)value;

    switch (def->oid) {
    case 1: *v = e.cantidad_aires; break;
    case 2: *v = a_decimas(e.temp_min); break;
    case 3: *v = a_decimas(e.temp_max); break;
    case 4: *v = a_decimas(e.temp_at); break;
    case 5: *v = a_decimas(e.temp_bypass); break;
    default: return 0;
    }
    return sizeof(s32_t);
}

static const struct snmp_scalar_array_node_def s_defs_configuracion[] = {
    { 1, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 2, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 3, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 4, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 5, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
};

// ---------------------------------------------------------------------------
// <BASE>.5 traps (RW): .1 trapDestino1 (IpAddress) .2 trapHabilitado1
//                      .3 trapDestino2 (IpAddress) .4 trapHabilitado2
// El SET se guarda en config.json y se aplica en caliente (via tarea_traps).
// ---------------------------------------------------------------------------

static s16_t traps_get(const struct snmp_scalar_array_node_def *def, void *value)
{
    config_snmp_t cfg;
    leer_cfg(&cfg);
    int destino = (int)(def->oid - 1) / 2;
    if (destino >= CONFIG_SNMP_NUM_DESTINOS) {
        return 0;
    }

    if (def->oid % 2 == 1) {
        ip4_addr_t ip;
        if (!ip4addr_aton(cfg.trap_ip[destino], &ip)) {
            ip4_addr_set_zero(&ip);
        }
        u32_t crudo = ip4_addr_get_u32(&ip); // ya en orden de red
        memcpy(value, &crudo, 4);
        return 4;
    }
    *(s32_t *)value = cfg.trap_habilitado[destino] ? 1 : 0;
    return sizeof(s32_t);
}

static snmp_err_t traps_set_test(const struct snmp_scalar_array_node_def *def, u16_t len, void *value)
{
    if (def->oid % 2 == 1) {
        return (len == 4) ? SNMP_ERR_NOERROR : SNMP_ERR_WRONGLENGTH;
    }
    s32_t v = *(s32_t *)value;
    return (v == 0 || v == 1) ? SNMP_ERR_NOERROR : SNMP_ERR_WRONGVALUE;
}

static snmp_err_t traps_set_value(const struct snmp_scalar_array_node_def *def, u16_t len, void *value)
{
    LWIP_UNUSED_ARG(len);
    int destino = (int)(def->oid - 1) / 2;
    if (destino >= CONFIG_SNMP_NUM_DESTINOS) {
        return SNMP_ERR_NOTWRITABLE;
    }

    config_snmp_t cfg;
    leer_cfg(&cfg);
    if (def->oid % 2 == 1) {
        ip4_addr_t ip;
        u32_t crudo;
        memcpy(&crudo, value, 4);
        ip4_addr_set_u32(&ip, crudo);
        ip4addr_ntoa_r(&ip, cfg.trap_ip[destino], sizeof(cfg.trap_ip[destino]));
    } else {
        cfg.trap_habilitado[destino] = *(s32_t *)value != 0;
    }

    taskENTER_CRITICAL(&s_lock);
    s_cfg = cfg;
    taskEXIT_CRITICAL(&s_lock);

    msg_t msg = { .tipo = MSG_APLICAR_DESTINOS };
    encolar(&msg);

    // Fuera del spinlock (SPIFFS). Si falla, queda aplicado en RAM igual
    // hasta el proximo reinicio -- mismo criterio que on_calibrar_sensor().
    if (config_braindlab_guardar_snmp(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo guardar la config SNMP (queda aplicada en RAM igual)");
    }
    ESP_LOGI(TAG, "Destino de trap %d: %s (%s)", destino + 1, cfg.trap_ip[destino],
             cfg.trap_habilitado[destino] ? "habilitado" : "deshabilitado");
    return SNMP_ERR_NOERROR;
}

static const struct snmp_scalar_array_node_def s_defs_traps[] = {
    { 1, SNMP_ASN1_TYPE_IPADDR,  SNMP_NODE_INSTANCE_READ_WRITE },
    { 2, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
    { 3, SNMP_ASN1_TYPE_IPADDR,  SNMP_NODE_INSTANCE_READ_WRITE },
    { 4, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
};

// ---------------------------------------------------------------------------
// <BASE>.6 alarmas (RO) -- el trap es UDP sin confirmacion: si el gestor
// estaba apagado o el cable desconectado, se pierde. Estas hojas permiten
// consultarlas por polling en cualquier momento.
// ---------------------------------------------------------------------------

static s16_t alarmas_get(const struct snmp_scalar_array_node_def *def, void *value)
{
    uint8_t alarmas;
    leer_estado(NULL, &alarmas);
    if (def->oid < 1 || def->oid > NUM_ALARMAS) {
        return 0;
    }
    *(s32_t *)value = (alarmas >> (def->oid - 1)) & 1;
    return sizeof(s32_t);
}

static const struct snmp_scalar_array_node_def s_defs_alarmas[] = {
    { 1, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 2, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 3, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 4, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 5, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 6, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 7, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { 8, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
};

// ---------------------------------------------------------------------------
// Arbol y MIBs
// ---------------------------------------------------------------------------

static const struct snmp_scalar_array_node s_nodo_system =
    SNMP_SCALAR_CREATE_ARRAY_NODE(1, s_defs_system, system_get, NULL, NULL);
static const struct snmp_scalar_array_node s_nodo_entradas =
    SNMP_SCALAR_CREATE_ARRAY_NODE(RAMA_ENTRADAS, s_defs_entradas, entradas_get, NULL, NULL);
static const struct snmp_scalar_array_node s_nodo_salidas =
    SNMP_SCALAR_CREATE_ARRAY_NODE(RAMA_SALIDAS, s_defs_salidas, salidas_get, salidas_set_test, salidas_set_value);
static const struct snmp_scalar_array_node s_nodo_calibracion =
    SNMP_SCALAR_CREATE_ARRAY_NODE(RAMA_CALIBRACION, s_defs_calibracion, calibracion_get, calibracion_set_test,
                                  calibracion_set_value);
static const struct snmp_scalar_array_node s_nodo_configuracion =
    SNMP_SCALAR_CREATE_ARRAY_NODE(RAMA_CONFIGURACION, s_defs_configuracion, configuracion_get, NULL, NULL);
static const struct snmp_scalar_array_node s_nodo_traps =
    SNMP_SCALAR_CREATE_ARRAY_NODE(RAMA_TRAPS, s_defs_traps, traps_get, traps_set_test, traps_set_value);
static const struct snmp_scalar_array_node s_nodo_alarmas =
    SNMP_SCALAR_CREATE_ARRAY_NODE(RAMA_ALARMAS, s_defs_alarmas, alarmas_get, NULL, NULL);

// Mismo patron que la MIB-2 de lwIP (snmp_mib2.c): base {1,3,6,1,2,1} y
// raiz con el oid del ultimo elemento; los hijos cuelgan de la base.
static const u32_t s_oid_mib2[] = { 1, 3, 6, 1, 2, 1 };
static const struct snmp_node *const s_hijos_mib2[] = { &s_nodo_system.node.node };
static const struct snmp_tree_node s_raiz_mib2 = SNMP_CREATE_TREE_NODE(1, s_hijos_mib2);
static const struct snmp_mib s_mib_mib2 = SNMP_MIB_CREATE(s_oid_mib2, &s_raiz_mib2.node);

// Hijos en orden creciente de OID (lo exige el GETNEXT/walk de lwIP).
static const struct snmp_node *const s_hijos_braindlab[] = {
    &s_nodo_entradas.node.node,
    &s_nodo_salidas.node.node,
    &s_nodo_calibracion.node.node,
    &s_nodo_configuracion.node.node,
    &s_nodo_traps.node.node,
    &s_nodo_alarmas.node.node,
};
static const struct snmp_tree_node s_raiz_braindlab = SNMP_CREATE_TREE_NODE(1, s_hijos_braindlab);
static const struct snmp_mib s_mib_braindlab = SNMP_MIB_CREATE(s_oid_base, &s_raiz_braindlab.node);

static const struct snmp_mib *s_mibs[] = { &s_mib_mib2, &s_mib_braindlab };

// ---------------------------------------------------------------------------
// Tarea de traps
// ---------------------------------------------------------------------------

static void aplicar_destinos(void)
{
    config_snmp_t cfg;
    leer_cfg(&cfg);
    for (int i = 0; i < CONFIG_SNMP_NUM_DESTINOS; i++) {
        ip_addr_t ip;
        if (!ipaddr_aton(cfg.trap_ip[i], &ip)) {
            ESP_LOGW(TAG, "trap%d_ip \"%s\" invalida, destino deshabilitado", i + 1, cfg.trap_ip[i]);
            ip_addr_set_zero_ip4(&ip);
        }
        snmp_trap_dst_ip_set((u8_t)i, &ip);
        snmp_trap_dst_enable((u8_t)i, cfg.trap_habilitado[i] ? 1 : 0);
    }
}

static void enviar_trap(const msg_t *msg)
{
    struct snmp_varbind vbs[MAX_VARBINDS_TRAP];
    s32_t valores[MAX_VARBINDS_TRAP];
    memset(vbs, 0, sizeof(vbs));

    for (int i = 0; i < msg->num_vb; i++) {
        memcpy(vbs[i].oid.id, s_oid_base, sizeof(s_oid_base));
        vbs[i].oid.id[OID_BASE_LEN] = msg->vb[i].rama;
        vbs[i].oid.id[OID_BASE_LEN + 1] = msg->vb[i].hoja;
        vbs[i].oid.id[OID_BASE_LEN + 2] = 0; // instancia escalar ".0"
        vbs[i].oid.len = OID_BASE_LEN + 3;
        vbs[i].type = SNMP_ASN1_TYPE_INTEGER;
        valores[i] = msg->vb[i].valor;
        vbs[i].value = &valores[i];
        vbs[i].value_len = sizeof(s32_t);
        if (i > 0) {
            vbs[i - 1].next = &vbs[i];
            vbs[i].prev = &vbs[i - 1];
        }
    }

    err_t err = snmp_send_trap_specific(msg->trap, msg->num_vb ? vbs : NULL);
    if (err == ERR_OK) {
        ESP_LOGI(TAG, "Trap %u enviado", msg->trap);
    } else {
        ESP_LOGW(TAG, "Trap %u no se pudo enviar (err=%d)", msg->trap, err);
    }
}

// Al (re)conectar: manda de nuevo el trap de activacion de cada alarma que
// siga vigente -- los que se generaron con el cable desconectado se
// perdieron, y asi el gestor queda al dia sin esperar al proximo polling.
static void reenviar_alarmas_vigentes(void)
{
    snmp_braindlab_estado_t e;
    uint8_t alarmas;
    leer_estado(&e, &alarmas);
    for (int i = 0; i < NUM_ALARMAS; i++) {
        msg_t msg;
        if (((alarmas >> i) & 1) && armar_trap_alarma(i, true, &e, &msg)) {
            enviar_trap(&msg);
        }
    }
}

static void tarea_traps(void *arg)
{
    while (snmp_traps_handle == NULL) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    aplicar_destinos();

    bool link_previo = false;
    bool primer_link = true;

    while (1) {
        msg_t msg;
        bool hay_msg = xQueueReceive(s_cola, &msg, pdMS_TO_TICKS(1000)) == pdTRUE;

        bool link = red_eth_esta_conectado();
        if (link && !link_previo) {
            // Margen para que el switch habilite el puerto y resuelva ARP;
            // un trap mandado en el mismo instante del link-up suele perderse.
            vTaskDelay(pdMS_TO_TICKS(2000));
            if (primer_link) {
                snmp_coldstart_trap();
                primer_link = false;
                ESP_LOGI(TAG, "Trap coldStart enviado");
            } else {
                // NO usar snmp_send_trap_generic(SNMP_GENTRAP_LINKUP): en v2c
                // lwIP arma el snmpTrapOID con specific_trap (0) en vez de
                // generic_trap y el trap sale como coldStart. Pasando el
                // generico tambien como "specific" sale bien (.1.3.6.1.6.3.1.1.5.4).
                snmp_send_trap(NULL, SNMP_GENTRAP_LINKUP, SNMP_GENTRAP_LINKUP, NULL);
                ESP_LOGI(TAG, "Trap linkUp enviado");
            }
            reenviar_alarmas_vigentes();
        }
        link_previo = link;

        if (!hay_msg) {
            continue;
        }
        if (msg.tipo == MSG_APLICAR_DESTINOS) {
            aplicar_destinos();
        } else if (!link) {
            ESP_LOGW(TAG, "Sin link Ethernet, trap %u descartado (si la alarma sigue vigente se reenvia al volver)",
                     msg.trap);
        } else {
            enviar_trap(&msg);
        }
    }
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

esp_err_t snmp_braindlab_init(void)
{
    if (s_cola != NULL) {
        return ESP_OK;
    }

    config_snmp_t cfg;
    config_braindlab_cargar_snmp(&cfg);
    taskENTER_CRITICAL(&s_lock);
    s_cfg = cfg;
    taskEXIT_CRITICAL(&s_lock);

    strlcpy(s_community_lectura, cfg.community_lectura, sizeof(s_community_lectura));
    strlcpy(s_community_escritura, cfg.community_escritura, sizeof(s_community_escritura));
    strlcpy(s_community_trap, cfg.community_trap, sizeof(s_community_trap));

    QueueHandle_t cola = xQueueCreate(16, sizeof(msg_t));
    if (cola == NULL) {
        return ESP_ERR_NO_MEM;
    }

    snmp_set_community(s_community_lectura);
    snmp_set_community_write(s_community_escritura);
    snmp_set_community_trap(s_community_trap);
    snmp_set_device_enterprise_oid(&s_enterprise_oid);
    snmp_set_default_trap_version(VERSION_TRAP_V2C);
    snmp_set_mibs(s_mibs, (u8_t)LWIP_ARRAYSIZE(s_mibs));
    snmp_init();

    s_cola = cola;
    if (xTaskCreate(tarea_traps, "snmp_traps", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear la tarea de traps -- GET/SET funcionan, traps NO");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Agente SNMP v1/v2c en UDP 161 (traps v2c a UDP 162)");
    return ESP_OK;
}

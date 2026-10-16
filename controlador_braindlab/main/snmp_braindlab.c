// Ver snmp_braindlab.h. Arbol de OIDs (detalle y tipos en ../BRAINDTIC-MIB.txt):
//
//   1.3.6.1.2.1.1                 system (MIB-2) -- sysDescr, sysObjectID, sysUpTime, sysName
//   <BRAINDTIC>          raiz de la empresa (ver OID_BRAINDTIC mas abajo)
//   <BRAINDTIC>.1        productos
//   <BRAINDTIC>.1.1      controlador_braindlab = <BASE> (este equipo)
//   <BRAINDTIC>.1.2...   reservados para otros equipos (labgeo, ...)
//   <BRAINDTIC>.2        reservado: objetos comunes a todos los equipos
//   <BASE>.1.1.c.f   control   tabla, fila f = AA1..AA4 (columna c, ver CTRL_*)
//   <BASE>.2.1.c.f   sensores  tabla, fila f = 1..6, temperatura + humedad (SENS_*)
//   <BASE>.3.1.c.f   energia   tabla, fila f = fases R,S,T del medidor Modbus (ENER_*)
//   <BASE>.4.1.x.0   sistema/estado         RO  ciclo, temp maxima, reserva, bypass fisico...
//   <BASE>.4.2.x.0   sistema/mando          RW  modo manual, bypass, alarma AT
//   <BASE>.4.3.x.0   sistema/configuracion  RO  umbrales vigentes de la climatizacion
//   <BASE>.4.4.x.0   sistema/traps          RW  destinos de trap (IP + habilitado)
//   <BASE>.5.x.0     alarmas   RO  alarmas vigentes (0/1) -- para polling
//   <BASE>.0.N       notificaciones (traps v2c, N = TRAP_* abajo)
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
#include "lwip/apps/snmp_table.h"
#include "config_braindlab.h"
#include "climatizacion.h"
#include "red_eth.h"
#include "eventos.h"
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
    RAMA_CONTROL = 1,
    RAMA_SENSORES = 2,
    RAMA_ENERGIA = 3,
    RAMA_SISTEMA = 4,
    RAMA_ALARMAS = 5,
};

// Sub-ramas de <BASE>.4 sistema.
enum {
    SIS_ESTADO = 1,
    SIS_MANDO = 2,
    SIS_CONFIGURACION = 3,
    SIS_TRAPS = 4,
};

// Hojas de <BASE>.4.1 estado.
enum {
    EST_CICLO = 1,
    EST_TEMP_MAXIMA = 2,
    EST_UNIDAD_RESERVA = 3,
    EST_BYPASS_FISICO = 4,
    EST_BYPASS_AUTO_HABILITADO = 5,
    EST_RTC_OK = 6,
};

// Hojas de <BASE>.4.2 mando.
enum {
    MANDO_MODO_MANUAL = 1,
    MANDO_BYPASS = 2,
    MANDO_BYPASS_MANUAL = 3,
    MANDO_ALARMA_AT = 4,
    MANDO_ALARMA_AT_MANUAL = 5,
};

// Columnas de las tablas (la 1 es el indice, not-accessible en la MIB).
enum {
    CTRL_NOMBRE = 2,
    CTRL_INSTALADO = 3,
    CTRL_ESTADO = 4,      // RW: forzar encendido/apagado (override manual)
    CTRL_MODO = 5,
    CTRL_RESERVA = 6,
    CTRL_FALLAS = 7,
};
enum {
    SENS_NOMBRE = 2,
    SENS_TIPO = 3,
    SENS_ESTADO = 4,
    SENS_TEMPERATURA = 5,
    SENS_HUMEDAD = 6,
    SENS_CALIB_TEMP = 7,
    SENS_CALIB_HUM = 8,
    SENS_REF_TEMP = 9,    // RW: calibrar contra un valor de referencia
    SENS_REF_HUM = 10,    // RW
};
enum {
    ENER_NOMBRE = 2,
    ENER_ESTADO = 3,
    ENER_VOLTAJE = 4,
    ENER_CORRIENTE = 5,
};

#define FILAS_CONTROL  4
#define FILAS_SENSORES 6
#define FILAS_ENERGIA  3

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
    ALARMA_MEDIDOR_ENERGIA,     // medidor Modbus sin lectura vigente
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
    TRAP_PRUEBA = 17,               // boton "Enviar trap de prueba" de la web
    TRAP_MEDIDOR_ENERGIA_FALLA = 18,
    TRAP_MEDIDOR_ENERGIA_NORMAL = 19,
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
    [ALARMA_MEDIDOR_ENERGIA]    = { TRAP_MEDIDOR_ENERGIA_FALLA, TRAP_MEDIDOR_ENERGIA_NORMAL, 3, 3 },
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
        uint8_t sufijo[4]; // OID debajo de <BASE> (escalar: termina en .0)
        uint8_t len;
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
static uint16_t s_alarmas;         // bit i = alarma i vigente (ya filtrada)
static config_snmp_t s_cfg;
static config_sensores_t s_sensores; // ver snmp_braindlab_set_sensores()

static snmp_braindlab_callbacks_t s_cbs;
static QueueHandle_t volatile s_cola = NULL;

// lwIP guarda el PUNTERO a las communities (no las copia) -- tienen que
// vivir para siempre y no cambiar mientras el agente corre.
static char s_community_lectura[32];
static char s_community_escritura[32];
static char s_community_trap[32];

// Solo los usa snmp_braindlab_publicar_estado() (tarea_climatizacion).
static uint8_t s_contador_alarmas[NUM_ALARMAS];

static void leer_estado(snmp_braindlab_estado_t *estado, uint16_t *alarmas)
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

// Todo SET pasa por aca (set_test de cada nodo): con la escritura apagada
// (default de fabrica) responde noAccess aunque la community sea la correcta.
static bool escritura_permitida(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool permitida = s_cfg.escritura_habilitada;
    taskEXIT_CRITICAL(&s_lock);
    return permitida;
}

static s32_t a_decimas(float valor)
{
    return isfinite(valor) ? (s32_t)lroundf(valor * 10.0f) : 0;
}

// La mayor de T1..T4 entre los sensores instalados (0 si no hay ninguno).
static float temperatura_maxima(const snmp_braindlab_estado_t *e)
{
    bool instalado[CONFIG_NUM_SENSORES];
    taskENTER_CRITICAL(&s_lock);
    memcpy(instalado, s_sensores.instalado, sizeof(instalado));
    taskEXIT_CRITICAL(&s_lock);

    bool hay = false;
    float maxima = 0.0f;
    for (int i = 0; i < 4; i++) {
        if (instalado[i] && (!hay || e->temperaturas[i] > maxima)) {
            maxima = e->temperaturas[i];
            hay = true;
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

static void agregar_vb(msg_t *msg, const uint8_t *sufijo, uint8_t len, s32_t valor)
{
    if (msg->num_vb < MAX_VARBINDS_TRAP && len <= sizeof(msg->vb[0].sufijo)) {
        memcpy(msg->vb[msg->num_vb].sufijo, sufijo, len);
        msg->vb[msg->num_vb].len = len;
        msg->vb[msg->num_vb].valor = valor;
        msg->num_vb++;
    }
}

// Varbind de un escalar (<BASE>.5.n.0 / <BASE>.4.sub.hoja.0) o de una celda
// de tabla (<BASE>.tabla.1.columna.fila).
#define VB_ALARMA(msg, n, v) \
    agregar_vb((msg), (const uint8_t[]){ RAMA_ALARMAS, (n), 0 }, 3, (v))
#define VB_SISTEMA(msg, sub, hoja, v) \
    agregar_vb((msg), (const uint8_t[]){ RAMA_SISTEMA, (sub), (hoja), 0 }, 4, (v))
#define VB_CELDA(msg, tabla, col, fila, v) \
    agregar_vb((msg), (const uint8_t[]){ (tabla), 1, (col), (fila) }, 4, (v))

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

    VB_ALARMA(msg, (uint8_t)(alarma + 1), activa ? 1 : 0);
    switch (alarma) {
    case ALARMA_ALTA_TEMP:
        VB_SISTEMA(msg, SIS_ESTADO, EST_CICLO, e->ciclo);
        VB_SISTEMA(msg, SIS_ESTADO, EST_TEMP_MAXIMA, a_decimas(temperatura_maxima(e)));
        break;
    case ALARMA_BYPASS_ACTIVO:
    case ALARMA_BYPASS_FALLA:
        VB_SISTEMA(msg, SIS_MANDO, MANDO_BYPASS, e->bypass_solicitado ? 1 : 0);
        VB_SISTEMA(msg, SIS_ESTADO, EST_BYPASS_FISICO, e->bypass_activo ? 1 : 0);
        break;
    case ALARMA_SENSOR_GABINETE:
        VB_CELDA(msg, RAMA_SENSORES, SENS_TEMPERATURA, 5, a_decimas(e->temp_gestor)); // ultimo valor bueno
        break;
    default:
        break;
    }
    return true;
}

// Evento del registro (eventos.h) para la transicion de 'alarma': mismo
// filtro de vueltas que los traps, asi el registro y el NOC ven lo mismo.
// Corre en tarea_climatizacion.
static void registrar_evento_alarma(int alarma, bool activa, const snmp_braindlab_estado_t *e)
{
    static bool s_falla_bypass_registrada = false;

    switch (alarma) {
    case ALARMA_ALTA_TEMP:
        if (activa) {
            eventos_registrar(EVENTO_CRITICO, EVENTO_ALARMA, "automatica", "Alta temperatura: maxima %.1f C",
                              temperatura_maxima(e));
        } else {
            eventos_registrar(EVENTO_OK, EVENTO_ALARMA, "automatica", "Alta temperatura normalizada (maxima %.1f C)",
                              temperatura_maxima(e));
        }
        break;
    case ALARMA_BYPASS_ACTIVO:
        if (activa) {
            // Sin pedido del firmware = lo activo el contacto externo (BP_H).
            const char *origen = !e->bypass_solicitado ? "contacto" : e->bypass_manual ? "manual" : "automatica";
            eventos_registrar(EVENTO_AVISO, EVENTO_ALARMA, origen, "%s",
                              !e->bypass_solicitado ? "Bypass fisico activo: activado por el contacto externo"
                                                    : "Bypass fisico activo: el A/A queda conectado directo");
        } else {
            eventos_registrar(EVENTO_OK, EVENTO_ALARMA, "sistema", "Bypass fisico desactivado");
        }
        break;
    case ALARMA_BYPASS_FALLA:
        // Bypass activo sin pedido = contacto externo, ya registrado arriba.
        // Solo es falla si se pidio y la placa no lo confirma.
        if (activa && e->bypass_solicitado && !e->bypass_activo) {
            eventos_registrar(EVENTO_CRITICO, EVENTO_ALARMA, "sistema",
                              "Falla de bypass: se pidio y la placa no lo confirma (BPS_STATUS)");
            s_falla_bypass_registrada = true;
        } else if (!activa && s_falla_bypass_registrada) {
            eventos_registrar(EVENTO_OK, EVENTO_ALARMA, "sistema", "Falla de bypass normalizada");
            s_falla_bypass_registrada = false;
        }
        break;
    case ALARMA_SENSORES_TEMP:
    case ALARMA_SENSOR_GABINETE: {
        config_sensores_t sens;
        taskENTER_CRITICAL(&s_lock);
        sens = s_sensores;
        taskEXIT_CRITICAL(&s_lock);
        if (alarma == ALARMA_SENSOR_GABINETE) {
            eventos_registrar(activa ? EVENTO_AVISO : EVENTO_OK, EVENTO_ALARMA, "sistema",
                              activa ? "Sin lectura del sensor %s (AM2301A)" : "Sensor %s normalizado", sens.nombre[4]);
            break;
        }
        if (!activa) {
            eventos_registrar(EVENTO_OK, EVENTO_ALARMA, "sistema", "Sensores de temperatura normalizados");
            break;
        }
        char lista[96] = "";
        for (int i = 0; i < 4; i++) {
            if (sens.instalado[i] && !e->ntc_ok[i]) {
                if (lista[0]) strlcat(lista, "; ", sizeof(lista));
                strlcat(lista, sens.nombre[i], sizeof(lista));
            }
        }
        eventos_registrar(EVENTO_AVISO, EVENTO_ALARMA, "sistema", "Sensor sin lectura: %s",
                          lista[0] ? lista : "ADS1115 sin respuesta");
        break;
    }
    case ALARMA_RTC:
        eventos_registrar(activa ? EVENTO_AVISO : EVENTO_OK, EVENTO_ALARMA, "sistema", "%s",
                          activa ? "Reloj (RTC) sin respuesta: la hora de los eventos es estimada"
                                 : "Reloj (RTC) normalizado");
        break;
    case ALARMA_MODO_MANUAL:
        // El inicio ya lo registra quien forzo (web, pantalla, SNMP).
        if (!activa) {
            eventos_registrar(EVENTO_INFO, EVENTO_MANUAL, "automatica",
                              "Fin del modo manual: la automatica retomo el control");
        }
        break;
    case ALARMA_BYPASS_AUTO_DESHAB:
        if (activa) {
            eventos_registrar(EVENTO_AVISO, EVENTO_ALARMA, "automatica",
                              "Bypass automatico bloqueado: se supero el maximo de escalones a alta temperatura");
        }
        break;
    case ALARMA_MEDIDOR_ENERGIA:
        eventos_registrar(activa ? EVENTO_AVISO : EVENTO_OK, EVENTO_ALARMA, "sistema", "%s",
                          activa ? "Medidor de energia sin respuesta (RS-485)" : "Medidor de energia normalizado");
        break;
    default:
        break;
    }
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
        [ALARMA_MEDIDOR_ENERGIA]    = !estado->energia_ok,
    };

    uint16_t alarmas;
    leer_estado(NULL, &alarmas);
    uint16_t cambiaron = 0;

    for (int i = 0; i < NUM_ALARMAS; i++) {
        bool vigente = (alarmas >> i) & 1;
        if (condicion[i] == vigente) {
            s_contador_alarmas[i] = 0;
            continue;
        }
        uint8_t necesarias = condicion[i] ? s_def_alarmas[i].vueltas_on : s_def_alarmas[i].vueltas_off;
        if (++s_contador_alarmas[i] >= necesarias) {
            s_contador_alarmas[i] = 0;
            alarmas ^= (uint16_t)(1u << i);
            cambiaron |= (uint16_t)(1u << i);
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
            registrar_evento_alarma(i, activa, estado);
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
    // 0..3 = T1..T4 (sensores 1..4), 4/5 = temperatura/humedad del gabinete (sensor 5).
    uint8_t fila = indice_sensor < 4 ? (uint8_t)(indice_sensor + 1) : 5;
    uint8_t columna = indice_sensor == 5 ? SENS_CALIB_HUM : SENS_CALIB_TEMP;
    VB_CELDA(&msg, RAMA_SENSORES, columna, fila, a_decimas(constante));
    encolar(&msg);
}

esp_err_t snmp_braindlab_configurar(const config_snmp_t *cfg, bool *requiere_reinicio)
{
    // Las communities quedan fijas desde el arranque (lwIP guarda el puntero
    // a s_community_*, ver snmp_braindlab_init()): solo avisar.
    *requiere_reinicio = strcmp(cfg->community_lectura, s_community_lectura) != 0 ||
                         strcmp(cfg->community_escritura, s_community_escritura) != 0 ||
                         strcmp(cfg->community_trap, s_community_trap) != 0;

    taskENTER_CRITICAL(&s_lock);
    s_cfg = *cfg;
    taskEXIT_CRITICAL(&s_lock);

    msg_t msg = { .tipo = MSG_APLICAR_DESTINOS };
    encolar(&msg);

    esp_err_t err = config_braindlab_guardar_snmp(cfg);
    ESP_LOGI(TAG, "Config SNMP desde la web: trap1=%s (%s) trap2=%s (%s)%s",
             cfg->trap_ip[0], cfg->trap_habilitado[0] ? "on" : "off",
             cfg->trap_ip[1], cfg->trap_habilitado[1] ? "on" : "off",
             *requiere_reinicio ? " -- communities nuevas al reiniciar" : "");
    return err;
}

void snmp_braindlab_set_sensores(const config_sensores_t *sensores)
{
    taskENTER_CRITICAL(&s_lock);
    s_sensores = *sensores;
    taskEXIT_CRITICAL(&s_lock);
}

esp_err_t snmp_braindlab_trap_prueba(void)
{
    if (s_cola == NULL) {
        return ESP_ERR_INVALID_STATE; // agente sin arrancar (la red no levanto)
    }
    config_snmp_t cfg;
    leer_cfg(&cfg);
    bool hay_destino = false;
    for (int i = 0; i < CONFIG_SNMP_NUM_DESTINOS; i++) {
        hay_destino |= cfg.trap_habilitado[i] && strcmp(cfg.trap_ip[i], "0.0.0.0") != 0;
    }
    if (!hay_destino) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!red_eth_esta_conectado()) {
        return ESP_ERR_INVALID_STATE;
    }
    snmp_braindlab_estado_t e;
    leer_estado(&e, NULL);
    msg_t msg = { .tipo = MSG_TRAP, .trap = TRAP_PRUEBA };
    VB_SISTEMA(&msg, SIS_ESTADO, EST_CICLO, e.ciclo);
    VB_SISTEMA(&msg, SIS_ESTADO, EST_TEMP_MAXIMA, a_decimas(temperatura_maxima(&e)));
    encolar(&msg);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// MIB-2 system (1.3.6.1.2.1.1) -- lo minimo para que cualquier gestor
// reconozca el equipo (snmpwalk ... system, auto-descubrimiento, etc.).
// ---------------------------------------------------------------------------

static const char s_sys_descr[] = "controlador_braindlab - gestor de A/A (ESP32 + W5500)";

// sysContact/sysName/sysLocation salen de s_cfg (se editan desde la web).
static s16_t copiar_texto(void *value, const char *texto)
{
    size_t len = strlen(texto);
    memcpy(value, texto, len);
    return (s16_t)len;
}

static s16_t system_get(const struct snmp_scalar_array_node_def *def, void *value)
{
    config_snmp_t cfg;
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
    case 4: // sysContact
        leer_cfg(&cfg);
        return copiar_texto(value, cfg.contacto);
    case 5: // sysName
        leer_cfg(&cfg);
        return copiar_texto(value, cfg.nombre);
    case 6: // sysLocation
        leer_cfg(&cfg);
        return copiar_texto(value, cfg.ubicacion);
    default:
        return 0;
    }
}

static const struct snmp_scalar_array_node_def s_defs_system[] = {
    { 1, SNMP_ASN1_TYPE_OCTET_STRING, SNMP_NODE_INSTANCE_READ_ONLY },
    { 2, SNMP_ASN1_TYPE_OBJECT_ID,    SNMP_NODE_INSTANCE_READ_ONLY },
    { 3, SNMP_ASN1_TYPE_TIMETICKS,    SNMP_NODE_INSTANCE_READ_ONLY },
    { 4, SNMP_ASN1_TYPE_OCTET_STRING, SNMP_NODE_INSTANCE_READ_ONLY },
    { 5, SNMP_ASN1_TYPE_OCTET_STRING, SNMP_NODE_INSTANCE_READ_ONLY },
    { 6, SNMP_ASN1_TYPE_OCTET_STRING, SNMP_NODE_INSTANCE_READ_ONLY },
};

// ---------------------------------------------------------------------------
// Tablas: filas fijas 1..N. La fila va en cell_instance->reference.u32.
// ---------------------------------------------------------------------------

static snmp_err_t fila_exacta(u32_t filas, const u32_t *row_oid, u8_t row_oid_len, struct snmp_node_instance *inst)
{
    if (row_oid_len != 1 || row_oid[0] < 1 || row_oid[0] > filas) {
        return SNMP_ERR_NOSUCHINSTANCE;
    }
    inst->reference.u32 = row_oid[0];
    return SNMP_ERR_NOERROR;
}

static snmp_err_t fila_siguiente(u32_t filas, struct snmp_obj_id *row_oid, struct snmp_node_instance *inst)
{
    struct snmp_next_oid_state estado;
    u32_t resultado[1];
    snmp_next_oid_init(&estado, row_oid->id, row_oid->len, resultado, 1);
    for (u32_t f = 1; f <= filas; f++) {
        snmp_next_oid_check(&estado, &f, 1, (void *)(uintptr_t)f);
    }
    if (estado.status != SNMP_NEXT_OID_STATUS_SUCCESS) {
        return SNMP_ERR_NOSUCHINSTANCE;
    }
    snmp_oid_assign(row_oid, estado.next_oid, estado.next_oid_len);
    inst->reference.u32 = (u32_t)(uintptr_t)estado.reference;
    return SNMP_ERR_NOERROR;
}

#define COLUMNA(inst) SNMP_TABLE_GET_COLUMN_FROM_OID((inst)->instance_oid.id)
#define FILA(inst)    ((inst)->reference.u32)

// ---------------------------------------------------------------------------
// <BASE>.1 control -- tabla AA1..AA4. Estado RW usa el mismo callback que
// POST /control_aire: el override dura hasta que la climatizacion cambie de
// ciclo o hasta modoManual=0 (<BASE>.4.2.1), igual que desde la web.
// ---------------------------------------------------------------------------

static snmp_err_t control_celda(const u32_t *col, const u32_t *row, u8_t len, struct snmp_node_instance *inst)
{
    LWIP_UNUSED_ARG(col);
    return fila_exacta(FILAS_CONTROL, row, len, inst);
}

static snmp_err_t control_siguiente(const u32_t *col, struct snmp_obj_id *row, struct snmp_node_instance *inst)
{
    LWIP_UNUSED_ARG(col);
    return fila_siguiente(FILAS_CONTROL, row, inst);
}

static s16_t control_get(struct snmp_node_instance *inst, void *value)
{
    snmp_braindlab_estado_t e;
    leer_estado(&e, NULL);
    int i = (int)FILA(inst) - 1;
    s32_t *v = (s32_t *)value;
    bool instalado = i < e.cantidad_aires;

    switch (COLUMNA(inst)) {
    case CTRL_NOMBRE: {
        char nombre[CONFIG_SENSOR_NOMBRE_MAX];
        taskENTER_CRITICAL(&s_lock);
        memcpy(nombre, s_sensores.nombre_aire[i], sizeof(nombre));
        taskEXIT_CRITICAL(&s_lock);
        nombre[sizeof(nombre) - 1] = '\0';
        return copiar_texto(value, nombre);
    }
    case CTRL_INSTALADO: *v = instalado ? 1 : 0; break;
    case CTRL_ESTADO:    *v = e.salida_aire[i] ? 1 : 0; break;
    case CTRL_MODO:      *v = e.aire_manual[i] ? 1 : 0; break;
    case CTRL_RESERVA:   *v = (instalado && e.cantidad_aires > 1 && i == e.indice_reserva) ? 1 : 0; break;
    case CTRL_FALLAS:    *v = e.fails[i]; break;
    default: return 0;
    }
    return sizeof(s32_t);
}

static snmp_err_t control_set_test(struct snmp_node_instance *inst, u16_t len, void *value)
{
    if (!escritura_permitida()) {
        return SNMP_ERR_NOACCESS;
    }
    LWIP_UNUSED_ARG(len);
    if (COLUMNA(inst) != CTRL_ESTADO) {
        return SNMP_ERR_NOTWRITABLE;
    }
    s32_t v = *(s32_t *)value;
    if (v != 0 && v != 1) {
        return SNMP_ERR_WRONGVALUE;
    }
    snmp_braindlab_estado_t e;
    leer_estado(&e, NULL);
    if (FILA(inst) > e.cantidad_aires) {
        return SNMP_ERR_INCONSISTENTVALUE; // ese aire no esta instalado
    }
    return s_cbs.on_control_aire ? SNMP_ERR_NOERROR : SNMP_ERR_RESOURCEUNAVAILABLE;
}

static snmp_err_t control_set_value(struct snmp_node_instance *inst, u16_t len, void *value)
{
    LWIP_UNUSED_ARG(len);
    bool v = *(s32_t *)value != 0;
    s_cbs.on_control_aire((uint8_t)(FILA(inst) - 1), v);
    eventos_manual_aire("snmp", (uint8_t)(FILA(inst) - 1), v);
    ESP_LOGI(TAG, "SET control AA%lu = %d", (unsigned long)FILA(inst), v);
    return SNMP_ERR_NOERROR;
}

static const struct snmp_table_col_def s_cols_control[] = {
    { CTRL_NOMBRE,    SNMP_ASN1_TYPE_OCTET_STRING, SNMP_NODE_INSTANCE_READ_ONLY },
    { CTRL_INSTALADO, SNMP_ASN1_TYPE_INTEGER,      SNMP_NODE_INSTANCE_READ_ONLY },
    { CTRL_ESTADO,    SNMP_ASN1_TYPE_INTEGER,      SNMP_NODE_INSTANCE_READ_WRITE },
    { CTRL_MODO,      SNMP_ASN1_TYPE_INTEGER,      SNMP_NODE_INSTANCE_READ_ONLY },
    { CTRL_RESERVA,   SNMP_ASN1_TYPE_INTEGER,      SNMP_NODE_INSTANCE_READ_ONLY },
    { CTRL_FALLAS,    SNMP_ASN1_TYPE_INTEGER,      SNMP_NODE_INSTANCE_READ_ONLY },
};

// ---------------------------------------------------------------------------
// <BASE>.2 sensores -- tabla 1..6, cada fila con temperatura y humedad.
// Hoy: 1..4 = NTC T1..T4 (solo temperatura), 5 = AM2301A del gabinete
// (temperatura + humedad), 6 = libre. Lo que un sensor no mide va en 0.
// Referencia (RW, decimas): GET = lectura calibrada actual; SET = "el valor
// real es este" -> mismo calculo que POST /calibrar_sensor.
// ---------------------------------------------------------------------------

enum { TIPO_NO_INSTALADO = 0, TIPO_TEMPERATURA = 1, TIPO_TEMP_HUMEDAD = 2 };
enum { ESTADO_SENSOR_NO_INSTALADO = 0, ESTADO_SENSOR_OK = 1, ESTADO_SENSOR_FALLA = 2 };

static const uint8_t s_tipo_sensor[FILAS_SENSORES] = {
    TIPO_TEMPERATURA, TIPO_TEMPERATURA, TIPO_TEMPERATURA, TIPO_TEMPERATURA, TIPO_TEMP_HUMEDAD, TIPO_NO_INSTALADO,
};

static bool sensor_instalado(u32_t fila)
{
    taskENTER_CRITICAL(&s_lock);
    bool instalado = fila >= 1 && fila <= FILAS_SENSORES && s_sensores.instalado[fila - 1];
    taskEXIT_CRITICAL(&s_lock);
    return instalado;
}

// Nombre que entiende on_calibrar_sensor() (mismo que POST /calibrar_sensor),
// o NULL si esa fila/columna no se puede calibrar (o el sensor no esta instalado).
static const char *sensor_a_calibrar(u32_t fila, u32_t columna)
{
    static const char *const temp[FILAS_SENSORES] = { "t1", "t2", "t3", "t4", "temp_gestor", NULL };
    if (!sensor_instalado(fila)) {
        return NULL;
    }
    if (columna == SENS_REF_TEMP) {
        return temp[fila - 1];
    }
    if (columna == SENS_REF_HUM && fila == 5) {
        return "humedad_gestor";
    }
    return NULL;
}

static snmp_err_t sensores_celda(const u32_t *col, const u32_t *row, u8_t len, struct snmp_node_instance *inst)
{
    LWIP_UNUSED_ARG(col);
    return fila_exacta(FILAS_SENSORES, row, len, inst);
}

static snmp_err_t sensores_siguiente(const u32_t *col, struct snmp_obj_id *row, struct snmp_node_instance *inst)
{
    LWIP_UNUSED_ARG(col);
    return fila_siguiente(FILAS_SENSORES, row, inst);
}

static s16_t sensores_get(struct snmp_node_instance *inst, void *value)
{
    snmp_braindlab_estado_t e;
    leer_estado(&e, NULL);
    int i = (int)FILA(inst) - 1;
    s32_t *v = (s32_t *)value;

    char nombre[CONFIG_SENSOR_NOMBRE_MAX];
    taskENTER_CRITICAL(&s_lock);
    bool instalado = s_sensores.instalado[i];
    memcpy(nombre, s_sensores.nombre[i], sizeof(nombre));
    taskEXIT_CRITICAL(&s_lock);
    nombre[sizeof(nombre) - 1] = '\0';

    // No instalado: la fila existe (los OIDs no se mueven entre sitios) pero
    // tipo/estado = noInstalado y valores en 0 -- el gestor no lo marca como falla.
    float temperatura = 0.0f, humedad = 0.0f, calib_temp = 0.0f, calib_hum = 0.0f;
    uint8_t estado = ESTADO_SENSOR_NO_INSTALADO;
    if (!instalado) {
        // todo en 0
    } else if (i < 4) {
        temperatura = e.temperaturas[i];
        calib_temp = e.calibracion[i];
        estado = e.ntc_ok[i] ? ESTADO_SENSOR_OK : ESTADO_SENSOR_FALLA;
    } else if (i == 4) {
        temperatura = e.temp_gestor;
        humedad = e.humedad_gestor;
        calib_temp = e.calibracion[4];
        calib_hum = e.calibracion[5];
        estado = e.gestor_ok ? ESTADO_SENSOR_OK : ESTADO_SENSOR_FALLA;
    }

    switch (COLUMNA(inst)) {
    case SENS_NOMBRE:      return copiar_texto(value, nombre);
    case SENS_TIPO:        *v = instalado ? s_tipo_sensor[i] : TIPO_NO_INSTALADO; break;
    case SENS_ESTADO:      *v = estado; break;
    case SENS_TEMPERATURA:
    case SENS_REF_TEMP:    *v = a_decimas(temperatura); break;
    case SENS_HUMEDAD:
    case SENS_REF_HUM:     *v = a_decimas(humedad); break;
    case SENS_CALIB_TEMP:  *v = a_decimas(calib_temp); break;
    case SENS_CALIB_HUM:   *v = a_decimas(calib_hum); break;
    default: return 0;
    }
    return sizeof(s32_t);
}

static snmp_err_t sensores_set_test(struct snmp_node_instance *inst, u16_t len, void *value)
{
    if (!escritura_permitida()) {
        return SNMP_ERR_NOACCESS;
    }
    LWIP_UNUSED_ARG(len);
    u32_t columna = COLUMNA(inst);
    if (columna != SENS_REF_TEMP && columna != SENS_REF_HUM) {
        return SNMP_ERR_NOTWRITABLE;
    }
    if (sensor_a_calibrar(FILA(inst), columna) == NULL) {
        return SNMP_ERR_INCONSISTENTVALUE; // no instalado o no mide eso
    }
    if (!s_cbs.on_calibrar_sensor) {
        return SNMP_ERR_RESOURCEUNAVAILABLE;
    }
    s32_t v = *(s32_t *)value;
    if (columna == SENS_REF_HUM) {
        return (v >= 0 && v <= 1000) ? SNMP_ERR_NOERROR : SNMP_ERR_WRONGVALUE;  // 0..100 %
    }
    return (v >= -400 && v <= 1250) ? SNMP_ERR_NOERROR : SNMP_ERR_WRONGVALUE;   // -40..125 °C
}

static snmp_err_t sensores_set_value(struct snmp_node_instance *inst, u16_t len, void *value)
{
    LWIP_UNUSED_ARG(len);
    const char *sensor = sensor_a_calibrar(FILA(inst), COLUMNA(inst));
    if (sensor == NULL) {
        return SNMP_ERR_NOTWRITABLE;
    }
    // on_calibrar_sensor() (app_main.c) guarda, aplica y dispara el trap
    // calibracionModificada -- igual que si viniera de la web.
    s32_t v = *(s32_t *)value;
    s_cbs.on_calibrar_sensor(sensor, (float)v / 10.0f);
    // Sin %f: corre en el thread de lwIP (pila chica).
    long abs_v = v < 0 ? -(long)v : (long)v;
    eventos_registrar(EVENTO_INFO, EVENTO_CONFIG, "snmp", "Calibracion de %s: referencia %s%ld.%ld",
                      sensor, v < 0 ? "-" : "", abs_v / 10, abs_v % 10);
    return SNMP_ERR_NOERROR;
}

static const struct snmp_table_col_def s_cols_sensores[] = {
    { SENS_NOMBRE,      SNMP_ASN1_TYPE_OCTET_STRING, SNMP_NODE_INSTANCE_READ_ONLY },
    { SENS_TIPO,        SNMP_ASN1_TYPE_INTEGER,      SNMP_NODE_INSTANCE_READ_ONLY },
    { SENS_ESTADO,      SNMP_ASN1_TYPE_INTEGER,      SNMP_NODE_INSTANCE_READ_ONLY },
    { SENS_TEMPERATURA, SNMP_ASN1_TYPE_INTEGER,      SNMP_NODE_INSTANCE_READ_ONLY },
    { SENS_HUMEDAD,     SNMP_ASN1_TYPE_INTEGER,      SNMP_NODE_INSTANCE_READ_ONLY },
    { SENS_CALIB_TEMP,  SNMP_ASN1_TYPE_INTEGER,      SNMP_NODE_INSTANCE_READ_ONLY },
    { SENS_CALIB_HUM,   SNMP_ASN1_TYPE_INTEGER,      SNMP_NODE_INSTANCE_READ_ONLY },
    { SENS_REF_TEMP,    SNMP_ASN1_TYPE_INTEGER,      SNMP_NODE_INSTANCE_READ_WRITE },
    { SENS_REF_HUM,     SNMP_ASN1_TYPE_INTEGER,      SNMP_NODE_INSTANCE_READ_WRITE },
};

// ---------------------------------------------------------------------------
// <BASE>.3 energia -- tabla fases R,S,T del medidor Modbus (JSY-MK-333G).
// Voltaje en decimas de V (2205 = 220.5 V), corriente en centesimas de A
// (1234 = 12.34 A). Si el medidor no responde, estado = sinLectura y los
// valores son la ultima lectura que hubo.
// ---------------------------------------------------------------------------

static snmp_err_t energia_celda(const u32_t *col, const u32_t *row, u8_t len, struct snmp_node_instance *inst)
{
    LWIP_UNUSED_ARG(col);
    return fila_exacta(FILAS_ENERGIA, row, len, inst);
}

static snmp_err_t energia_siguiente(const u32_t *col, struct snmp_obj_id *row, struct snmp_node_instance *inst)
{
    LWIP_UNUSED_ARG(col);
    return fila_siguiente(FILAS_ENERGIA, row, inst);
}

static s16_t energia_get(struct snmp_node_instance *inst, void *value)
{
    static const char *const fases[FILAS_ENERGIA] = { "R", "S", "T" };
    snmp_braindlab_estado_t e;
    leer_estado(&e, NULL);
    int i = (int)FILA(inst) - 1;
    s32_t *v = (s32_t *)value;

    switch (COLUMNA(inst)) {
    case ENER_NOMBRE:    return copiar_texto(value, fases[i]);
    case ENER_ESTADO:    *v = e.energia_ok ? 1 : 2; break; // ok(1) / sinLectura(2)
    case ENER_VOLTAJE:   *v = a_decimas(e.voltajes[i]); break;
    case ENER_CORRIENTE: *v = isfinite(e.corrientes[i]) ? (s32_t)lroundf(e.corrientes[i] * 100.0f) : 0; break;
    default: return 0;
    }
    return sizeof(s32_t);
}

static const struct snmp_table_col_def s_cols_energia[] = {
    { ENER_NOMBRE,    SNMP_ASN1_TYPE_OCTET_STRING, SNMP_NODE_INSTANCE_READ_ONLY },
    { ENER_ESTADO,    SNMP_ASN1_TYPE_INTEGER,      SNMP_NODE_INSTANCE_READ_ONLY },
    { ENER_VOLTAJE,   SNMP_ASN1_TYPE_INTEGER,      SNMP_NODE_INSTANCE_READ_ONLY },
    { ENER_CORRIENTE, SNMP_ASN1_TYPE_INTEGER,      SNMP_NODE_INSTANCE_READ_ONLY },
};

// ---------------------------------------------------------------------------
// <BASE>.4.1 sistema/estado (RO)
// ---------------------------------------------------------------------------

static s16_t estado_get(const struct snmp_scalar_array_node_def *def, void *value)
{
    snmp_braindlab_estado_t e;
    leer_estado(&e, NULL);
    s32_t *v = (s32_t *)value;

    switch (def->oid) {
    case EST_CICLO:                  *v = e.ciclo; break;
    case EST_TEMP_MAXIMA:            *v = a_decimas(temperatura_maxima(&e)); break;
    case EST_UNIDAD_RESERVA:         *v = e.indice_reserva + 1; break;
    case EST_BYPASS_FISICO:          *v = e.bypass_activo ? 1 : 0; break;
    case EST_BYPASS_AUTO_HABILITADO: *v = e.bypass_auto_habilitado ? 1 : 0; break;
    case EST_RTC_OK:                 *v = e.rtc_ok ? 1 : 0; break;
    default: return 0;
    }
    return sizeof(s32_t);
}

static const struct snmp_scalar_array_node_def s_defs_estado[] = {
    { EST_CICLO,                  SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { EST_TEMP_MAXIMA,            SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { EST_UNIDAD_RESERVA,         SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { EST_BYPASS_FISICO,          SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { EST_BYPASS_AUTO_HABILITADO, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { EST_RTC_OK,                 SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
};

// ---------------------------------------------------------------------------
// <BASE>.4.2 sistema/mando (RW) -- mismos callbacks que POST /control_at,
// /control_bypass y /control_automatico.
// ---------------------------------------------------------------------------

static s16_t mando_get(const struct snmp_scalar_array_node_def *def, void *value)
{
    snmp_braindlab_estado_t e;
    leer_estado(&e, NULL);
    s32_t *v = (s32_t *)value;

    switch (def->oid) {
    case MANDO_MODO_MANUAL:      *v = e.modo_manual ? 1 : 0; break;
    case MANDO_BYPASS:           *v = e.bypass_solicitado ? 1 : 0; break;   // BP_S
    case MANDO_BYPASS_MANUAL:    *v = e.bypass_manual ? 1 : 0; break;
    case MANDO_ALARMA_AT:        *v = e.alarma_at ? 1 : 0; break;           // OUT_AT
    case MANDO_ALARMA_AT_MANUAL: *v = e.at_manual ? 1 : 0; break;
    default: return 0;
    }
    return sizeof(s32_t);
}

static snmp_err_t mando_set_test(const struct snmp_scalar_array_node_def *def, u16_t len, void *value)
{
    if (!escritura_permitida()) {
        return SNMP_ERR_NOACCESS;
    }
    LWIP_UNUSED_ARG(len);
    s32_t v = *(s32_t *)value;

    switch (def->oid) {
    case MANDO_MODO_MANUAL:
        // Solo se puede APAGAR (volver a automatico). Para entrar en manual
        // se escribe la salida puntual que se quiere forzar.
        if (v != 0) {
            return SNMP_ERR_WRONGVALUE;
        }
        return s_cbs.on_control_automatico ? SNMP_ERR_NOERROR : SNMP_ERR_RESOURCEUNAVAILABLE;
    case MANDO_BYPASS:
        if (v != 0 && v != 1) {
            return SNMP_ERR_WRONGVALUE;
        }
        return s_cbs.on_control_bypass ? SNMP_ERR_NOERROR : SNMP_ERR_RESOURCEUNAVAILABLE;
    case MANDO_ALARMA_AT:
        if (v != 0 && v != 1) {
            return SNMP_ERR_WRONGVALUE;
        }
        return s_cbs.on_control_at ? SNMP_ERR_NOERROR : SNMP_ERR_RESOURCEUNAVAILABLE;
    default:
        return SNMP_ERR_NOTWRITABLE;
    }
}

static snmp_err_t mando_set_value(const struct snmp_scalar_array_node_def *def, u16_t len, void *value)
{
    LWIP_UNUSED_ARG(len);
    bool v = *(s32_t *)value != 0;

    switch (def->oid) {
    case MANDO_MODO_MANUAL: s_cbs.on_control_automatico(); eventos_manual_automatico("snmp"); break;
    case MANDO_BYPASS:      s_cbs.on_control_bypass(v); eventos_manual_bypass("snmp", v); break;
    case MANDO_ALARMA_AT:   s_cbs.on_control_at(v); eventos_manual_at("snmp", v); break;
    default: return SNMP_ERR_NOTWRITABLE;
    }
    ESP_LOGI(TAG, "SET mando.%lu = %d", (unsigned long)def->oid, v);
    return SNMP_ERR_NOERROR;
}

static const struct snmp_scalar_array_node_def s_defs_mando[] = {
    { MANDO_MODO_MANUAL,      SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
    { MANDO_BYPASS,           SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
    { MANDO_BYPASS_MANUAL,    SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
    { MANDO_ALARMA_AT,        SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_WRITE },
    { MANDO_ALARMA_AT_MANUAL, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
};

// ---------------------------------------------------------------------------
// <BASE>.4.3 sistema/configuracion (RO) -- los umbrales que
// tarea_climatizacion tiene EN USO (cargados al arrancar). Un POST
// /configurar_climatizacion se ve aca recien despues de reiniciar.
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
// <BASE>.4.4 sistema/traps (RW): .1 trapDestino1 (IpAddress) .2 trapHabilitado1
//                                .3 trapDestino2 (IpAddress) .4 trapHabilitado2
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
    if (!escritura_permitida()) {
        return SNMP_ERR_NOACCESS;
    }
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
// <BASE>.5 alarmas (RO) -- el trap es UDP sin confirmacion: si el gestor
// estaba apagado o el cable desconectado, se pierde. Estas hojas permiten
// consultarlas por polling en cualquier momento.
// ---------------------------------------------------------------------------

static s16_t alarmas_get(const struct snmp_scalar_array_node_def *def, void *value)
{
    uint16_t alarmas;
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
    { 9, SNMP_ASN1_TYPE_INTEGER, SNMP_NODE_INSTANCE_READ_ONLY },
};

// ---------------------------------------------------------------------------
// Arbol y MIBs
// ---------------------------------------------------------------------------

static const struct snmp_scalar_array_node s_nodo_system =
    SNMP_SCALAR_CREATE_ARRAY_NODE(1, s_defs_system, system_get, NULL, NULL);

// Las tablas cuelgan directo de su rama: <BASE>.1 = bdlControlTabla, y lwIP
// atiende .1 (entry) .columna .fila debajo.
static const struct snmp_table_node s_tabla_control = SNMP_TABLE_CREATE(
    RAMA_CONTROL, s_cols_control, control_celda, control_siguiente, control_get, control_set_test,
    control_set_value);
static const struct snmp_table_node s_tabla_sensores = SNMP_TABLE_CREATE(
    RAMA_SENSORES, s_cols_sensores, sensores_celda, sensores_siguiente, sensores_get, sensores_set_test,
    sensores_set_value);
static const struct snmp_table_node s_tabla_energia = SNMP_TABLE_CREATE(
    RAMA_ENERGIA, s_cols_energia, energia_celda, energia_siguiente, energia_get, NULL, NULL);

static const struct snmp_scalar_array_node s_nodo_estado =
    SNMP_SCALAR_CREATE_ARRAY_NODE(SIS_ESTADO, s_defs_estado, estado_get, NULL, NULL);
static const struct snmp_scalar_array_node s_nodo_mando =
    SNMP_SCALAR_CREATE_ARRAY_NODE(SIS_MANDO, s_defs_mando, mando_get, mando_set_test, mando_set_value);
static const struct snmp_scalar_array_node s_nodo_configuracion =
    SNMP_SCALAR_CREATE_ARRAY_NODE(SIS_CONFIGURACION, s_defs_configuracion, configuracion_get, NULL, NULL);
static const struct snmp_scalar_array_node s_nodo_traps =
    SNMP_SCALAR_CREATE_ARRAY_NODE(SIS_TRAPS, s_defs_traps, traps_get, traps_set_test, traps_set_value);
static const struct snmp_node *const s_hijos_sistema[] = {
    &s_nodo_estado.node.node,
    &s_nodo_mando.node.node,
    &s_nodo_configuracion.node.node,
    &s_nodo_traps.node.node,
};
static const struct snmp_tree_node s_nodo_sistema = SNMP_CREATE_TREE_NODE(RAMA_SISTEMA, s_hijos_sistema);

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
    &s_tabla_control.node.node,
    &s_tabla_sensores.node.node,
    &s_tabla_energia.node.node,
    &s_nodo_sistema.node,
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
        for (int j = 0; j < msg->vb[i].len; j++) {
            vbs[i].oid.id[OID_BASE_LEN + j] = msg->vb[i].sufijo[j];
        }
        vbs[i].oid.len = (u8_t)(OID_BASE_LEN + msg->vb[i].len);
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
    uint16_t alarmas;
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

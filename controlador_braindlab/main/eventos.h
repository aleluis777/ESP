#pragma once

// Registro de eventos: arranques (con el motivo), alarmas que se activan y
// normalizan, cambios de ciclo, acciones manuales (con el origen: web,
// pantalla, SNMP), cambios de configuracion y accesos a la web. Distinto del
// historico por minuto (registro_sd.h): solo hay una linea cuando PASA algo.
//
// Un archivo CSV por mes en la MicroSD:
//
//   /sd/EVENTOS/AAAA-MM.CSV    (ej. /sd/EVENTOS/2026-10.CSV)
//   fecha_hora,nivel,categoria,origen,detalle
//
// Lo escribe tarea_registro_sd (la misma que escribe el historico, asi
// todo el acceso a la SD queda en un solo task): eventos_registrar() solo
// arma la linea y la encola, no bloquea. Sin SD el evento se pierde (queda
// en el log serie igual).
//
// La hora sale del RTC: tarea_climatizacion la refresca con
// eventos_actualizar_hora() en cada vuelta. Lo que se registra antes de la
// primera lectura (el arranque) espera en memoria y se fecha hacia atras
// con el tiempo transcurrido desde que paso.

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EVENTO_INFO = 0,   // informativo (cambio de ciclo, accion manual, config)
    EVENTO_OK,         // una alarma se normalizo
    EVENTO_AVISO,      // falla o situacion a revisar
    EVENTO_CRITICO,    // alta temperatura, falla de bypass, error de firmware
} evento_nivel_t;

typedef enum {
    EVENTO_ARRANQUE = 0,
    EVENTO_ALARMA,
    EVENTO_CONTROL,    // la automatica (ciclos), red
    EVENTO_MANUAL,     // forzados a mano (aires, bypass, AT, volver a automatico)
    EVENTO_CONFIG,
    EVENTO_SEGURIDAD,  // inicios de sesion, contrasenas
} evento_categoria_t;

#define EVENTOS_DIR        "/sd/EVENTOS"
#define EVENTO_ORIGEN_MAX  16  // "web:admin", "pantalla", "snmp", "automatica"...
#define EVENTO_DETALLE_MAX 112

// Una linea del archivo de eventos (lo que viaja por la cola de la SD).
typedef struct {
    time_t cuando;
    uint8_t nivel;     // evento_nivel_t
    uint8_t categoria; // evento_categoria_t
    char origen[EVENTO_ORIGEN_MAX];
    char detalle[EVENTO_DETALLE_MAX];
} evento_t;

// Registra un evento. 'origen' quien lo causo ("sistema", "automatica",
// "web:admin", "pantalla", "snmp", "contacto"...). 'fmt' estilo printf.
// Se puede llamar desde cualquier tarea; no bloquea.
void eventos_registrar(evento_nivel_t nivel, evento_categoria_t categoria, const char *origen,
                       const char *fmt, ...) __attribute__((format(printf, 4, 5)));

// Acciones manuales (mismo texto desde la web, la pantalla y SNMP).
// 'indice' 0..3 = AA1..AA4.
void eventos_manual_aire(const char *origen, uint8_t indice, bool encendido);
void eventos_manual_bypass(const char *origen, bool solicitado);
void eventos_manual_at(const char *origen, bool activa);
void eventos_manual_automatico(const char *origen);

// Hora actual del RTC (valida: el RTC respondio, avanza y el anio es
// razonable). La llama tarea_climatizacion en cada vuelta.
void eventos_actualizar_hora(const struct tm *ahora);

// Motivo del proximo reinicio ("desde la web (admin)", "actualizacion de
// firmware"...): queda en memoria RTC (sobrevive al esp_restart()) y lo
// usa el evento de arranque. Llamar justo antes de esp_restart().
void eventos_motivo_reinicio(const char *motivo);

// Evento de arranque con el motivo (corte de luz, baja tension, error de
// firmware, reinicio pedido...). Llamar una vez al principio de app_main().
void eventos_registrar_arranque(const char *version_firmware);

// Compara la version con la del arranque anterior (guardada en NVS) y, si
// cambio, registra "Firmware actualizado: de X a Y". Llamar cuando la NVS ya
// esta inicializada (despues de auth_web_init()).
void eventos_revisar_version(const char *version_firmware);

// Texto de las columnas nivel/categoria del CSV ("aviso", "alarma"...).
const char *eventos_texto_nivel(uint8_t nivel);
const char *eventos_texto_categoria(uint8_t categoria);

// ---------- Lectura (la usa servidor_web.c) ----------

// Meses con eventos (de /sd/EVENTOS), del mas nuevo al mas viejo: llena
// meses[i] con "AAAA-MM" y kb[i]. Devuelve cuantos o -1 si no hay SD.
int eventos_meses_con_datos(char meses[][8], uint32_t kb[], int max);

// Ruta del CSV de un mes. false si la fecha es invalida o no hay SD.
bool eventos_ruta_mes(int anio, int mes, char *ruta, size_t largo);

#ifdef __cplusplus
}
#endif

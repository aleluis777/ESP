#include "eventos.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "registro_sd.h"

static const char *TAG = "EVENTOS";

// Referencia de hora: el ultimo valor bueno del RTC y el esp_timer de ese
// momento. La hora de un evento = referencia + lo que paso desde entonces
// (sirve aunque el RTC falle despues: el esp_timer sigue contando).
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_hay_hora = false;
static time_t s_hora_ref;
static int64_t s_us_ref;

// Tope de eventos por minuto: una alarma que va y viene (sensor que falla
// de a ratos) no llena la SD. Lo que pasa del tope se cuenta y se avisa con
// un solo evento, junto con el primer evento de un minuto posterior.
#define TOPE_POR_MINUTO 20
static int64_t s_ventana_us = 0;   // inicio del minuto actual
static int s_en_ventana = 0;       // eventos aceptados en este minuto
static int s_omitidos = 0;         // descartados por el tope (se avisan despues)

// Eventos registrados antes de tener hora (el arranque, sobre todo).
#define PENDIENTES_MAX 8
typedef struct {
    evento_t ev;
    int64_t us; // esp_timer_get_time() cuando paso
} pendiente_t;
static pendiente_t s_pendientes[PENDIENTES_MAX];
static int s_num_pendientes = 0;

// Motivo del reinicio pedido por software: memoria RTC, sobrevive a
// esp_restart() (no a un corte de luz -- ahi la magia queda en basura y no
// coincide).
#define MAGIA_REINICIO 0xB0D1CAFEu
static RTC_NOINIT_ATTR uint32_t s_reinicio_magia;
static RTC_NOINIT_ATTR char s_reinicio_motivo[48];

const char *eventos_texto_nivel(uint8_t nivel)
{
    static const char *const textos[] = { "info", "ok", "aviso", "critico" };
    return nivel < sizeof(textos) / sizeof(textos[0]) ? textos[nivel] : "info";
}

const char *eventos_texto_categoria(uint8_t categoria)
{
    static const char *const textos[] = { "arranque", "alarma", "control", "manual", "config", "seguridad" };
    return categoria < sizeof(textos) / sizeof(textos[0]) ? textos[categoria] : "otro";
}

// El CSV no usa comillas: comas -> ';' y saltos de linea -> espacio, asi
// cada evento es siempre UNA linea con 5 columnas.
static void limpiar_campo(char *s)
{
    for (; *s; s++) {
        if (*s == ',') *s = ';';
        else if (*s == '\n' || *s == '\r' || *s == '"') *s = ' ';
    }
}

// Encola (o deja pendiente si todavia no hay hora) un evento ya armado.
static void guardar(evento_t *ev, int64_t ahora_us)
{
    bool hay_hora;
    time_t ref;
    int64_t us_ref;
    taskENTER_CRITICAL(&s_lock);
    hay_hora = s_hay_hora;
    ref = s_hora_ref;
    us_ref = s_us_ref;
    if (!hay_hora) {
        if (s_num_pendientes == PENDIENTES_MAX) { // lleno: se pierde el mas viejo
            memmove(&s_pendientes[0], &s_pendientes[1], sizeof(s_pendientes[0]) * (PENDIENTES_MAX - 1));
            s_num_pendientes--;
        }
        s_pendientes[s_num_pendientes].ev = *ev;
        s_pendientes[s_num_pendientes].us = ahora_us;
        s_num_pendientes++;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (hay_hora) {
        ev->cuando = ref + (time_t)((ahora_us - us_ref) / 1000000LL);
        registro_sd_encolar_evento(ev);
    }
}

void eventos_registrar(evento_nivel_t nivel, evento_categoria_t categoria, const char *origen,
                       const char *fmt, ...)
{
    // Tope por minuto. Al empezar un minuto nuevo, si hubo descartados se
    // deja constancia antes del evento actual.
    int64_t ahora_us = esp_timer_get_time();
    int omitidos_aviso = 0;
    bool aceptado;
    taskENTER_CRITICAL(&s_lock);
    if (ahora_us - s_ventana_us >= 60LL * 1000000LL) {
        s_ventana_us = ahora_us;
        s_en_ventana = 0;
        omitidos_aviso = s_omitidos;
        s_omitidos = 0;
    }
    aceptado = s_en_ventana < TOPE_POR_MINUTO;
    if (aceptado) {
        s_en_ventana++;
    } else {
        s_omitidos++;
    }
    taskEXIT_CRITICAL(&s_lock);

    if (omitidos_aviso > 0) {
        evento_t aviso = { .nivel = EVENTO_AVISO, .categoria = EVENTO_ALARMA };
        strlcpy(aviso.origen, "sistema", sizeof(aviso.origen));
        snprintf(aviso.detalle, sizeof(aviso.detalle),
                 "Se omitieron %d eventos (mas de %d por minuto: alarma intermitente?)", omitidos_aviso,
                 TOPE_POR_MINUTO);
        ESP_LOGW(TAG, "%s", aviso.detalle);
        guardar(&aviso, ahora_us);
    }
    if (!aceptado) {
        return;
    }

    evento_t ev = { .nivel = (uint8_t)nivel, .categoria = (uint8_t)categoria };
    strlcpy(ev.origen, origen ? origen : "sistema", sizeof(ev.origen));
    va_list args;
    va_start(args, fmt);
    vsnprintf(ev.detalle, sizeof(ev.detalle), fmt, args);
    va_end(args);
    limpiar_campo(ev.origen);
    limpiar_campo(ev.detalle);

    ESP_LOGI(TAG, "[%s/%s] %s: %s", eventos_texto_nivel(ev.nivel), eventos_texto_categoria(ev.categoria),
             ev.origen, ev.detalle);
    guardar(&ev, ahora_us);
}

void eventos_actualizar_hora(const struct tm *ahora)
{
    struct tm copia = *ahora;
    time_t t = mktime(&copia);
    if (t == (time_t)-1) {
        return;
    }
    int64_t us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    s_hay_hora = true;
    s_hora_ref = t;
    s_us_ref = us;
    taskEXIT_CRITICAL(&s_lock);

    // Los que esperaban hora: se fechan hacia atras y se encolan, de a uno
    // (fuera del lock: encolar puede tardar).
    while (1) {
        pendiente_t p;
        bool hay = false;
        taskENTER_CRITICAL(&s_lock);
        if (s_num_pendientes > 0) {
            p = s_pendientes[0];
            memmove(&s_pendientes[0], &s_pendientes[1], sizeof(s_pendientes[0]) * (PENDIENTES_MAX - 1));
            s_num_pendientes--;
            hay = true;
        }
        taskEXIT_CRITICAL(&s_lock);
        if (!hay) {
            break;
        }
        p.ev.cuando = t - (time_t)((us - p.us) / 1000000LL);
        registro_sd_encolar_evento(&p.ev);
    }
}

// ---------- Acciones manuales ----------

void eventos_manual_aire(const char *origen, uint8_t indice, bool encendido)
{
    eventos_registrar(EVENTO_INFO, EVENTO_MANUAL, origen, "AA%u forzado a mano: %s", (unsigned)indice + 1,
                      encendido ? "ENCENDIDO" : "APAGADO");
}

void eventos_manual_bypass(const char *origen, bool solicitado)
{
    eventos_registrar(EVENTO_AVISO, EVENTO_MANUAL, origen, "Bypass pedido a mano: %s",
                      solicitado ? "ACTIVAR" : "DESACTIVAR");
}

void eventos_manual_at(const char *origen, bool activa)
{
    eventos_registrar(EVENTO_INFO, EVENTO_MANUAL, origen, "Alarma AT forzada a mano: %s",
                      activa ? "ACTIVADA" : "DESACTIVADA");
}

void eventos_manual_automatico(const char *origen)
{
    eventos_registrar(EVENTO_INFO, EVENTO_MANUAL, origen, "Volver a automatico: se cancelan los forzados a mano");
}

// ---------- Arranque ----------

void eventos_motivo_reinicio(const char *motivo)
{
    strlcpy(s_reinicio_motivo, motivo, sizeof(s_reinicio_motivo));
    s_reinicio_magia = MAGIA_REINICIO;
}

// Compara la version que corre con la del arranque anterior (NVS) y la
// guarda si cambio. Devuelve true y llena 'anterior' si hubo actualizacion.
// Mas confiable que la memoria RTC para la OTA: el firmware nuevo puede
// ubicar s_reinicio_* en otra direccion.
static bool firmware_cambio(const char *version, char *anterior, size_t largo)
{
    nvs_handle_t nvs;
    if (nvs_open("eventos", NVS_READWRITE, &nvs) != ESP_OK) {
        return false; // sin NVS: no se puede saber
    }
    size_t tam = largo;
    bool habia = nvs_get_str(nvs, "version", anterior, &tam) == ESP_OK;
    bool cambio = habia && strcmp(anterior, version) != 0;
    if (!habia || cambio) {
        nvs_set_str(nvs, "version", version);
        nvs_commit(nvs);
    }
    nvs_close(nvs);
    return cambio;
}

void eventos_registrar_arranque(const char *version_firmware)
{
    char motivo[sizeof(s_reinicio_motivo)] = "";
    if (s_reinicio_magia == MAGIA_REINICIO) {
        memcpy(motivo, s_reinicio_motivo, sizeof(motivo));
        motivo[sizeof(motivo) - 1] = '\0';
    }
    s_reinicio_magia = 0; // un solo uso

    evento_nivel_t nivel = EVENTO_AVISO;
    const char *texto;
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  texto = "Equipo encendido (instalacion o volvio la energia)"; break;
    case ESP_RST_BROWNOUT: texto = "Reinicio por baja tension (brownout): revisar la fuente"; break;
    case ESP_RST_PANIC:    texto = "Reinicio por error de firmware (panic)"; nivel = EVENTO_CRITICO; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      texto = "Reinicio por watchdog (el firmware se colgo)"; nivel = EVENTO_CRITICO; break;
    case ESP_RST_EXT:      texto = "Reinicio por el pin de reset"; nivel = EVENTO_INFO; break;
    case ESP_RST_SW:       texto = motivo[0] ? "Reinicio" : "Reinicio por software"; nivel = EVENTO_INFO; break;
    default:               texto = "Reinicio (motivo desconocido)"; break;
    }
    eventos_registrar(nivel, EVENTO_ARRANQUE, "sistema", "%s%s%s - firmware %s", texto,
                      motivo[0] ? ": " : "", motivo, version_firmware);
}

void eventos_revisar_version(const char *version_firmware)
{
    char anterior[33] = "";
    if (firmware_cambio(version_firmware, anterior, sizeof(anterior))) {
        eventos_registrar(EVENTO_INFO, EVENTO_ARRANQUE, "sistema", "Firmware actualizado: de %s a %s", anterior,
                          version_firmware);
    }
}

// ---------- Lectura ----------

int eventos_meses_con_datos(char meses[][8], uint32_t kb[], int max)
{
    if (!registro_sd_montada()) {
        return -1;
    }
    DIR *dir = opendir(EVENTOS_DIR);
    if (!dir) {
        return 0; // todavia no hay ningun evento
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && n < max) {
        // Solo "AAAA-MM.CSV" (11 caracteres).
        const char *nom = e->d_name;
        if (strlen(nom) != 11 || nom[4] != '-' || strcasecmp(nom + 7, ".CSV") != 0) {
            continue;
        }
        int anio = atoi(nom), mes = atoi(nom + 5);
        if (anio < 2000 || anio > 2099 || mes < 1 || mes > 12) {
            continue;
        }
        char ruta[48];
        snprintf(ruta, sizeof(ruta), EVENTOS_DIR "/%.11s", nom);
        struct stat st;
        if (stat(ruta, &st) != 0 || st.st_size == 0) {
            continue;
        }
        snprintf(meses[n], 8, "%04d-%02d", anio, mes);
        kb[n] = ((uint32_t)st.st_size + 1023) / 1024;
        n++;
    }
    closedir(dir);

    // Del mas nuevo al mas viejo ("AAAA-MM" ordena bien como texto).
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && strcmp(meses[j - 1], meses[j]) < 0; j--) {
            char tmp[8];
            memcpy(tmp, meses[j], 8);
            memcpy(meses[j], meses[j - 1], 8);
            memcpy(meses[j - 1], tmp, 8);
            uint32_t k = kb[j];
            kb[j] = kb[j - 1];
            kb[j - 1] = k;
        }
    }
    return n;
}

bool eventos_ruta_mes(int anio, int mes, char *ruta, size_t largo)
{
    if (!registro_sd_montada() || anio < 2000 || anio > 2099 || mes < 1 || mes > 12) {
        return false;
    }
    snprintf(ruta, largo, EVENTOS_DIR "/%04d-%02d.CSV", anio, mes);
    return true;
}

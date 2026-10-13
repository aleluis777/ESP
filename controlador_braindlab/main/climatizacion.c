#include <string.h>
#include <time.h>
#include "climatizacion.h"
#include "esp_log.h"

static const char *TAG = "CLIMATIZACION";

void climatizacion_iniciar(const config_climatizacion_t *cfg, clima_estado_t *estado)
{
    memset(estado, 0, sizeof(*estado));
    estado->ciclo = CLIMA_REPOSO;
    estado->bypass_auto_habilitado = true;
    // config_braindlab_cargar_climatizacion() ya garantiza 1..N.
    estado->indice_reserva = cfg->aire_alternar - 1;
    ESP_LOGI(TAG, "%u aire(s) instalado(s)%s", cfg->cantidad_aires,
             cfg->cantidad_aires > 1 ? ", ver reserva abajo" : " (1+0, sin reserva)");
    if (cfg->cantidad_aires > 1) {
        ESP_LOGI(TAG, "Esta semana descansa AA%u (se enciende solo en alta temperatura)", cfg->aire_alternar);
    }
}

void climatizacion_semana_rotacion(const struct tm *ahora, char salida[11])
{
    // mktime() normaliza la fecha y recalcula tm_wday (no se confia en el
    // dia de semana que guarda el DS1307). Sin TZ configurada es UTC: sin
    // horario de verano que corra la hora.
    struct tm t = *ahora;
    t.tm_hour = 12; // mediodia: lejos de cualquier borde de dia al restar
    t.tm_min = 0;
    t.tm_sec = 0;
    t.tm_isdst = 0;
    mktime(&t);
    int dias_desde_martes = (t.tm_wday - 2 + 7) % 7; // tm_wday: 0=domingo, 2=martes
    t.tm_mday -= dias_desde_martes;
    mktime(&t);
    strftime(salida, 11, "%Y-%m-%d", &t);
}

uint8_t climatizacion_siguiente_alternar(uint8_t aire_actual, uint8_t cantidad_aires)
{
    if (cantidad_aires <= 1) {
        return 1;
    }
    return (aire_actual % cantidad_aires) + 1; // 1->2, ..., N->1
}

// Recalcula bypass_auto_habilitado al volver a REPOSO. A diferencia de
// neuvov2.ino (que en la transicion ALTA_TEMP->REPOSO solo apagaba el
// bypass automatico si se superaban las fallas, pero nunca lo volvia a
// habilitar solo -- y en BYPASS->REPOSO si lo hacia, una asimetria que
// parece un descuido del firmware viejo), aca se recalcula completo siempre:
// mientras las fallas de la unidad de reserva actual no superen el limite,
// el bypass automatico queda habilitado.
static void recalcular_bypass_habilitado(const config_climatizacion_t *cfg, clima_estado_t *estado)
{
    estado->bypass_auto_habilitado = estado->fails[estado->indice_reserva] <= cfg->fails_max_bypass;
}

void climatizacion_actualizar(const float temperaturas[4], const bool validas[4],
                               const config_climatizacion_t *cfg, clima_estado_t *estado)
{
    bool todas_bajo_tmin = true;
    bool alguna_sobre_tmax = false;
    bool alguna_sobre_at = false;
    bool alguna_sobre_bypass = false;
    int cuantas_validas = 0;

    for (int i = 0; i < 4; i++) {
        if (!validas[i]) continue;
        cuantas_validas++;
        if (temperaturas[i] >= cfg->temp_min) todas_bajo_tmin = false;
        if (temperaturas[i] > cfg->temp_max) alguna_sobre_tmax = true;
        if (temperaturas[i] > cfg->temp_at) alguna_sobre_at = true;
        if (temperaturas[i] > cfg->temp_bypass) alguna_sobre_bypass = true;
    }

    clima_ciclo_t ciclo_anterior = estado->ciclo;

    // Sin ningun sensor valido "todas bajo tmin" seria true por vacio:
    // se mantiene el ciclo actual en vez de apagar todo.
    if (cuantas_validas == 0) {
        todas_bajo_tmin = false;
    }

    switch (estado->ciclo) {
        case CLIMA_REPOSO:
            if (alguna_sobre_tmax) {
                estado->ciclo = CLIMA_NORMAL;
            }
            break;

        case CLIMA_NORMAL:
            if (todas_bajo_tmin) {
                estado->ciclo = CLIMA_REPOSO;
            } else if (alguna_sobre_at) {
                estado->fails[estado->indice_reserva]++;
                estado->ciclo = CLIMA_ALTA_TEMP;
            }
            break;

        case CLIMA_ALTA_TEMP:
            if (todas_bajo_tmin) {
                estado->ciclo = CLIMA_REPOSO;
            } else if (alguna_sobre_bypass && estado->bypass_auto_habilitado) {
                estado->ciclo = CLIMA_BYPASS;
            }
            break;

        case CLIMA_BYPASS:
            if (todas_bajo_tmin) {
                estado->ciclo = CLIMA_REPOSO;
            }
            break;
    }

    if (estado->ciclo != ciclo_anterior) {
        ESP_LOGI(TAG, "Transicion %d -> %d (reserva=#%u, fails=%u)", ciclo_anterior, estado->ciclo,
                 estado->indice_reserva + 1, estado->fails[estado->indice_reserva]);
        if (estado->ciclo == CLIMA_REPOSO) {
            recalcular_bypass_habilitado(cfg, estado);
        }
    }

    // Salidas: derivadas del ciclo actual, no de la transicion -- asi si
    // esta funcion se llama de nuevo sin que haya cambiado 'ciclo' (lectura
    // de sensores sin cruzar ningun umbral), el estado de salida se
    // recalcula igual y queda consistente con cfg->cantidad_aires aunque
    // este haya cambiado en caliente.
    memset(estado->salida_aire, 0, sizeof(estado->salida_aire));
    for (int i = 0; i < cfg->cantidad_aires && i < 4; i++) {
        // Con 1 solo aire no hay reserva (1+0): N-1 daria 0 encendidos.
        bool es_reserva = (cfg->cantidad_aires > 1) && (i == estado->indice_reserva);
        switch (estado->ciclo) {
            case CLIMA_REPOSO:
                estado->salida_aire[i] = false;
                break;
            case CLIMA_NORMAL:
                estado->salida_aire[i] = !es_reserva;
                break;
            case CLIMA_ALTA_TEMP:
            case CLIMA_BYPASS:
                estado->salida_aire[i] = true;
                break;
        }
    }

    estado->alarma_at = (estado->ciclo == CLIMA_ALTA_TEMP || estado->ciclo == CLIMA_BYPASS);
    estado->bypass_solicitado = (estado->ciclo == CLIMA_BYPASS);
}

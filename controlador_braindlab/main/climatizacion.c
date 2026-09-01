#include <string.h>
#include "climatizacion.h"
#include "esp_log.h"

static const char *TAG = "CLIMATIZACION";

void climatizacion_iniciar(clima_estado_t *estado)
{
    memset(estado, 0, sizeof(*estado));
    estado->ciclo = CLIMA_REPOSO;
    estado->bypass_auto_habilitado = true;
}

void climatizacion_rotar_reserva(const config_climatizacion_t *cfg, clima_estado_t *estado)
{
    if (!cfg->rotar_reserva || cfg->cantidad_aires == 0) {
        return;
    }
    estado->indice_reserva = (estado->indice_reserva + 1) % cfg->cantidad_aires;
    ESP_LOGI(TAG, "Rotacion de reserva: ahora es la unidad #%u", estado->indice_reserva + 1);
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

void climatizacion_actualizar(const float temperaturas[4], const config_climatizacion_t *cfg,
                               clima_estado_t *estado)
{
    bool todas_bajo_tmin = true;
    bool alguna_sobre_tmax = false;
    bool alguna_sobre_at = false;
    bool alguna_sobre_bypass = false;

    for (int i = 0; i < 4; i++) {
        if (temperaturas[i] >= cfg->temp_min) todas_bajo_tmin = false;
        if (temperaturas[i] > cfg->temp_max) alguna_sobre_tmax = true;
        if (temperaturas[i] > cfg->temp_at) alguna_sobre_at = true;
        if (temperaturas[i] > cfg->temp_bypass) alguna_sobre_bypass = true;
    }

    clima_ciclo_t ciclo_anterior = estado->ciclo;

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
        bool es_reserva = (i == estado->indice_reserva);
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

// Decide CUANDO se guarda un punto de una corrida (a diferencia de
// app_main.c, que lee los sensores todo el tiempo a 5Hz sin importar si hay
// que guardar algo o no). Corrida 1 guarda en tiempos fijos (el esquema
// clasico de un ensayo de consolidacion); Corrida 2 guarda cada tanto que
// avanza el Dial 2. Confirmado con el usuario que este es el comportamiento
// correcto (ver conversacion sobre labgeo2025.ino / corridaxx()).

#include "programador_corrida.h"
#include "almacenamiento.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "PROGRAMADOR";

// Esquema de tiempos para la Corrida 1 (segundos desde el inicio).
static const uint32_t k_segundos[] = {
    0, 15, 30, 60, 120, 240, 480, 900, 1800, 3600,
    7200, 14400, 28800, 57600, 86400, 86410,
};
#define N_SEGUNDOS (sizeof(k_segundos) / sizeof(k_segundos[0]))

// Esquema de deformacion para la Corrida 2 (micrometros de Dial 2 desde el inicio).
static const uint32_t k_micrometros[] = {
    50, 100, 150, 200, 300, 400, 500, 600, 700, 800, 900, 1000,
    1200, 1400, 1600, 1800, 2000, 2200, 2400, 2600, 2800, 3000, 3200, 3400,
    3600, 3800, 4000, 4200, 4400, 4600, 4800, 5000, 5500, 6000, 6500,
    7000, 7500, 8000, 8500, 9000, 9500, 10000, 10500, 11000, 11500,
};
#define N_MICROMETROS (sizeof(k_micrometros) / sizeof(k_micrometros[0]))

// Arranca el cronometro de la corrida (esp_timer_get_time da microsegundos
// desde el boot, de ahi restamos para saber cuanto paso) y reinicia el
// archivo de esa corrida en flash (almacenamiento_iniciar_corrida trunca
// cualquier dato viejo con el mismo run_id).
void programador_iniciar(programador_t *p, uint8_t run_id)
{
    p->activa = true;
    p->run_id = run_id;
    p->inicio_us = esp_timer_get_time();
    p->siguiente_checkpoint = 0;
    almacenamiento_iniciar_corrida(run_id);
    ESP_LOGI(TAG, "Corrida %u iniciada", (unsigned)run_id);
}

// Solo apaga el flag 'activa'. Los puntos ya guardados en el archivo no se
// tocan -- quedan disponibles para cuando la pantalla pida verlos
// (REQUEST_RUN) aunque la corrida ya haya terminado.
void programador_detener(programador_t *p)
{
    ESP_LOGI(TAG, "Corrida %u detenida en checkpoint %u", (unsigned)p->run_id, p->siguiente_checkpoint);
    p->activa = false;
}

// Se llama en cada vuelta del loop de sensores (~5Hz). No guarda un punto
// cada vez que se llama -- solo cuando se cumple el proximo checkpoint del
// cronograma (por eso 'toca_checkpoint').
bool programador_actualizar(programador_t *p, int32_t dial1_um, int32_t dial2_um, int32_t peso_mN,
                             uint32_t *tiempo_ms_out)
{
    // El tiempo transcurrido se calcula siempre que hay corrida activa
    // (independiente de si toca guardar un punto o no), porque tambien se
    // usa para mandarlo en el SENSOR_UPDATE de la pantalla.
    uint32_t tiempo_ms = 0;
    if (p->activa) {
        tiempo_ms = (uint32_t)((esp_timer_get_time() - p->inicio_us) / 1000);
    }
    if (tiempo_ms_out) {
        *tiempo_ms_out = tiempo_ms;
    }

    if (!p->activa) {
        return false;
    }

    // 'siguiente_checkpoint' es un indice que solo avanza para adelante: una
    // vez que se guarda el punto del checkpoint N, el N-1 nunca se vuelve a
    // chequear. Por eso alcanza con comparar contra un solo indice en vez
    // de recorrer toda la tabla cada vez.
    bool toca_checkpoint = false;

    if (p->run_id == 1) {
        // Corrida 1: dispara por tiempo transcurrido (k_segundos), sin
        // importar el valor de los diales -- se registran igual, pero no
        // deciden cuando.
        if (p->siguiente_checkpoint < N_SEGUNDOS &&
            tiempo_ms >= k_segundos[p->siguiente_checkpoint] * 1000UL) {
            toca_checkpoint = true;
        }
    } else {
        // Corrida 2: dispara cuando el Dial 2 avanzo lo suficiente desde el
        // inicio de la corrida (umbral de k_micrometros), sin importar
        // cuanto tiempo paso.
        if (p->siguiente_checkpoint < N_MICROMETROS &&
            (uint32_t)dial2_um >= k_micrometros[p->siguiente_checkpoint]) {
            toca_checkpoint = true;
        }
    }

    if (!toca_checkpoint) {
        return false;
    }

    // Guarda el punto (con los 4 valores actuales) y avanza al proximo
    // checkpoint de la tabla.
    almacenamiento_agregar_punto(p->run_id, dial1_um, dial2_um, peso_mN, tiempo_ms);
    p->siguiente_checkpoint++;

    uint16_t total = (p->run_id == 1) ? N_SEGUNDOS : N_MICROMETROS;
    if (p->siguiente_checkpoint >= total) {
        ESP_LOGI(TAG, "Corrida %u: cronograma completo (%u puntos)", (unsigned)p->run_id, total);
    }
    return true;
}

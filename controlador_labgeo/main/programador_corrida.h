#pragma once

// Reimplementa (esta vez conectada de verdad al loop -- en el .ino anterior
// la funcion equivalente `corrida()` estaba escrita pero nunca se llamaba)
// el cronograma clasico de un ensayo de consolidacion/edometro:
//
//   Corrida 1 (run_id=1): registra un punto en tiempos fijos crecientes
//   (0s, 15s, 30s, 1, 2, 4, 8, 15, 30min, 1, 2, 4, 8, 16, 24h -- el esquema
//   Casagrande/Taylor de siempre).
//
//   Corrida 2 (run_id=2): registra un punto cada vez que el Dial 2 avanza un
//   umbral de deformacion (en vez de por tiempo).
//
// Las dos tablas de checkpoints son las mismas que estaban en el .ino
// original (segundos[] / micrometros[]), solo que ahora si se usan.

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool activa;
    uint8_t run_id;          // 1 o 2
    int64_t inicio_us;       // esp_timer_get_time() al llamar a programador_iniciar()
    uint16_t siguiente_checkpoint;
} programador_t;

void programador_iniciar(programador_t *p, uint8_t run_id);
void programador_detener(programador_t *p);

// Llamar en cada ciclo de lectura de sensores con los valores actuales.
// Si toca un checkpoint, guarda el punto (almacenamiento_agregar_punto) y
// devuelve true. tiempo_ms_out siempre se completa con el tiempo transcurrido
// desde el inicio de la corrida (para mandarlo en SENSOR_UPDATE).
bool programador_actualizar(programador_t *p, int32_t dial1_um, int32_t dial2_um, int32_t peso_mN,
                             uint32_t *tiempo_ms_out);

#ifdef __cplusplus
}
#endif

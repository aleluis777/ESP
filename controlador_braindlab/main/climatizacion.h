#pragma once

// Maquina de estados de control de temperatura para N unidades de A/A.
//
// OJO: esto NO es un port 1:1 de ControlAA() en neuvov2.ino (el firmware
// anterior del mismo tipo de equipo, ver E:\ESP\neuvov2). Esa version vieja
// hacia: a temp_max encendia UNA sola unidad ("lead", rotando semanalmente
// entre 2 posiciones fijas), y a temp_at encendia las 4 de una. Lo que se
// implementa aca es la especificacion nueva que se pidio explicitamente:
//
//   - Por debajo de temp_min: las N unidades apagadas.
//   - Al llegar a temp_max: se encienden N-1 unidades (todas menos una,
//     la "de reserva").
//   - Si sigue subiendo hasta temp_at: se enciende tambien la de reserva
//     (las N encendidas) + alarma de alta temperatura.
//   - Si sigue subiendo hasta temp_bypass: se activa el bypass (puentea el
//     control, el equipo de A/A queda conectado directo).
//   - Histeresis: desde cualquier etapa encendida, se vuelve a "todo
//     apagado" al bajar de temp_min (igual que el firmware viejo: un unico
//     umbral de apagado, no uno por etapa).
//
// Decisiones que quedan como valores por defecto razonables (revisar en
// config_braindlab_defaults.h si hace falta otro comportamiento):
//   - Los 4 sensores de temperatura se evaluan siempre los 4, sin importar
//     cuantas unidades de A/A (N) esten configuradas -- igual que el
//     firmware viejo, que comparaba In[0..3] sin importar cuantos "leads"
//     hubiera.
//   - "fails" se cuenta por indice de la unidad de reserva de esa semana
//     cada vez que se escala a ALTA_TEMP -- generaliza failsArray[lead-1]
//     del firmware viejo a N unidades.
//   - La unidad de reserva rota semanalmente para repartir desgaste: la
//     elige aire_alternar de config.json, y tarea_climatizacion (app_main.c)
//     la avanza cada lunes->martes y reinicia el equipo. Aca solo se usa
//     indice_reserva, que el que llama carga al arrancar.
//   - Con 1 solo aire (1+0) no hay reserva: se enciende en la etapa normal
//     y en alta temperatura no hay nada mas que encender (solo la alarma).

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "config_braindlab.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLIMA_REPOSO = 0,    // todo apagado
    CLIMA_NORMAL,        // N-1 unidades encendidas
    CLIMA_ALTA_TEMP,     // las N unidades encendidas + alarma AT
    CLIMA_BYPASS,        // bypass solicitado
} clima_ciclo_t;

typedef struct {
    clima_ciclo_t ciclo;
    bool          salida_aire[4];        // estado deseado de AA1..AA4 (indices 0..3)
    bool          alarma_at;
    bool          bypass_solicitado;
    uint8_t       indice_reserva;        // 0..N-1, cual de las N es la "de reserva" esta semana
    uint8_t       fails[4];              // fallas acumuladas por indice de reserva (ver .h arriba)
    bool          bypass_auto_habilitado; // false = se supero fails_max_bypass, no se vuelve a pedir bypass solo
} clima_estado_t;

// Deja 'estado' en CLIMA_REPOSO, todo apagado, fails en 0,
// bypass_auto_habilitado=true e indice_reserva = cfg->aire_alternar - 1 (el
// que descansa esta semana). Llamar una vez al arrancar.
void climatizacion_iniciar(const config_climatizacion_t *cfg, clima_estado_t *estado);

// Recalcula 'estado' en base a las 4 lecturas de temperatura (indices 0..3 =
// T1..T4) y la config vigente. Para llamar cada vez que hay lecturas nuevas
// de sensores (no tiene noscion de tiempo propia, no hace falta llamarla a
// una frecuencia fija).
void climatizacion_actualizar(const float temperaturas[4], const config_climatizacion_t *cfg,
                               clima_estado_t *estado);

// Semana de rotacion a la que pertenece 'ahora': la fecha "AAAA-MM-DD" del
// ultimo martes 00:00 (la rotacion es de lunes a martes). Dos instantes de
// la misma semana dan el mismo texto; al pasar de lunes a martes cambia.
// Las fechas en ese formato se pueden comparar con strcmp().
void climatizacion_semana_rotacion(const struct tm *ahora, char salida[11]);

// Aire que descansa la semana siguiente: 1..N, despues de N vuelve a 1.
uint8_t climatizacion_siguiente_alternar(uint8_t aire_actual, uint8_t cantidad_aires);

#ifdef __cplusplus
}
#endif

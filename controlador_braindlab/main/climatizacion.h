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
//   - La unidad de reserva puede rotar semanalmente (climatizacion_rotar_reserva)
//     para repartir desgaste, generalizando la rotacion de "lead" del
//     firmware viejo (que solo rotaba entre 2 posiciones).

#include <stdbool.h>
#include <stdint.h>
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

// Deja 'estado' en CLIMA_REPOSO, todo apagado, indice_reserva=0, fails en 0
// y bypass_auto_habilitado=true. Llamar una vez al arrancar (o al limpiar
// fallas manualmente, equivalente al boton=2 de /api/restart en neuvov2.ino).
void climatizacion_iniciar(clima_estado_t *estado);

// Recalcula 'estado' en base a las 4 lecturas de temperatura (indices 0..3 =
// T1..T4) y la config vigente. Para llamar cada vez que hay lecturas nuevas
// de sensores (no tiene noscion de tiempo propia, no hace falta llamarla a
// una frecuencia fija).
void climatizacion_actualizar(const float temperaturas[4], const config_climatizacion_t *cfg,
                               clima_estado_t *estado);

// Avanza indice_reserva a la siguiente unidad (0..cantidad_aires-1, con
// wraparound) si cfg->rotar_reserva esta activo. Para llamar desde el punto
// donde se decida el limite de "semana" (por ejemplo comparando dia de
// semana del RTC, igual que el semana/semana_last de neuvov2.ino) -- este
// modulo no sabe nada de RTC ni de tiempo real a proposito, para que se
// pueda probar sin hardware.
void climatizacion_rotar_reserva(const config_climatizacion_t *cfg, clima_estado_t *estado);

#ifdef __cplusplus
}
#endif

#pragma once

// Valores por defecto -- se usan campo por campo cuando config.json no
// existe todavia, esta corrupto, o le falta ese campo puntual. Igual
// criterio que config_labgeo_defaults.h en controlador_labgeo.

// ---------------------------------------------------------------------------
// Climatizacion
// ---------------------------------------------------------------------------

// Heredados literal de neuvov2.ino / data/config.json (el firmware anterior,
// real, de este mismo tipo de equipo -- ver E:\ESP\neuvov2). OJO: temp_at y
// temp_bypass (80/82) laman la atencion por ser demasiado altos para una
// temperatura ambiente real; se mantienen igual que el archivo original
// encontrado porque no hay forma de confirmar si eran valores de prueba o
// del sitio real ("Equipo Chiclayo2"). CONFIRMAR antes de usar en un
// despliegue real -- no asumir que estos 4 numeros son correctos.
#define TEMP_MIN_DEFAULT    (25.00f)  // por debajo de esto, apaga todo (histeresis)
#define TEMP_MAX_DEFAULT    (28.00f)  // activa la etapa "normal" (N-1 unidades)
#define TEMP_AT_DEFAULT     (80.00f)  // activa la etapa "alta temperatura" (las N unidades + alarma)
#define TEMP_BYPASS_DEFAULT (82.00f)  // activa el bypass

// Cuantas unidades de A/A maneja este equipo (1-4, el hardware tiene 4
// canales de rele -- ver HARDWARE.md §5.1). Default = 4 (usa toda la
// capacidad de la placa); en el sitio original de neuvov2 esto era
// efectivamente 2, configurable por sitio en config.json.
#define CANTIDAD_AIRES_DEFAULT (4)

// Umbral de fallas (veces que se escalo a "alta temperatura") a partir del
// cual se deshabilita el bypass automatico -- mismo criterio que
// failsArray[...]>3 en neuvov2.ino.
#define FAILS_MAX_BYPASS_DEFAULT (3)

// Si la unidad "de reserva" (la que se guarda para la etapa de alta
// temperatura) rota semanalmente entre las N configuradas, para repartir el
// desgaste -- mismo espiritu que la rotacion de "lead" en neuvov2.ino,
// generalizada a N unidades en vez de solo 2.
#define ROTAR_RESERVA_DEFAULT (true)

// ---------------------------------------------------------------------------
// Red (mismo campo que config_labgeo_defaults.h -- placeholder hasta que se
// aplique de verdad al W5500)
// ---------------------------------------------------------------------------

#define RED_IP_DEFAULT      "192.168.5.95"
#define RED_GATEWAY_DEFAULT "192.168.5.1"
#define RED_MASCARA_DEFAULT "255.255.255.0"

// ---------------------------------------------------------------------------
// Calibracion (ver config_calibracion_t) -- todas arrancan en 0.0 (sin
// corregir) hasta que se calibre a mano contra un instrumento de referencia.
// ---------------------------------------------------------------------------

#define CALIBRACION_DEFAULT (0.0f)

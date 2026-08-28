#pragma once

// Valores por defecto de calibracion/configuracion -- se usan campo por
// campo cuando config.json no existe todavia, esta corrupto, o le falta ese
// campo puntual. Pensados para tocarlos aca (recompilar) en vez de editar
// el JSON a mano cada vez que se quiere cambiar el default de fabrica.

#define CELDA_OFFSET_DEFAULT    (0.0f)
#define CELDA_PENDIENTE_DEFAULT (1.0f)

#define DIAL1_OFFSET_DEFAULT    (0.0f)
#define DIAL1_PENDIENTE_DEFAULT (1.0f)

#define DIAL2_OFFSET_DEFAULT    (0.0f)
#define DIAL2_PENDIENTE_DEFAULT (1.0f)

// Misma IP/gateway/mascara que usa hoy red_eth.c hardcodeado -- estos son
// solo los defaults del archivo; aplicarlos de verdad al W5500 es el paso 3
// (todavia no implementado, ver conversacion).
#define RED_IP_DEFAULT      "192.168.5.91"
#define RED_GATEWAY_DEFAULT "192.168.5.1"
#define RED_MASCARA_DEFAULT "255.255.255.0"

// Datos generales del equipo (nombre, diametro de la probeta en mm, unidad
// de peso a mostrar) -- se guardan en sistema.json, igual que "red". El
// diametro lo usa grafica.html (web/grafica.html) para calcular area y el
// desplazamiento de referencia del grafico "versus" de la Corrida 2.
#define EQUIPO_NOMBRE_DEFAULT   ""
#define EQUIPO_DIAMETRO_DEFAULT (0.0f)
#define EQUIPO_UNIDAD_DEFAULT   "Kg"

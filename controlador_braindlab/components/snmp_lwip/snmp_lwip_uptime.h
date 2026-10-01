#pragma once

// Se inyecta con "-include" SOLO al compilar los .c del agente de lwIP (ver
// CMakeLists.txt de este componente). Sin MIB2_STATS, lwip/snmp.h deja
// MIB2_COPY_SYSUPTIME_TO() vacio y los traps salen con sysUpTime basura; y
// la definicion que trae lwIP para el caso con MIB2_STATS usa sys_now() (ms
// en 32 bits), que da la vuelta a los 49 dias -- un gestor SNMP interpreta
// un sysUpTime que baja como un reinicio del equipo. esp_timer es de 64
// bits: en centesimas (TimeTicks) recien da la vuelta a los ~497 dias, que
// es el limite propio del tipo TimeTicks de SNMP.
//
// snmp_braindlab.c usa la misma cuenta para sysUpTime.0 (GET), asi los
// traps y el GET coinciden.

#include <stdint.h>
#include "esp_timer.h"

#define MIB2_COPY_SYSUPTIME_TO(p) (*(p) = (uint32_t)(esp_timer_get_time() / 10000))

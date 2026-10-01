# Componente "puente" hacia el agente SNMP que ya trae lwIP dentro de
# ESP-IDF (components/lwip/lwip/src/apps/snmp). ESP-IDF no lo compila por
# defecto -- aca solo se apuntan sus .c tal cual, NO se copia ni se modifica
# nada de la libreria. Ver snmp_braindlab.c (main/) para la MIB propia.
#
# Solo SNMP v1/v2c: se dejan afuera snmpv3*.c, snmp_snmpv2_*.c (USM, solo
# para v3), snmp_raw.c (usamos netconn, ver abajo) y todos los
# snmp_mib2*.c (MIB-2 estandar de lwIP: exige LWIP_STATS+MIB2_STATS en el
# NUCLEO de lwIP, que se compila aparte con el lwipopts.h de ESP-IDF -- no
# se puede prender desde aca. El grupo "system" de MIB-2 lo arma
# snmp_braindlab.c a mano).

idf_build_get_property(idf_path IDF_PATH)
set(snmp_dir "${idf_path}/components/lwip/lwip/src/apps/snmp")

idf_component_register(SRCS "${snmp_dir}/snmp_asn1.c"
                            "${snmp_dir}/snmp_core.c"
                            "${snmp_dir}/snmp_msg.c"
                            "${snmp_dir}/snmp_netconn.c"
                            "${snmp_dir}/snmp_pbuf_stream.c"
                            "${snmp_dir}/snmp_scalar.c"
                            "${snmp_dir}/snmp_table.c"
                            "${snmp_dir}/snmp_threadsync.c"
                            "${snmp_dir}/snmp_traps.c"
                       PRIV_INCLUDE_DIRS "${snmp_dir}"
                       REQUIRES lwip
                       PRIV_REQUIRES esp_timer)

# PUBLIC: los que incluyan "lwip/apps/snmp.h" (main/snmp_braindlab.c) tienen
# que ver exactamente las mismas opciones con las que se compilo el agente.
# Ninguna de estas afecta al nucleo de lwIP (LWIP_SNMP no cambia ninguna
# estructura de lwip/*.h), por eso alcanza con definirlas solo aca.
#   SNMP_USE_NETCONN=1/SNMP_USE_RAW=0 -- el agente corre en SU PROPIA tarea
#     ("snmp_netconn"), no dentro de la tarea TCP/IP: un callback lento de
#     la MIB (ej. guardar config.json en SPIFFS) no congela el resto de la
#     red (web, WS).
#   SNMP_TRAP_DESTINATIONS=2 -- dos gestores para recibir traps.
#   SNMP_MAX_COMMUNITY_STR_LEN=31 -- el default se calcula del largo de
#     "public"/"private" y una community mas larga dispara un LWIP_ASSERT.
#   SNMP_STACK_SIZE=6144 -- los SET corren en la tarea del agente y algunos
#     terminan guardando config.json (cJSON + SPIFFS), no alcanza con 4K.
#   SNMP_THREAD_PRIO=3 -- por debajo de tarea_climatizacion (5): una rafaga
#     de GETBULK nunca le quita CPU al control.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    LWIP_SNMP=1
    SNMP_USE_NETCONN=1
    SNMP_USE_RAW=0
    SNMP_LWIP_MIB2=0
    SNMP_TRAP_DESTINATIONS=2
    SNMP_MAX_COMMUNITY_STR_LEN=31
    SNMP_STACK_SIZE=6144
    SNMP_THREAD_PRIO=3)

# sysUpTime de los traps -- ver snmp_lwip_uptime.h.
# -Wno-dangling-pointer: falso positivo de GCC 14 en snmp_traps.c (codigo de
# lwIP, no nuestro): guarda un puntero a un array local en varbinds->prev y
# lo restaura antes de salir de la funcion. Con -Werror (default de ESP-IDF)
# corta el build. Solo afecta a los .c de este componente.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -include "${CMAKE_CURRENT_LIST_DIR}/snmp_lwip_uptime.h"
    -Wno-dangling-pointer)

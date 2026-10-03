#pragma once

// Definiciones del protocolo binario por UART entre esta placa
// (controlador_braindlab, ESP32) y la pantalla tactil braindlab (ESP32-S3).
// Mismo esquema que protocolo_labgeo.h (controlador_labgeo <-> blink): SOF +
// CMD + LEN + PAYLOAD + CRC8, sin ACK ni reintentos. Ver
// PROTOCOLO_UART_BRAINDLAB.md para la especificacion completa.
//
// Este archivo tiene que ser IDENTICO en los dos proyectos (aca y en
// braindlab/main/protocolo_braindlab.h) -- si se edita uno, copiar el mismo
// cambio al otro.

#include <stdint.h>

#define BRAINDLAB_SOF         0xAA
#define BRAINDLAB_MAX_PAYLOAD 255 // limite de payload por trama (LEN <= 255) en esta v1

typedef enum {
    // Controlador -> Pantalla
    BRAINDLAB_CMD_ESTADO_UPDATE     = 0x01, // estado completo (climatizacion + sensores + red + hora), periodico

    // Pantalla -> Controlador (mismo pedido que hacen /control_aire,
    // /control_bypass, /control_at, /control_automatico de servidor_web.c --
    // la pantalla es un cliente mas del mismo mecanismo de override manual)
    BRAINDLAB_CMD_CONTROL_AIRE       = 0x10, // forzar AA(indice+1) a mano
    BRAINDLAB_CMD_CONTROL_BYPASS     = 0x11, // forzar bypass_solicitado a mano
    BRAINDLAB_CMD_CONTROL_AT         = 0x12, // forzar OUT_AT a mano
    BRAINDLAB_CMD_CONTROL_AUTOMATICO = 0x13, // cancelar todos los overrides manuales
} braindlab_cmd_t;

// CRC-8 (poly 0x07, init 0x00), calculado sobre CMD + LEN(2B) + PAYLOAD
// (todo menos el byte SOF y el propio CRC). Debe dar el mismo resultado en
// ambos extremos del enlace.
static inline uint8_t braindlab_crc8(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0x00;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// ---------- Helpers little-endian para armar/leer payloads ----------

static inline void braindlab_put_u8(uint8_t *buf, uint16_t *idx, uint8_t v)
{
    buf[(*idx)++] = v;
}

static inline void braindlab_put_i16(uint8_t *buf, uint16_t *idx, int16_t v)
{
    uint16_t u = (uint16_t)v;
    buf[(*idx)++] = (uint8_t)(u & 0xFF);
    buf[(*idx)++] = (uint8_t)((u >> 8) & 0xFF);
}

static inline void braindlab_put_u16(uint8_t *buf, uint16_t *idx, uint16_t v)
{
    buf[(*idx)++] = (uint8_t)(v & 0xFF);
    buf[(*idx)++] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void braindlab_put_u32(uint8_t *buf, uint16_t *idx, uint32_t v)
{
    buf[(*idx)++] = (uint8_t)(v & 0xFF);
    buf[(*idx)++] = (uint8_t)((v >> 8) & 0xFF);
    buf[(*idx)++] = (uint8_t)((v >> 16) & 0xFF);
    buf[(*idx)++] = (uint8_t)((v >> 24) & 0xFF);
}

static inline int16_t braindlab_leer_i16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint16_t braindlab_leer_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t braindlab_leer_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ---------- Payload de BRAINDLAB_CMD_ESTADO_UPDATE (50 bytes) ----------
//
// Todos los campos son los mismos que ya se mandan por WebSocket (ver
// publicar_estado_ws() en app_main.c) -- la pantalla es, en los hechos, otro
// cliente mas del mismo estado, solo que por UART en vez de WS/JSON.
// Temperaturas y humedad van como i16 en DECIMAS de unidad (235 = 23.5),
// para no mandar floats crudos por el cable.
//
// Offset  Tamano  Campo              Tipo
// 0       1       ciclo              u8   (0=REPOSO,1=NORMAL,2=ALTA_TEMP,3=BYPASS)
// 1       8       temperaturas[4]    i16 x4, decimas de grado C (T1..T4)
// 9       1       salida_aire        u8   bitmask, bit i = AA(i+1) (0..3)
// 10      1       aire_manual        u8   bitmask, bit i = AA(i+1) forzado a mano
// 11      1       indice_reserva     u8   0..3
// 12      1       alarma_at          u8   bool
// 13      1       alarma_at_manual   u8   bool
// 14      1       bypass_solicitado  u8   bool
// 15      1       bypass_manual      u8   bool
// 16      1       bypass_activo      u8   bool (feedback fisico, BPS_STATUS)
// 17      1       modo_manual        u8   bool (algun override activo)
// 18      1       eth_conectado      u8   bool
// 19      1       gestor_ok          u8   bool (ultima lectura del AM2301A OK)
// 20      2       temp_gestor        i16  decimas de grado C
// 22      2       humedad_gestor     i16  decimas de %HR
// 24      4       uptime_s           u32  segundos desde que arranco el equipo
// 28      2       anio               u16  (ej 2026)
// 30      1       mes                u8   1..12
// 31      1       dia                u8   1..31
// 32      1       hora               u8   0..23
// 33      1       minuto             u8   0..59
// 34      1       segundo            u8   0..59
// 35      1       energia_ok         u8   bool (medidor RS-485 respondiendo)
// 36      6       voltajes[3]        u16 x3, decimas de V (R, S, T)
// 42      6       corrientes[3]      u16 x3, centesimas de A (R, S, T)
// 48      1       sd_estado          u8   0=OK, 1=sin tarjeta, 2=sin formato
//                                         (no es FAT32), 3=error de escritura,
//                                         4=formateando
// 49      1       fallas             u8   bitmask, bit en 1 = modulo con falla
//                                         AHORA (ver BRAINDLAB_FALLA_*)
//
// anio=0 (mes=dia=hora=minuto=segundo=0) significa "RTC no disponible",
// equivalente al fecha_hora="----" del JSON del WS. energia_ok=0 significa
// que el medidor no responde: ignorar voltajes/corrientes y mostrar "--".
//
// Los campos de energia (offset 35+), sd_estado (48) y fallas (49) se agregaron al final
// a proposito: una pantalla con firmware viejo (que espera 35 bytes) sigue
// funcionando, solo ignora lo que sobra. BRAINDLAB_ESTADO_UPDATE_LEN_V1 es
// ese minimo viejo; cada campo nuevo se lee solo si LEN lo alcanza.
#define BRAINDLAB_ESTADO_UPDATE_LEN_V1 35
#define BRAINDLAB_ESTADO_UPDATE_LEN    50

#define BRAINDLAB_SD_OK              0
#define BRAINDLAB_SD_SIN_TARJETA     1
#define BRAINDLAB_SD_SIN_FORMATO     2
#define BRAINDLAB_SD_ERROR_ESCRITURA 3
#define BRAINDLAB_SD_FORMATEANDO     4

// Bits de 'fallas' (offset 49): estado en tiempo real de cada modulo, el
// mismo que publica el WS (rtc_ok, adc_ok, gestor_ok, eth_conectado,
// energia_ok, sd_estado). 0 = todo OK.
#define BRAINDLAB_FALLA_RTC      (1 << 0) // DS1307 no responde
#define BRAINDLAB_FALLA_ADC      (1 << 1) // ADS1115 (NTC T1-T4) no responde
#define BRAINDLAB_FALLA_GESTOR   (1 << 2) // AM2301A (temp/HR gabinete) fallo la ultima lectura
#define BRAINDLAB_FALLA_ETH      (1 << 3) // Ethernet sin link
#define BRAINDLAB_FALLA_MODBUS   (1 << 4) // medidor JSY-MK-333G sin respuesta
#define BRAINDLAB_FALLA_SD       (1 << 5) // MicroSD != OK (detalle en sd_estado)

// ---------- Payloads de los comandos Pantalla -> Controlador ----------
//
// CONTROL_AIRE: 2 bytes, indice(u8, 0..3) + encendido(u8 bool)
// CONTROL_BYPASS: 1 byte, solicitado(u8 bool)
// CONTROL_AT: 1 byte, activa(u8 bool)
// CONTROL_AUTOMATICO: 0 bytes

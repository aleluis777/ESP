#pragma once

// Definiciones del protocolo binario por UART entre esta pantalla (ESP32-S3)
// y el controlador de sensores (ESP32 comun). Ver PROTOCOLO_UART.md para la
// especificacion completa (formato de trama, unidades, ejemplos).

#include <stdint.h>

#define LABGEO_SOF          0xAA
#define LABGEO_MAX_PAYLOAD  255      // limite de payload por trama (LEN <= 255) en esta v1
#define LABGEO_CHUNK_MAX_PUNTOS 16   // puntos maximos por trama RUN_CHUNK (3 + 16*12 = 195 bytes)

typedef enum {
    // Controlador -> Pantalla
    LABGEO_CMD_SENSOR_UPDATE = 0x01, // lectura en vivo (hasta 5 veces/seg)
    LABGEO_CMD_RUN_CHUNK     = 0x02, // datos guardados de una corrida (bajo pedido)

    // Pantalla -> Controlador
    LABGEO_CMD_START         = 0x10, // inicia una corrida (el controlador maneja el tiempo)
    LABGEO_CMD_STOP          = 0x11, // detiene la corrida activa
    LABGEO_CMD_REQUEST_RUN   = 0x12, // pide los datos guardados de run_id (1 o 2)
} labgeo_cmd_t;

// CRC-8 (poly 0x07, init 0x00), calculado sobre CMD + LEN(2B) + PAYLOAD
// (todo menos el byte SOF y el propio CRC). Debe dar el mismo resultado en
// ambos extremos del enlace.
static inline uint8_t labgeo_crc8(const uint8_t *data, uint16_t len)
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

static inline void labgeo_put_u8(uint8_t *buf, uint16_t *idx, uint8_t v)
{
    buf[(*idx)++] = v;
}

static inline void labgeo_put_i32(uint8_t *buf, uint16_t *idx, int32_t v)
{
    uint32_t u = (uint32_t)v;
    buf[(*idx)++] = (uint8_t)(u & 0xFF);
    buf[(*idx)++] = (uint8_t)((u >> 8) & 0xFF);
    buf[(*idx)++] = (uint8_t)((u >> 16) & 0xFF);
    buf[(*idx)++] = (uint8_t)((u >> 24) & 0xFF);
}

static inline void labgeo_put_u32(uint8_t *buf, uint16_t *idx, uint32_t v)
{
    buf[(*idx)++] = (uint8_t)(v & 0xFF);
    buf[(*idx)++] = (uint8_t)((v >> 8) & 0xFF);
    buf[(*idx)++] = (uint8_t)((v >> 16) & 0xFF);
    buf[(*idx)++] = (uint8_t)((v >> 24) & 0xFF);
}

static inline int32_t labgeo_leer_i32(const uint8_t *p)
{
    return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static inline uint32_t labgeo_leer_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

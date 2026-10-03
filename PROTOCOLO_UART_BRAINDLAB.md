# Protocolo UART — Pantalla braindlab (ESP32-S3) <-> Controlador de climatizacion (ESP32)

Mismo esquema que `PROTOCOLO_UART.md` (controlador_labgeo <-> blink): SOF +
CMD + LEN + PAYLOAD + CRC8, sin ACK ni reintentos. Implementado en:
`controlador_braindlab/main/protocolo_braindlab.h`, `uart_pantalla.c/.h` (lado
controlador) y `braindlab/main/protocolo_braindlab.h`, `uart_braindlab.c/.h`
(lado pantalla). El archivo `protocolo_braindlab.h` es **identico** en los
dos proyectos.

## 1. Capa fisica

- UART simple, full-duplex, asincrono. **8N1**, sin control de flujo.
  **Baudrate: 115200**.
- TX de un lado va al RX del otro (cruzados), GND comun entre las dos placas.
- **Controlador** (`controlador_braindlab`, `UART_NUM_1`): **TX = GPIO26**
  (P6), **RX = GPIO39** (P1). GPIO25 (originalmente reservado para esto en
  HARDWARE.md §14) quedo libre para el IRQ del W5500 en su lugar -- ver
  conversacion de reparto de pines.
- **Pantalla** (`braindlab`, `UART_NUM_1`, mismo hardware que `blink`):
  **TX = GPIO17, RX = GPIO18**.

## 2. Formato de trama (identico a PROTOCOLO_UART.md)

```
Offset  Tamano  Campo
0       1       SOF        = 0xAA
1       1       CMD
2       2       LEN        uint16 little-endian, longitud de PAYLOAD (0-255)
4       LEN     PAYLOAD    especifico de cada CMD
4+LEN   1       CRC8       poly 0x07, init 0x00, sobre CMD+LEN(2B)+PAYLOAD
```

Sin byte-stuffing, sin ACK/reintentos -- una trama con CRC invalido se
descarta y se pierde.

## 3. Comandos

### Controlador -> Pantalla

| CMD    | Nombre         | Cuando se manda |
|--------|----------------|------------------|
| `0x01` | ESTADO_UPDATE  | Periodico, una vez por vuelta de `tarea_climatizacion` (~cada 2s). |

### Pantalla -> Controlador

La pantalla es un cliente mas del mismo mecanismo de override manual que ya
atienden `/control_aire`, `/control_bypass`, `/control_at`,
`/control_automatico` en `servidor_web.c` -- estos comandos llaman
literalmente los mismos callbacks de `app_main.c` que esos endpoints HTTP.

| CMD    | Nombre              | Cuando se manda |
|--------|---------------------|------------------|
| `0x10` | CONTROL_AIRE        | El usuario toca el boton ON/OFF de una tarjeta de aire. |
| `0x11` | CONTROL_BYPASS      | (sin widget propio en el dashboard todavia) |
| `0x12` | CONTROL_AT          | (sin widget propio en el dashboard todavia) |
| `0x13` | CONTROL_AUTOMATICO  | (sin widget propio en el dashboard todavia) |

---

### `0x01` ESTADO_UPDATE (Controlador -> Pantalla)

Mismos campos que ya manda `publicar_estado_ws()` por WebSocket (ver
`servidor_web.c`/`app_main.c` de `controlador_braindlab`) -- la pantalla es,
en los hechos, otro cliente mas del mismo estado, solo que por UART en vez
de WS/JSON. Temperaturas y humedad van como `i16` en **decimas** de unidad
(235 = 23.5), para no mandar floats crudos por el cable.

| Offset | Tamano | Campo              | Tipo | Notas |
|--------|--------|---------------------|------|-------|
| 0      | 1      | ciclo               | u8   | 0=REPOSO,1=NORMAL,2=ALTA_TEMP,3=BYPASS (ver `climatizacion.h`) |
| 1      | 8      | temperaturas[4]     | i16 x4 | T1..T4, decimas de °C |
| 9      | 1      | salida_aire         | u8   | bitmask, bit i = AA(i+1) (0..3) |
| 10     | 1      | aire_manual         | u8   | bitmask, bit i = AA(i+1) forzado a mano |
| 11     | 1      | indice_reserva      | u8   | 0..3 |
| 12     | 1      | alarma_at           | u8   | bool |
| 13     | 1      | alarma_at_manual    | u8   | bool |
| 14     | 1      | bypass_solicitado   | u8   | bool |
| 15     | 1      | bypass_manual       | u8   | bool |
| 16     | 1      | bypass_activo       | u8   | bool (feedback fisico, BPS_STATUS) |
| 17     | 1      | modo_manual         | u8   | bool (algun override activo) |
| 18     | 1      | eth_conectado       | u8   | bool |
| 19     | 1      | gestor_ok           | u8   | bool (ultima lectura del AM2301A OK) |
| 20     | 2      | temp_gestor         | i16  | decimas de °C |
| 22     | 2      | humedad_gestor      | i16  | decimas de %HR |
| 24     | 4      | uptime_s            | u32  | segundos desde que arranco el equipo |
| 28     | 2      | anio                | u16  | ej 2026 |
| 30     | 1      | mes                 | u8   | 1..12 |
| 31     | 1      | dia                 | u8   | 1..31 |
| 32     | 1      | hora                | u8   | 0..23 |
| 33     | 1      | minuto              | u8   | 0..59 |
| 34     | 1      | segundo             | u8   | 0..59 |
| 35     | 1      | energia_ok          | u8   | bool, medidor RS-485 (JSY-MK-333G) respondiendo |
| 36     | 6      | voltajes[3]         | u16 x3 | decimas de V, fases R, S, T |
| 42     | 6      | corrientes[3]       | u16 x3 | centesimas de A, fases R, S, T |
| 48     | 1      | sd_estado           | u8   | MicroSD: 0=OK, 1=sin tarjeta, 2=sin formato (no FAT32), 3=error de escritura, 4=formateando |
| 49     | 1      | fallas              | u8   | bitmask, 1 = modulo con falla ahora: bit0 RTC, bit1 ADC (ADS1115), bit2 AM2301A, bit3 Ethernet sin link, bit4 medidor Modbus, bit5 SD (detalle en `sd_estado`) |

Payload total: **50 bytes**. `anio=0` (con mes/dia/hora/minuto/segundo en 0)
significa "RTC no disponible", equivalente a `fecha_hora="----"` en el JSON
del WS. `energia_ok=0` significa que el medidor no responde: ignorar
voltajes/corrientes y mostrar "--".

**Compatibilidad:** los campos de energia (offset 35 en adelante) se
agregaron al final. Una pantalla con firmware viejo acepta cualquier
`LEN >= 35` e ignora lo que sobra; la pantalla nueva lee la energia solo si
`LEN >= 48`, `sd_estado` solo si `LEN >= 49` y `fallas` solo si `LEN >= 50`
(`BRAINDLAB_ESTADO_UPDATE_LEN_V1` = 35, `BRAINDLAB_ESTADO_UPDATE_LEN` = 50
en `protocolo_braindlab.h`).

**Aplicado hoy en el dashboard** (`uart_braindlab.c`): `salida_aire` ->
encendido/apagado + animacion de viento de cada tarjeta de aire;
`temperaturas` -> las 4 tarjetas de "SENSORES AMBIENTALES";
`humedad_gestor` -> tarjeta de "HUMEDAD RELATIVA"; `alarma_at`/`ciclo` ->
LED + texto de "Sistema OK/Bypass Activo/Alta Temperatura" y las filas de
"ALARMAS ACTIVAS" (se muestran solo si `alarma_at`/`bypass_activo` estan
realmente activos); `anio..segundo` -> hora/fecha de la barra superior;
`energia_ok`/`voltajes`/`corrientes` -> vista "PARAMETROS ELECTRICOS"
(`ui_energia.c`, 6 tarjetas: Voltaje R/S/T y Corriente R/S/T);
`sd_estado` != 0 -> fila "SD" en "ALARMAS ACTIVAS" con el motivo;
`fallas` -> fila "MOD" con la lista de modulos caidos. Ademas la pantalla
muestra la fila "COM" si no recibe un `ESTADO_UPDATE` valido en 6 s (el
controlador no puede saber si la pantalla esta, el protocolo no tiene ACK).
`temp_gestor`, `indice_reserva`, `modo_manual`, `eth_conectado`, `uptime_s`
llegan pero todavia no tienen un widget propio en el dashboard.

---

### `0x10` CONTROL_AIRE (Pantalla -> Controlador)

Payload: 2 bytes, `indice` (u8, 0-3) + `encendido` (u8 bool).

Fuerza a mano AA(indice+1). Persiste del lado del controlador hasta que la
automatica cruce a otro `ciclo` (mismo criterio que `/control_aire` HTTP,
ver `app_main.c` de `controlador_braindlab`).

### `0x11` CONTROL_BYPASS (Pantalla -> Controlador)

Payload: 1 byte, `solicitado` (u8 bool). Fuerza `bypass_solicitado` (BP_S).

### `0x12` CONTROL_AT (Pantalla -> Controlador)

Payload: 1 byte, `activa` (u8 bool). Fuerza `OUT_AT` (la alarma).

### `0x13` CONTROL_AUTOMATICO (Pantalla -> Controlador)

Sin payload. Cancela cualquier override manual vigente (AA1-4, bypass, AT).

## 4. Diferencias con PROTOCOLO_UART.md (labgeo/blink)

- Sin comandos de "corrida" (`START`/`STOP`/`REQUEST_RUN`/`RUN_CHUNK`): la
  climatizacion no tiene el concepto de corridas con inicio/fin, es control
  continuo. El unico comando periodico es `ESTADO_UPDATE`.
- Los comandos de control manual (`CONTROL_*`) no tienen equivalente en
  labgeo/blink -- son propios de este protocolo, calcados de los endpoints
  HTTP que ya existian (`/control_aire`, etc.).
- `CONTROL_BYPASS`/`CONTROL_AT`/`CONTROL_AUTOMATICO` estan implementados en
  el firmware de los dos lados (el controlador los atiende, la pantalla
  tiene las funciones `uart_braindlab_enviar_control_*()` listas para
  llamar) pero **todavia no tienen un boton propio en el dashboard** -- el
  unico control manual disparado desde la pantalla hoy es
  `CONTROL_AIRE` (boton ON/OFF de cada tarjeta de aire).

## 5. Pendiente

- Widgets en el dashboard para forzar bypass/AT a mano y para "volver a
  automatico" (equivalentes a los botones ya agregados en `configurar.html`
  del lado web).
- Mostrar `temp_gestor` en algun lado del dashboard (hoy solo se aplica
  `humedad_gestor`, no hay una tarjeta de "T. Gestor" dedicada).
- Confirmar el pinout fisico real una vez que ambas placas esten cableadas
  entre si (TX/RX cruzados + GND comun).

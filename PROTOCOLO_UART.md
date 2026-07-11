# Protocolo UART — Pantalla LabGeo (ESP32-S3) <-> Controlador de sensores (ESP32)

Este documento describe el protocolo binario que la pantalla (ESP32-S3, solo
vista — no procesa ni guarda datos) usa para hablar con el controlador de
sensores (ESP32 comun, con Ethernet, que lee los diales y la celda de carga y
maneja el tiempo de cada corrida). Implementado en el lado de la pantalla en:
`blink/blink/main/protocolo_labgeo.h`, `uart_labgeo.c/.h`.

## 1. Capa fisica

- **UART** simple (no SPI): full-duplex, asincrono, cada lado puede enviar
  cuando quiera sin coordinarse con un maestro. Con los volumenes de datos de
  este proyecto (unos 100-200 bytes/seg en uso normal) sobra por mucho.
- **8N1**, sin control de flujo.
- **Baudrate: 115200** (subir a 921600 si en el futuro se necesita bajar la
  latencia de la descarga completa de una corrida; ambos lados deben usar el
  mismo valor).
- TX de un lado va al RX del otro (cruzados), y GND comun entre ambas placas.
- Pines confirmados en la pantalla: `UART_NUM_1`, **TX = IO17**, **RX = IO18**.
  Del lado del controlador, conectar su RX al IO17 de la pantalla y su TX al
  IO18 de la pantalla (mas GND comun).

## 2. Formato de trama

```
Offset  Tamano  Campo
0       1       SOF        = 0xAA (byte de sincronismo)
1       1       CMD        (ver tabla de comandos)
2       2       LEN        uint16, little-endian. Longitud de PAYLOAD en bytes (0-255 en esta v1)
4       LEN     PAYLOAD    especifico de cada CMD
4+LEN   1       CRC8       ver formula abajo
```

- Todos los campos multi-byte son **little-endian**.
- `LEN` va en 2 bytes por si a futuro se necesitan payloads mas grandes, pero
  en esta v1 **no se debe superar 255 bytes de payload** (asi el buffer de
  recepcion es simple de dimensionar en ambos lados).
- No hay byte-stuffing/escapado: como el receptor sabe exactamente cuantos
  bytes de payload esperar (por el campo LEN), un 0xAA que aparezca dentro
  del payload no rompe el parseo.
- **No hay ACK ni reintentos en esta v1.** Si una trama llega corrupta se
  descarta (ver CRC) y se pierde. Para start/stop, que son criticos, conviene
  que el controlador loguee o refleje el estado (por ejemplo mandando un
  `SENSOR_UPDATE` inmediatamente al recibir un `START`) para que quede
  evidencia de que se aplico.

### CRC8

Poly `0x07`, init `0x00`, calculado sobre `CMD + LEN(2B) + PAYLOAD` (o sea,
todo el mensaje **menos** el SOF y menos el propio CRC).

```c
uint8_t crc8(const uint8_t *data, uint16_t len) {
    uint8_t crc = 0x00;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}
```

## 3. Comandos

### Controlador -> Pantalla

| CMD    | Nombre         | Cuando se manda |
|--------|----------------|------------------|
| `0x01` | SENSOR_UPDATE  | Periodico mientras hay una corrida activa. Hasta 5 veces/seg. |
| `0x02` | RUN_CHUNK      | En respuesta a un `REQUEST_RUN` de la pantalla, uno o mas por corrida. |

### Pantalla -> Controlador

| CMD    | Nombre         | Cuando se manda |
|--------|----------------|------------------|
| `0x10` | START          | El usuario toca "Iniciar Corrida" en la pantalla. |
| `0x11` | STOP           | El usuario toca "Detener Corrida". |
| `0x12` | REQUEST_RUN    | El usuario entra a la vista de graficas o cambia de corrida ahi. |

---

### `0x01` SENSOR_UPDATE (Controlador -> Pantalla)

Lectura en vivo de los 4 sensores. El **tiempo lo maneja el controlador**
(la pantalla no tiene cronometro propio, solo muestra lo que le llega aca).

| Offset | Tamano | Campo       | Tipo    | Unidad |
|--------|--------|-------------|---------|--------|
| 0      | 1      | run_id      | u8      | 1 = Primera Corrida, 2 = Segunda Corrida |
| 1      | 4      | dial1       | i32     | micrometros (µm) |
| 5      | 4      | dial2       | i32     | micrometros (µm) |
| 9      | 4      | peso        | i32     | milinewtons (mN) |
| 13     | 4      | tiempo      | u32     | milisegundos desde que arranco la corrida |

Payload total: **17 bytes**.

La pantalla convierte para mostrar: `dial_mm = dial_um / 1000.0` (3
decimales), `peso_N = peso_mN / 1000.0` (1 decimal), `tiempo` a `HH:MM:SS`.

**Frecuencia:** hasta 5 Hz. No hace falta mas rapido, la pantalla no hace
nada con la info entre actualizaciones salvo pintarla.

**Ejemplo** (run_id=1, dial1=12.345mm, dial2=8.000mm, peso=52.345N,
tiempo=00:01:05 = 65000ms), CRC8 incluido:

```
AA 01 11 00  01  39 30 00 00  40 1F 00 00  79 CC 00 00  E8 FD 00 00  66
   |  |__|   |   |___dial1___| |___dial2___| |___peso____| |__tiempo___| |
   |  LEN=17 run_id                                                     CRC8
   CMD
```

---

### `0x02` RUN_CHUNK (Controlador -> Pantalla)

Respuesta a `REQUEST_RUN`. Los datos guardados de una corrida se mandan en
uno o mas paquetes ("chunks") de hasta 16 puntos cada uno, para no violar el
limite de 255 bytes de payload.

| Offset | Tamano | Campo      | Tipo | Descripcion |
|--------|--------|------------|------|-------------|
| 0      | 1      | run_id     | u8   | 1 o 2 |
| 1      | 1      | count      | u8   | cantidad de puntos en este chunk (1-16) |
| 2      | 1      | es_ultimo  | u8   | 1 = es el ultimo chunk de esta corrida, 0 = vienen mas |
| 3      | 12*count | puntos   | —    | `count` estructuras de 12 bytes (ver abajo) |

Cada **punto** (12 bytes):

| Offset | Tamano | Campo   | Tipo | Unidad |
|--------|--------|---------|------|--------|
| 0      | 4      | dial1   | i32  | micrometros (µm) |
| 4      | 4      | peso    | i32  | milinewtons (mN) |
| 8      | 4      | tiempo  | u32  | milisegundos desde el inicio de la corrida |

Payload total por chunk: `3 + count*12` bytes (maximo 195 con count=16).

La pantalla hoy solo grafica `dial1` y `peso` (por indice de punto, no usa el
campo `tiempo` del punto todavia como eje X — queda disponible para cuando se
quiera graficar por tiempo real en vez de por muestra). No hay limite de
cuantos chunks se pueden mandar para una corrida; la pantalla los va
agregando al grafico (rolling window de 30 puntos) a medida que llegan.

**No hace falta que el controlador sepa el total de puntos de antemano**:
puede ir leyendo su almacenamiento y mandando chunks de 16 en 16 a medida que
los arma, y marcar `es_ultimo=1` en el ultimo.

---

### `0x10` START (Pantalla -> Controlador)

Payload: 1 byte, `run_id` (1 o 2).

Le dice al controlador "arranca la corrida `run_id`": debe poner su
cronometro en 0, empezar a leer los sensores y a mandar `SENSOR_UPDATE`
periodicamente (hasta 5 Hz) hasta recibir el `STOP` correspondiente.

```
AA 10 01 00 01 0B
```

### `0x11` STOP (Pantalla -> Controlador)

Payload: 1 byte, `run_id` (1 o 2).

Le dice al controlador que pare de correr/mandar `SENSOR_UPDATE` para esa
corrida y guarde los datos (para poder responder despues a un
`REQUEST_RUN`).

```
AA 11 01 00 01 1D
```

### `0x12` REQUEST_RUN (Pantalla -> Controlador)

Payload: 1 byte, `run_id` (1 o 2).

Le pide al controlador que mande los datos guardados de esa corrida como uno
o mas `RUN_CHUNK`. Se manda cada vez que el usuario entra a la vista de
graficas o toca el boton de "Corrida 1" / "Corrida 2" ahi.

```
AA 12 01 00 02 2E
```

## 4. Flujos tipicos

**Correr un ensayo:**
1. Usuario toca "Iniciar Primera Corrida" -> pantalla manda `START(run_id=1)`.
2. Controlador empieza a leer sensores y manda `SENSOR_UPDATE` hasta 5 veces/seg.
3. Usuario toca "Detener Corrida" -> pantalla manda `STOP(run_id=1)`.
4. Controlador deja de mandar updates y guarda los datos de esa corrida.
5. (Se repite para run_id=2 cuando el usuario toca "Siguiente".)

**Ver los datos guardados:**
1. Usuario toca "Ver Grafica" (o "Corrida 1"/"Corrida 2" ya adentro) ->
   pantalla manda `REQUEST_RUN(run_id)`.
2. Controlador responde con uno o mas `RUN_CHUNK(run_id, ...)`, el ultimo con
   `es_ultimo=1`.
3. La pantalla va graficando cada chunk a medida que llega.

## 5. Cosas pendientes / mejoras futuras (no bloqueantes para v1)

- Sin ACK/retransmision: si se pierde un `START`/`STOP` por ruido en la
  linea, hoy no hay forma de saberlo desde la pantalla. Si esto es un
  problema en la practica, se puede agregar un `SENSOR_UPDATE` inmediato como
  confirmacion implicita, o un CMD de ACK explicito.
- `LEN` de 2 bytes permite crecer el payload maximo mas alla de 255 si algun
  dia se necesita (hoy limitado a 255 por simplicidad de buffers).
- El campo `tiempo` de cada punto en `RUN_CHUNK` no se usa todavia como eje X
  del grafico (se grafica por indice de muestra); queda ahi para cuando se
  quiera graficar contra tiempo real.

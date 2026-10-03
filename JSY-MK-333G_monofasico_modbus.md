# JSY-MK-333G — Uso monofásico 220 V con 3 corrientes

Guía rápida para leer el módulo por Modbus RTU en una red monofásica de 220 V entre dos líneas (R y S), aprovechando los 3 transformadores de corriente (CT) para medir 3 circuitos distintos.

---

## 1. Conexión eléctrica (con todo desenergizado)

### Bornero de tensión (bornero grande verde)

| Borne | Conectar a |
|---|---|
| A | R |
| B | R (puente con A) |
| C | R (puente con A) |
| N | S |

Al puentear A, B y C a la misma línea R, los tres canales miden los mismos 220 V. Así cada CT puede medir la corriente y la potencia de un circuito diferente con valores correctos.

> Si solo conectas A (y dejas B y C libres), las corrientes B y C se medirán igual, pero sus potencias y factores de potencia saldrán en 0 porque esos canales no tienen tensión de referencia.

### Transformadores de corriente

| CT | Abraza a |
|---|---|
| CT fase A | Circuito 1 (cable R del circuito) |
| CT fase B | Circuito 2 (cable R del circuito) |
| CT fase C | Circuito 3 (cable R del circuito) |

- Cada CT debe abrazar **un solo conductor** (no R y S juntos, o se anulan y lees 0).
- Respeta la flecha del CT en el sentido de la carga. Si la potencia sale invertida, gira el CT.

### Bornero de alimentación y comunicación (4 pines)

| Borne | Conectar a |
|---|---|
| V+ | Fuente DC +9 a +24 V |
| G- | Fuente DC negativo (GND) |
| A | A (o D+) del adaptador USB–RS485 |
| B | B (o D-) del adaptador USB–RS485 |

Si no hay comunicación, prueba invertir A y B.

---

## 2. Parámetros de comunicación

| Parámetro | Valor de fábrica |
|---|---|
| Protocolo | Modbus RTU |
| Interfaz | RS-485 (half-duplex) |
| Dirección del esclavo | 1 |
| Velocidad | 9600 bps |
| Formato | 8 bits de datos, sin paridad, 1 bit de parada (8N1) |
| Función de lectura | 03 (Read Holding Registers) |
| Función de escritura | 10H / 16 (Write Multiple Registers) |
| Orden de bytes | Byte alto primero dentro de cada registro |
| Valores de 32 bits | Registro alto primero |
| Velocidades posibles | 4800, 9600, 19200 bps |

Notas:

- Los datos internos se actualizan cada 1 segundo; no tiene sentido leer más rápido.
- Si el módulo detecta un error en la trama, simplemente **no responde** (verás *timeout*).

---

## 3. Registros a leer en modo monofásico

La columna **modpoll `-r`** es la dirección decimal + 1 (modpoll cuenta desde 1). Todos se leen con `-t 4`.

### Mediciones instantáneas

| Hex | modpoll `-r` | Dato | Conversión |
|---|---|---|---|
| 0100 | 257 | Voltaje canal A | ÷100 → V |
| 0101 | 258 | Voltaje canal B (≈ A si está puenteado) | ÷100 → V |
| 0102 | 259 | Voltaje canal C (≈ A si está puenteado) | ÷100 → V |
| 0103 | 260 | Corriente circuito 1 (CT A) | ÷100 → A |
| 0104 | 261 | Corriente circuito 2 (CT B) | ÷100 → A |
| 0105 | 262 | Corriente circuito 3 (CT C) | ÷100 → A |
| 0106 | 263 | Potencia activa circuito 1 | W |
| 0107 | 264 | Potencia activa circuito 2 | W |
| 0108 | 265 | Potencia activa circuito 3 | W |
| 0109–010A | 266–267 | Potencia activa total (32 bits) | W |
| 0115 | 278 | Frecuencia | ÷100 → Hz |
| 0116 | 279 | Factor de potencia circuito 1 | ÷1000 |
| 0117 | 280 | Factor de potencia circuito 2 | ÷1000 |
| 0118 | 281 | Factor de potencia circuito 3 | ÷1000 |

### Energía acumulada (32 bits, se conserva sin alimentación)

| Hex | modpoll `-r` | Dato | Conversión |
|---|---|---|---|
| 011A–011B | 283–284 | Energía circuito 1 | ÷100 → kWh |
| 011C–011D | 285–286 | Energía circuito 2 | ÷100 → kWh |
| 011E–011F | 287–288 | Energía circuito 3 | ÷100 → kWh |
| 0120–0121 | 289–290 | Energía total | ÷100 → kWh |

### Registros que no aplican en monofásico

- **0133H (alarmas):** el bit de "secuencia de fases invertida" puede activarse porque las tres tensiones están en fase. Ignóralo.
- **Potencias reactivas y aparentes (010B–0114H)** sí son válidas por circuito, pero normalmente no se necesitan.

### Cómo armar un valor de 32 bits

```
valor = (registro_alto × 65536) + registro_bajo
```

Ejemplo: en 289–290 lees `1` y `5000` → (1 × 65536 + 5000) = 70536 → ÷100 = **705.36 kWh**.

---

## 4. Lectura con modpoll (Windows)

Desde la carpeta donde está `modpoll.exe`. Si el puerto no abre, usa `\\.\COM10` en vez de `COM10`.

**Prueba de vida** (el primer valor debe ser `819` = 333H):

```powershell
.\modpoll.exe -m rtu -a 1 -b 9600 -p none -t 4 -r 1 -c 1 -1 COM10
```

**Voltajes:**

```powershell
.\modpoll.exe -m rtu -a 1 -b 9600 -p none -t 4 -r 257 -c 3 -1 COM10
```

**Corrientes:**

```powershell
.\modpoll.exe -m rtu -a 1 -b 9600 -p none -t 4 -r 260 -c 3 -1 COM10
```

**Potencias activas (por circuito + total):**

```powershell
.\modpoll.exe -m rtu -a 1 -b 9600 -p none -t 4 -r 263 -c 5 -1 COM10
```

**Todo lo útil en una sola lectura** (257 a 290, 34 registros):

```powershell
.\modpoll.exe -m rtu -a 1 -b 9600 -p none -t 4 -r 257 -c 34 -1 COM10
```

Quitando `-1` del final, modpoll lee en bucle cada segundo (detener con Ctrl+C).

---

## 5. Cómo es la comunicación por dentro (trama Modbus RTU)

El PC (maestro) pregunta y el módulo (esclavo) responde. Cada trama tiene:

| Campo | Tamaño | Descripción |
|---|---|---|
| Dirección | 1 byte | Dirección del esclavo (01) |
| Función | 1 byte | 03 = leer registros |
| Datos | N bytes | Dirección inicial y cantidad (pregunta) o valores (respuesta) |
| CRC16 | 2 bytes | Verificación, **byte bajo primero** |

### Ejemplo: leer los 3 voltajes

Pregunta del PC:

```
01 03 01 00 00 03 04 37
│  │  └─┬─┘ └─┬─┘ └─┬─┘
│  │    │     │     └─ CRC
│  │    │     └─ cantidad: 3 registros
│  │    └─ dirección inicial: 0100H
│  └─ función 03 (leer)
└─ esclavo 1
```

Respuesta del módulo:

```
01 03 06 56 11 56 22 56 33 1F 77
│  │  │  └─┬─┘ └─┬─┘ └─┬─┘ └─┬─┘
│  │  │    │     │     │     └─ CRC
│  │  │    │     │     └─ 5633H = 22067 → 220.67 V
│  │  │    │     └─ 5622H = 22050 → 220.50 V
│  │  │    └─ 5611H = 22033 → 220.33 V
│  │  └─ 6 bytes de datos
│  └─ función 03
└─ esclavo 1
```

### Otras tramas útiles (CRC ya calculado)

| Qué hace | Trama a enviar |
|---|---|
| Leer modelo (0000H, 1 registro) | `01 03 00 00 00 01 84 0A` |
| Leer voltajes (0100H, 3 registros) | `01 03 01 00 00 03 04 37` |
| Leer 0100H a 0121H (34 registros) | `01 03 01 00 00 22 C4 2F` |

### Respuestas de error

Si el módulo responde con la función en 83H (03 + bit alto), el tercer byte indica el error:

| Código | Significado |
|---|---|
| 01 | Función no soportada |
| 02 | Dirección de registro no válida |
| 03 | Valor de dato fuera de rango |

---

## 6. Diagnóstico rápido

| Síntoma | Causa probable |
|---|---|
| *Reply time-out* | Cables A/B invertidos, dirección o velocidad incorrecta, puerto equivocado, módulo sin alimentación |
| LED "communication" no parpadea | No llega la trama: revisar cableado RS-485 y puerto COM |
| LED parpadea pero no hay respuesta | Parámetros serie distintos (velocidad, paridad) o dirección |
| Voltajes ~110 V | Borne N sin conectar (neutro flotando) |
| Corriente 0 con carga encendida | CT abrazando dos conductores o carga en otro circuito |
| Potencia negativa o invertida | CT colocado al revés |
| Potencia B/C en 0 pero corriente sí | Bornes B y C sin puentear a R |

---

## 7. Precauciones

- No escribas en **0004H** (cambia dirección y velocidad) ni en **000CH–000DH** (borra todas las energías) mientras pruebas.
- Conecta y desconecta los bornes de tensión y los CT **siempre sin tensión**.
- Usa cable par trenzado apantallado para el RS-485 y aléjalo de los cables de potencia.

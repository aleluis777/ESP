# HARDWARE.md — Gestor de Aire Acondicionado (ESP32)

> Referencia de hardware para asistentes de IA y para el equipo de firmware.
> Fuente: esquemáticos Altium — hoja completa + detalles de ESP32 (`U13`), relés A (`U3/U4/U5`), relés B (`U8/U9/U10`), salidas AT/BP (`U14/U16`) y lógica de bypass (`Q6–Q13`).
> **⚠️ = hallazgo o inferencia, no verificado en placa.**
> **Nota de fidelidad:** la vista general del esquemático se revisó a baja resolución. Los valores y nets marcados como "ilegible" necesitan un recorte ampliado para confirmarse.

---

## 1. Resumen del sistema

Controlador embebido para gestión de equipos de aire acondicionado.

| Bloque | Implementación |
|---|---|
| MCU | **ESP32-WROOM-32D** (módulo de 38 pines, antena PCB, sin PSRAM) |
| Red Ethernet (RJ45) | Módulo W5500 (`U6`) sobre SPI |
| Memoria externa | Flash SPI `W25Q16DVSSIG` (`U7`) — **NO POBLADA (DNP)** |
| Sensores de temperatura | 4× NTC B3950 → módulo ADC I²C de 8 canales (`U2`) |
| Reloj de tiempo real | Módulo RTC I²C (`U9`) |
| Temperatura + humedad | Sensor digital de 1 hilo (net `HUM`) |
| Bus serie industrial | RS-485 half-duplex con `SP3485` (`U12`) |
| Salida alerta alta temp. | Relé `U14` → header `P2` (contacto seco) |
| Salida estado de bypass | Relé `U16` → header `P5` (contacto seco) |
| Salidas de contacto seco | **4 salidas** (relés simples `AA1`–`AA4`) hacia el A/A |
| Bypass | Lógica discreta de transistores + 2 relés dobles que puentean el A/A |
| Señalización local | Buzzer (`Z1`) + LEDs indicadores |
| Programación | Conector tipo USB (`X1`) usado como cabecera UART |

---

## 2. Alimentación

Bloque **REGULACION 12V-4V-3V3**:

```
P1 (header) ── FUSE1 ── D1 (protección de polaridad) ── riel 4V
                                                          │
                                            U11 AMS1117 ──┴── 3V3 (+ C4 100nF)
```

| Riel | Uso |
|---|---|
| `4V` | Bobinas de todos los relés (U3, U4, U5, U8, U9, U10, U14, U16) |
| `3V3` | ESP32, módulos Ethernet/ADC/RTC, SP3485, lógica de bypass |

### ⚠️ Puntos a revisar en la alimentación

1. **El título del bloque dice "12V-4V-3V3" pero solo se ve una etapa de regulación (AMS1117).** No aparece el convertidor de 12 V a 4 V. O está fuera de la hoja, o `P1` recibe directamente 4 V. **Confirmar la tensión real de entrada en `P1`** — la etiqueta cercana es ilegible (parece "48V" o "-48V", lo que sería otra cosa completamente).
2. **AMS1117 alimentado desde 4 V es marginal.** El dropout del AMS1117-3.3 ronda 1.1–1.3 V a corriente alta. Con 4 V de entrada quedan ~2.7–2.9 V de salida en los picos de transmisión WiFi del ESP32 (300–500 mA), por debajo del mínimo del módulo. Esto produce resets aleatorios difíciles de diagnosticar. **Recomendación: usar un LDO de bajo dropout (tipo AMS1117 no sirve aquí) o alimentar el regulador desde el riel de 12 V.**
3. **El pin VBUS de `X1` está conectado al riel `4V`.** Si `X1` es un conector USB real, VBUS son 5 V y el riel de bobinas subirá a 5 V al conectar el cable de programación. Si los relés son de 4 V nominales, funcionarán fuera de especificación durante la programación. **Confirmar.**
4. **No se observan diodos flyback en paralelo con ninguna bobina de relé** en ninguna hoja. Si no están poblados, cada desactivación genera un pico inductivo capaz de destruir los transistores y resetear el ESP32. **Prioridad alta.**

---

## 3. Mapa de pines del ESP32 (`U13`)

| Pin | Módulo | GPIO | Net | Función | Dir. | Activo en |
|---:|---|---|---|---|---|---|
| 1 | GND | — | `GND` | Tierra | PWR | — |
| 2 | 3V3 | — | `3V3` | Alimentación | PWR | — |
| 3 | EN | — | `EN_ESP` | Enable/reset (pull-up `R23` 100k) + pin de `X1` | IN | BAJO = reset |
| 4 | SENSOR_VP | 36 | `RX3` | RS-485 RX (desde RO del SP3485) | IN (solo entrada) | — |
| 5 | SENSOR_VN | 39 | `P1` | **Reservado, sin uso previsto** | IN (solo entrada) | — |
| 6 | IO34 | 34 | `BPS_STATUS` | Realimentación de bypass | IN (solo entrada) | **BAJO** |
| 7 | IO35 | 35 | `RESET` | Pulsador `SW1` (pull-up `R10` 4K7) — **SIN USO: el firmware no lo lee** (ver §8.2) | IN (solo entrada) | **BAJO** |
| 8 | IO32 | 32 | `EN_485` | DE + /RE del SP3485 | OUT | ALTO = TX |
| 9 | IO33 | 33 | `TX3` | RS-485 TX (hacia DI del SP3485) | OUT | — |
| 10 | IO25 | 25 | `P2` | **IRQ del W5500 `U6`** (pin 5) — el firmware lo usa por interrupción (ver §4.1). Antes iba a ser el RX de la pantalla, que se movió a GPIO39 (§14) | IN | **BAJO** |
| 11 | IO26 | 26 | `P6` | **UART2 TX** — enlace serie hacia la pantalla braindlab (ver §14). Antes iba al /CS de `U7`, que no se puebla (ver §4.1) | OUT | — |
| 12 | IO27 | 27 | `HUM` | Sensor digital temp+humedad (1 hilo) | I/O | — |
| 13 | IO14 | 14 | `BP_S` | Solicitud de bypass por software | OUT | **BAJO** |
| 14 | IO12 | 12 | `AA1` | Relé simple U3 (vía Q1) + LED1 | OUT | ALTO |
| 15 | GND | — | `GND` | Tierra | PWR | — |
| 16 | IO13 | 13 | `OUT_AT` | Alerta alta temp: relé U14 (Q11) + Q5→`S1` | OUT | ALTO |
| 17–22 | SD0…CLK | 6–11 | — | **Flash interna del módulo. NO USAR.** | — | — |
| 23 | IO15 | 15 | `P7` | **Buzzer `Z1` (vía Q12, `R25` 10K)** | OUT | ALTO |
| 24 | IO2 | 2 | `AA2` | Relé simple U4 (vía Q2) + LED2 | OUT | ALTO |
| 25 | IO0 | 0 | `BOOT0` | Strapping de arranque (pull-up `R2` 100k) + pin de `X1`. **Propuesto: /CS de la MicroSD** (pendiente de cablear) | I/O | BAJO = bootloader |
| 26 | IO4 | 4 | `AA4` | Relé simple U9 (vía Q4) | OUT | ALTO |
| 27 | IO16 | 16 | `AA3` | Relé simple U8 (vía Q3) | OUT | ALTO |
| 28 | IO17 | 17 | `SS_TX` | **/CS del módulo W5500 `U6`** | OUT | **BAJO** |
| 29 | IO5 | 5 | `RST` | **RESET del módulo W5500 `U6`** (pin 6, `rst`) | OUT | **BAJO** |
| 30 | IO18 | 18 | `SPI_SCK` | SPI — reloj (compartido) | OUT | — |
| 31 | IO19 | 19 | `SPI_MISO` | SPI — MISO (compartido) | IN | — |
| 32 | NC | — | — | No conectado | — | — |
| 33 | IO21 | 21 | `SDA` | I²C — datos | I/O | — |
| 34 | RXD0 | 3 | `RX0` | UART0 — consola / programación (`X1`) | IN | — |
| 35 | TXD0 | 1 | `TX0` | UART0 — consola / programación (`X1`) | OUT | — |
| 36 | IO22 | 22 | `SCL` | I²C — reloj | OUT | — |
| 37 | IO23 | 23 | `SPI_MOSI` | SPI — MOSI (compartido) | OUT | — |
| 38 | GND | — | `GND` | Tierra | PWR | — |

---

## 4. Buses

### 4.1 SPI — compartido entre dos periféricos

**Módulo Ethernet `U6` (W5500) — pinout confirmado:**

| Pin | Nombre | Net | GPIO |
|---:|---|---|---|
| 9 | `mosi` | `SPI_MOSI` | 23 |
| 8 | `gnd` | `GND` | — |
| 7 | `vcc` | `3V3` | — |
| 6 | `rst` | `RST` | 5 |
| 5 | `IRQ` | `P2` | 25 |
| 4 | `ss` | `SS_TX` | 17 |
| 3 | `nc` | sin conectar (pin no usado) | — |
| 2 | `miso` | `SPI_MISO` | 19 |
| 1 | `csk` (sck) | `SPI_SCK` | 18 |

**Resumen del bus (un solo dispositivo):**

| Señal | GPIO | W5500 (`U6`) |
|---|---|---|
| SCK | 18 | pin 1 `csk` |
| MISO | 19 | pin 2 `miso` |
| MOSI | 23 | pin 9 `mosi` |
| CS | 17 (`SS_TX`) | pin 4 `ss` |
| RESET | 5 (`RST`) | pin 6 `rst` |

**`U7` — W25Q16DVSSIG (NO POBLADO).** Cableado presente en la PCB por si se quisiera montar en el futuro:

| Pin | Señal | Conexión |
|---:|---|---|
| 1 | /CS | `P6` (GPIO26) |
| 2 | DO | `SPI_MISO` |
| 3 | /WP | `3V3` |
| 4 | GND | `GND` |
| 5 | DI | `SPI_MOSI` |
| 6 | CLK | `SPI_SCK` |
| 7 | /HOLD | `3V3` |
| 8 | VCC | `3V3` |

> ### `U7` (flash W25Q16) — NO SE PUEBLA
> Decisión de diseño: el chip `U7` **no se monta en la placa**. Consecuencias:
>
> - **`SPI_MOSI`, `SPI_MISO` y `SPI_SCK` son de un solo dispositivo.** El W5500 es el único maestro-esclavo del bus, lo que elimina cualquier riesgo de contención y simplifica el driver.
> - **GPIO26 (`P6`) queda completamente libre.** El pin ya no tiene función asignada.
> - **Marcar `U7` como DNP en el BOM y en la variante de montaje de Altium**, para que no se cuele en una tirada futura.
> - Si en algún momento se decide poblarlo, hace falta **añadir un pull-up de 10K en su `/CS` a 3V3** y volver a reservar GPIO26. Sin ese pull-up, un `/CS` flotante haría que la flash respondiera a la vez que el W5500 y corrompiera el tráfico Ethernet.
>
> **Nota:** la sugerencia anterior de llevar `IRQ` a GPIO26 quedó obsoleta: GPIO26 es el TX hacia la pantalla (§14) y el `IRQ` ya va a GPIO25 (§4.1).

**Notas para el firmware:**

- El W5500 trabaja en SPI **modo 0**. Soporta hasta 80 MHz, pero con un módulo enchufable conviene quedarse en **10–20 MHz**.
- **`SS_TX` (GPIO17) debe inicializarse en ALTO** antes de la primera transacción.
- Al ser el único dispositivo del bus, no hay que arbitrar chip selects ni preocuparse por contención en `SPI_MISO`.

**Reset del W5500 (`RST`, GPIO5) — confirmado como salida.** El pin `rst` del W5500 es **activo en BAJO**:

- Estado normal de operación: **GPIO5 en ALTO**.
- Para resetear: pulso a BAJO de **mínimo 500 µs**, volver a ALTO, y **esperar ~50 ms** antes de la primera transacción SPI (el PLL interno necesita estabilizarse). Acceder antes devuelve basura.
- GPIO5 tiene pull-up interno habilitado por defecto tras el reset del ESP32, así que el W5500 no queda en reset durante el arranque. Aun así, **conviene verificar si el módulo trae su propio pull-up en `rst`**; si no lo tiene, añadir uno de 10K externo.

**IRQ del W5500 (pin 5) — conectado a GPIO25 (verificado en la placa).** Versiones anteriores de este documento lo daban como sin conectar; era un error.

- El firmware usa la interrupción: `CONFIG_ETHERNET_SPI_INT0_GPIO=25` en `sdkconfig.defaults`. Antes iba por polling (`-1`, lectura SPI cada 10 ms).
- `IRQ` es salida open-drain, **activa en BAJO**. GPIO25 tiene pull-up interno.
- Con la MicroSD compartiendo el bus SPI, la interrupción evita que el polling ocupe el bus 100 veces por segundo.

### 4.2 I²C — dos periféricos

| Señal | GPIO |
|---|---|
| SDA | 21 |
| SCL | 22 |

**`U2` — Módulo ADC de 8 canales:**

| Pin | Señal | Net |
|---:|---|---|
| 1 | 3V3 | `3V3` |
| 2 | GND | `GND` |
| 3 | SDA | `SDA` |
| 4 | SCL | `SCL` |
| 5 | A1 | `ST1` |
| 6 | A2 | `ST2` |
| 7 | A3 | `ST3` |
| 8 | A4 | `ST4` *(ilegible, por confirmar)* |
| 9 | A5 | `REF` |
| 10 | A6 | `OBL1` |
| 11 | A7 | `OBL2` |
| 12 | A8 | `OBL3` |

Ocho canales analógicos sugieren **dos ADS1115 en el módulo** (4 canales cada uno), con direcciones distintas — probablemente `0x48` y `0x49`. **Confirmar referencia y direcciones.**

**Sensores de temperatura: NTC con B = 3950.** `ST1`–`ST4` son los cuatro canales de temperatura y `REF` la referencia.

El **acondicionamiento (divisor resistivo) está en la placa del módulo**, no en el esquemático principal. `ST1`–`ST4` llegan al ADC ya como tensión.

**Conversión a temperatura — ecuación Beta:**

```
1/T = 1/T0 + (1/B) · ln(R_ntc / R0)

  T  = temperatura en kelvin  (restar 273.15 para °C)
  T0 = 298.15 K  (25 °C)
  R0 = resistencia nominal del NTC a 25 °C
  B  = 3950
```

Y la resistencia se despeja del divisor. Con el NTC en el lado bajo:

```
R_ntc = R_serie · V_adc / (V_exc − V_adc)
```

**Datos que faltan para poder implementarlo:**

1. **Valor nominal del NTC a 25 °C** — los B3950 más comunes son de 10 kΩ y de 100 kΩ. Sin este dato la fórmula no da nada.
2. **Valor de la resistencia serie del divisor** y si el NTC va al lado alto o al bajo.
3. **Tensión de excitación del divisor** y qué mide exactamente el canal `REF`.

**Notas de implementación:**

- El NTC tiene **coeficiente negativo**: a más temperatura, menos resistencia. La lectura del ADC se mueve en sentido contrario al intuitivo según cómo esté el divisor. Es una fuente clásica de signos invertidos.
- La ecuación Beta tiene un error de ±1–2 °C en los extremos del rango. Si necesitas mejor exactitud, usar Steinhart-Hart con los tres coeficientes del fabricante.
- El divisor **es ratiométrico**: si `REF` mide la misma excitación que alimenta los NTC, trabaja siempre con el cociente `V_adc/V_ref` en lugar de con volts absolutos. Así se cancela la deriva de la fuente y de la referencia del ADC. Es la razón de que exista ese canal — conviene usarlo.
- Vigilar el **autocalentamiento**: si el divisor deja pasar demasiada corriente, el propio NTC se calienta y falsea la medida. Con un NTC de 100 kΩ el problema es despreciable; con uno de 10 kΩ y excitación permanente, no tanto.

⚠️ **Corrección respecto a versiones anteriores de este documento:** los sensores se habían anotado como PTC. Son **NTC**, coeficiente negativo. La curva y el signo son los contrarios.

`OBL1`–`OBL3` siguen sin identificar.

**`U9` — Módulo RTC:** pines `SDA`, `SCL`, `3V3`, `GND` (+ un pin `DS`). Probablemente DS3231 (dirección `0x68`). **Confirmar referencia.**

⚠️ **Verificar los pull-ups del bus I²C.** Si los dos módulos traen los suyos propios, quedan en paralelo y bajan la resistencia efectiva. No es fatal, pero conviene revisarlo.

### 4.3 RS-485 — `U12` SP3485

| Pin SP3485 | Señal | Net | GPIO |
|---:|---|---|---|
| 1 | RO | `RX3` | 36 |
| 2 | /RE | `EN_485` | 32 |
| 3 | DE | `EN_485` | 32 |
| 4 | DI | `TX3` | 33 |
| 5 | GND | `GND` | — |
| 6 | A | línea A (con protección) | — |
| 7 | B | línea B (con protección) | — |
| 8 | VCC | `3V3` | — |

- `/RE` y `DE` van unidos al mismo GPIO: **`EN_485` = 1 → transmitir, `EN_485` = 0 → recibir.** Nunca quedan ambos activos.
- El SP3485 es transceptor de 3.3 V, compatible directo con el ESP32.
- Las líneas A/B llevan LEDs indicadores (`D2`, `D4` con `R3`/`R19` 4K7) y diodos de protección (`D3`, `D5`, `D6`). `C8` = 100 nF de desacoplo.
- Resistencias de polarización/terminación: `R15`, `R17` (valores parcialmente legibles, ~10K/100K). **Confirmar si hay resistencia de terminación de 120 Ω y si es conmutable.**

**Secuencia obligatoria de transmisión:** poner `EN_485` en alto → escribir → **esperar a que el buffer de TX se vacíe realmente** (`uart_wait_tx_done()`, no solo `write()`) → bajar `EN_485`. Bajarlo antes de tiempo trunca el último byte.

### 4.4 Sensor de humedad/temperatura

Net `HUM` en GPIO27, línea única bidireccional (tipo DHT22/AM2302). Requiere pull-up externo de 4.7–10 kΩ — **confirmar si está poblado**.

---

## 5. Etapa de potencia — Relés

### 5.1 Relés simples de control del A/A

```
GPIO ──[R 10k]── Base(Q, NPN)
                  Emisor ── GND
                  Colector ── pin 2 (bobina −)
                              pin 5 (bobina +) ── +4V
```

| Canal | Net | GPIO | R base | Transistor | Relé | Salida | LED |
|---|---|---|---|---|---|---|---|
| 1 | `AA1` | 12 | `R1` 10 kΩ | Q1 | U3 | `U1` | LED1 (`R6` ~4K5) |
| 2 | `AA2` | 2 | `R8` 10 kΩ | Q2 | U4 | `U2` | LED2 (`R7` ~4K5) |
| 3 | `AA3` | 16 | `R9` 10 kΩ | Q3 | U8 | `U3` | — |
| 4 | `AA4` | 4 | `R11` 10 kΩ | Q4 | U9 | `U4` | — |

**Activo en ALTO. Estado seguro: GPIO en `0`.**

> ⚠️ Los nets de salida se llaman `U1`…`U4`, igual que designadores de componente. Renombrarlos (`OUT_AA1`, etc.).
> ⚠️ Solo `AA1` y `AA2` tienen LED indicador. Si es intencional, está bien; si no, faltan dos LEDs para `AA3`/`AA4`.

### 5.2 Relés dobles de bypass — **corrección respecto a la versión anterior**

| Relé | Pin A1 (bobina −) | Pin A2 (bobina +) | Contactos | Cubre |
|---|---|---|---|---|
| U5 | `REL_BP` | `4V` | `Y1`, `2Y1-1`, `Y2`, `5Y2-1` | canales 1 y 2 |
| U10 | `REL_BP` | `4V` | `Y3`, `2Y3-1`, `Y4`, `5Y4-1` | canales 3 y 4 |

**Resuelto:** lo que se leía como `REL_BPA1` y `4VA2` en los recortes es en realidad el net `REL_BP` / `4V` **seguido del nombre de pin de la bobina** (`A1` y `A2`). No son nets distintos. **Ambos relés dobles cuelgan del mismo net `REL_BP` y del mismo riel de 4 V.** No falta ningún driver.

Cada relé doble es de dos polos: los contactos de los relés simples pasan a través del relé doble antes de llegar al conector de salida.

### 5.3 Comportamiento funcional del bypass

- **Bypass INACTIVO:** la salida de cada canal la determina su relé simple (`AA1`–`AA4`). El gestor controla el A/A.
- **Bypass ACTIVO:** los relés dobles conmutan y **puentean las salidas**, dejando el A/A conectado directamente, sin importar `AA1`–`AA4`. El gestor queda fuera del lazo.

---

## 6. Salidas de contacto seco

### 6.1 Alerta de alta temperatura — header `P2`

```
GPIO13 (OUT_AT) ──[R22]── Base(Q11, NPN) ── Emisor GND
                                             Colector ── U14 pin 2 (bobina −)
                                                         U14 pin 5 (bobina +) ── +4V
```

| Relé U14 | Header P2 |
|---|---|
| pin 3 | P2-1 |
| pin 1 (común, `AT1`/`AT2`) | P2-2 |
| pin 4 | P2-3 |

Salida aislada C/NC/NA, apta para la entrada de alarma de un equipo externo. **Activo en ALTO.**

⚠️ `OUT_AT` (GPIO13) maneja **dos** bases en paralelo: `Q11` (vía `R22`) hacia el relé, y `Q5` (vía `R12` 1 kΩ) hacia el net `S1`. Con `R12` de 1 kΩ la corriente de base de Q5 sola ronda 2.6 mA; sumada a la de Q11 sigue dentro de los 40 mA del GPIO, pero conviene verificar el valor de `R22`, que es ilegible en el esquemático.

### 6.2 Estado de bypass — header `P5`

```
REL_BP ── U16 pin 2 (bobina −)
          U16 pin 5 (bobina +) ── +4V
```

| Relé U16 | Header P5 |
|---|---|
| pin 3 | P5-1 |
| pin 1 (común, `BP1`/`BP2`) | P5-2 |
| pin 4 | P5-3 |

`U16` no tiene driver propio: cuelga del mismo net `REL_BP` y se energiza en paralelo con los relés dobles.

> ### ⚠️ Carga sobre Q6 — revisar
> `REL_BP` alimenta el lado bajo de **tres bobinas**: `U5`, `U10` (relés dobles, de dos polos, típicamente más consumo) y `U16`. A 40–70 mA por bobina son **150–250 mA de corriente de colector**, con solo ~2.2 mA de base (`R13` = 1 kΩ). Eso exige hFE ≥ 100 en saturación, que es exactamente donde los transistores pequeños empiezan a fallar.
>
> **Estado: validado en campo por el diseñador — el circuito funciona correctamente en la placa actual.** Se documenta solo como margen a vigilar: si en el futuro se cambia la referencia de Q6 o de los relés por otros de mayor consumo, este es el punto que se queda corto primero. Si alguna vez aparecen relés que no enganchan o que zumban, medir la Vce de Q6 con los tres activos: por encima de 0.3 V estaría en zona lineal.

---

## 7. Lógica de bypass (`Q6`–`Q13`)

Bloque discreto que combina la señal de bypass por hardware (`BP_H`) y la de software (`BP_S`) para generar `REL_BP`, `BPS_STATUS` y `S2`.

### 7.1 Etapa de entrada — Q7 y Q8 (S8550, PNP)

```
3V3 ── Q7 (base ← R14 10K ← BP_H)
         │
       Q8 (base ← R18 10K ← BP_S)
         │
      NODO_BP ──[R21 100K]── GND
```

Los dos PNP están **en serie**. Un PNP conduce cuando su base está en BAJO.

### 7.2 Etapas de salida desde `NODO_BP`

| Rama | Componentes | Salida | Comportamiento |
|---|---|---|---|
| Mando de relés | `R13` 1K → base Q6 (NPN, emisor GND) | `REL_BP` | `NODO_BP` alto → Q6 satura → hunde `REL_BP` → **energiza U5, U10 y U16** |
| Realimentación al MCU | `R27` 5.1K → base Q13 (NPN, emisor GND); `R26` 100K pull-up a 3V3; `C3` a GND | `BPS_STATUS` → GPIO34 | `NODO_BP` alto → Q13 satura → **`BPS_STATUS` = 0**. Inactivo → pull-up lo lleva a 1. **Señal invertida.** |
| Señal auxiliar | `R20` 10K → base Q10 (NPN); colector con `R16` 10K a 3V3 → base Q9 (NPN, emisor GND) | `S2` | Doble inversión: `NODO_BP` alto → Q10 satura → base Q9 a tierra → Q9 corta → **`S2` liberado**. `NODO_BP` bajo → Q9 satura → **`S2` a GND** |

`C3` forma un filtro RC con `R26` (100K) para antirrebote. Con `C3` = 100 nF la constante es ~10 ms. **Confirmar el valor de `C3`** y respetar ese retardo antes de leer el pin tras un cambio.

### 7.3 ⚠️ HALLAZGO CRÍTICO: la lógica es AND, no OR

**Intención funcional confirmada por el diseñador:** el bypass debe poder activarse **desde el contacto físico externo (`BP_H`) o desde el ESP32 por Ethernet (`BP_S`), indistintamente**, y `BPS_STATUS` debe reflejar que hay bypass **venga de donde venga**. Eso es una función OR.

**El circuito no hace eso.**

Con Q7 y Q8 en serie, la corriente solo llega a `NODO_BP` si **ambos** conducen: `BP_H` en BAJO **Y** `BP_S` en BAJO.

| `BP_H` | `BP_S` | Q7 | Q8 | `NODO_BP` | Bypass | `BPS_STATUS` |
|---|---|---|---|---|---|---|
| BAJO | BAJO | ON | ON | **ALTO** | **ACTIVO** | 0 |
| BAJO | ALTO | ON | OFF | BAJO | inactivo | 1 |
| ALTO / flotante | BAJO | OFF | ON | BAJO | inactivo | 1 |
| ALTO / flotante | ALTO | OFF | OFF | BAJO | inactivo | 1 |

**Consecuencias:**

1. Con el contacto de bypass por hardware **abierto**, el firmware **nunca** puede activar el bypass.
2. `R14` y `R18` no tienen resistencia de polarización a 3V3. Con `BP_H` o `BP_S` flotantes (por ejemplo el ESP32 en reset, GPIO en alta impedancia) la base del PNP queda al aire. El estado por defecto resulta ser "bypass inactivo", que es seguro, pero deja las bases indefinidas. **Añadir un pull-up de 10K a 3V3 en cada base.**

**Para obtener la función OR:** pasar los dos PNP de serie a **paralelo** — ambos emisores a 3V3, ambos colectores a `NODO_BP`, manteniendo `R21` como pull-down. Así cualquiera de las dos señales activa el bypass por su cuenta.

> ⚠️ **Numeración de pines de Q7/Q8.** Con la convención del resto del esquemático (en Q6: pin 1 = base, pin 2 = emisor, pin 3 = colector), Q7 tiene el **colector** a 3V3 y el **emisor** hacia Q8 — un PNP conectado al revés, que trabajaría en región activa inversa con ganancia muy baja. Puede ser que la librería del S8550 numere distinto. **Verificar el símbolo antes de fabricar.**

### 7.4 ⚠️ Nets `S1` y `S2` posiblemente sin destino

En la hoja general, los colectores de `Q5` (net `S1`) y `Q9` (net `S2`) aparecen con un marcador rojo punteado, que en Altium suele indicar un net con un solo pin conectado (violación de ERC). **Si `S1` y `S2` no van a ningún lado, esos dos transistores no hacen nada.** Confirmar si son señales para los headers `P3`/`P4` o si quedaron huérfanas.

---

## 8. Interfaz de usuario y programación

### 8.1 Conector de programación `X1` (`USB_DEV`)

Conector con forma de USB usado como cabecera de programación. Nets: `GND`, `EN_ESP`, `RX0`, `TX0`, `BOOT0`, `4V` (en VBUS). `C5` = 100 nF.

**No es un puerto USB funcional** — no hay controlador USB-serie en la placa. Se necesita un adaptador externo que mapee esos pines. Ver la advertencia sobre VBUS en la sección 2.

### 8.2 Pulsador de reset `SW1`

Pulsador a GND sobre el net `RESET` (GPIO35), con pull-up `R10` = 4K7 a 3V3. **Activo en BAJO.** Es una entrada de propósito general, **no** el reset del ESP32 (que sería `EN_ESP`).

**Decisión: el pulsador `SW1` NO se usa.** El firmware no lee GPIO35 ni le asigna ninguna función. Al ser solo entrada, tampoco sirve para reasignarlo como salida (p. ej. /CS de la MicroSD).

### 8.3 Buzzer `Z1`

```
3V3 ── Z1 (buzzer) ── Colector(Q12, NPN) ── Emisor GND
                      Base ←[R25 10K]← P7 (GPIO15)
```

**Activo en ALTO.** Si es un buzzer pasivo hay que generarle la frecuencia con PWM (LEDC); si es activo, basta con poner el pin en alto.

### 8.4 LEDs indicadores

| LED | Fuente | Resistencia |
|---|---|---|
| LED de alimentación | `3V3` | `R5` ~4 kΩ |
| LED1 | `AA1` (GPIO12) | `R6` ~4.5 kΩ |
| LED2 | `AA2` (GPIO2) | `R7` ~4.5 kΩ |

⚠️ Con 4.5 kΩ desde 3.3 V y una caída de LED de ~2 V, la corriente es de apenas **~0.3 mA**. Los LEDs van a estar muy tenues. Si se quiere visibilidad, bajar a 470 Ω–1 kΩ.

---

## 9. Bloque de entradas de contacto seco — **NO POBLADO (DNP)**

Seis redes RC idénticas: resistencia de 10 kΩ desde `3V3` y condensador a GND. Es la topología típica de entrada de contacto seco (pull-up + antirrebote), con el contacto externo cerrando a GND.

| Pull-up | Condensador | Net |
|---|---|---|
| `R28` 10K | `C1` | `P1` |
| `R29` 10K | `C2` | `P2` |
| `R30` 10K | `C6` | `P4` |
| `R31` 10K | `C7` | `P3` |
| `R32` 10K | `C9` | `P6` |
| `R33` 10K | `C10` | `P7` |

**Decisión de diseño: este bloque no se suelda.** Quedó dibujado como previsión, por si más adelante hacían falta entradas. Ningún componente de la tabla se monta.

> ### ⚠️ Trampa latente si alguien puebla este bloque
> Dos de estas seis redes chocan con funciones que **ya están en uso**:
>
> - **`R33`/`C10` sobre `P7` (GPIO15)** — ese pin maneja el **buzzer** vía Q12. Poblar la red le añadiría un pull-up permanente a 3V3 y un condensador en paralelo con la base del transistor. El buzzer sonaría de forma indebida.
> - **`R32`/`C9` sobre `P6` (GPIO26)** — pin actualmente libre; si se le asigna otra función, la red interferiría.
>
> Además, **`P3` y `P4` no corresponden a ningún pin del ESP32.** Solo aparecen en el header 2×2, así que esas dos redes no llegarían al microcontrolador aunque se poblaran.
>
> **Recomendación: marcar las seis resistencias y los seis condensadores como DNP explícito en Altium**, y corregir las etiquetas `P6`/`P7` para que no reutilicen nets ya asignados. Tal como está, cualquiera que lea el esquemático sin este documento asumirá que son seis entradas disponibles.

⚠️ **Consecuencia:** con este bloque despoblado, **no hay ninguna entrada de contacto seco cableada al ESP32**. Si el equipo necesita esa función, falta definir por dónde entra — probablemente por los headers `P3`/`P4` de 18 pines, que aún no he podido leer.

## 10. Restricciones críticas para el firmware

### Pines de strapping en uso

| GPIO | Net | Situación |
|---|---|---|
| **12** | `AA1` | MTDI: define el voltaje del flash interno. Debe estar BAJO al arrancar. **Mitigado por el diseño:** con el GPIO en alta impedancia durante el reset, `R1` (10K) y la unión base-emisor de Q1 fijan el pin cerca de 0 V. El LED1 no interfiere (no conduce por debajo de ~1.8 V). **Riesgo bajo, pero medir el pin durante el arranque para confirmar.** |
| **2** | `AA2` | Debe estar bajo o flotante al arrancar. Mismo mecanismo de clamp vía `R8` + Q2. Correcto. |
| **0** | `BOOT0` | Bajo = bootloader. Pull-up `R2` 100k. Correcto. |
| **15** | `P7` (buzzer) | Bajo al arrancar silencia los logs de boot por UART0. El clamp de `R25` + Q12 lo mantiene bajo. **Consecuencia: no verás los mensajes de arranque del ESP32 por consola.** Es cosmético, pero conviene saberlo al depurar. |
| **5** | `RST` (W5500) | Debe estar alto al arrancar. **Compatible con el uso:** el `rst` del W5500 es activo en bajo, así que el estado de reposo (alto) coincide con lo que exige el strapping. El pull-up interno del ESP32 lo mantiene alto durante el boot. Verificar solo que el módulo `U6` no lo fuerce a bajo. |

**Inicialización obligatoria al comienzo de `setup()`:**

```
GPIO12, 2, 16, 4  (AA1–AA4)  →  LOW    (relés desenergizados)
GPIO13 (OUT_AT)              →  LOW    (sin alerta)
GPIO15 (P7, buzzer)          →  LOW    (silencio)
GPIO14 (BP_S)                →  HIGH   (bypass no solicitado — activo en bajo)
GPIO32 (EN_485)              →  LOW    (modo recepción)
GPIO17 (SS_TX)               →  HIGH   (CS W5500 inactivo)
GPIO5  (RST)                 →  HIGH   (W5500 fuera de reset)
```

### Variante del módulo — confirmada

**ESP32-WROOM-32D.** Sin PSRAM, por lo que **GPIO16 y GPIO17 están libres** y el uso que les da el diseño (`AA3` y `SS_TX`) es válido. Este punto queda cerrado.

### Pines de solo entrada (sin pull interno)

`GPIO34` (`BPS_STATUS`), `GPIO35` (`RESET`), `GPIO36` (`RX3`), `GPIO39` (`P1`).
No admiten `OUTPUT` ni `INPUT_PULLUP`. `BPS_STATUS` tiene `R26` (100K) y `RESET` tiene `R10` (4K7) como pull-ups externos.

### Pines prohibidos

`GPIO6`–`GPIO11`: flash SPI interna del módulo. Cualquier uso provoca fallo inmediato.

### ADC

`ADC2` (GPIO0, 2, 4, 12–15, 25–27) **queda inutilizable mientras el WiFi esté activo**. Como las lecturas analógicas van por el ADC I²C externo, esto no debería afectar, salvo que se quiera usar `P1` o `P2` como analógicos.

---

## 11. Resumen de polaridades

| Señal | GPIO | Dir. | Activo en | Estado seguro |
|---|---|---|---|---|
| `AA1` | 12 | OUT | ALTO | 0 |
| `AA2` | 2 | OUT | ALTO | 0 |
| `AA3` | 16 | OUT | ALTO | 0 |
| `AA4` | 4 | OUT | ALTO | 0 |
| `OUT_AT` | 13 | OUT | ALTO | 0 |
| `P7` (buzzer) | 15 | OUT | ALTO | 0 |
| `BP_S` | 14 | OUT | **BAJO** | **1** |
| `BPS_STATUS` | 34 | IN | **BAJO** | — |
| `RESET` (SW1) | 35 | IN | **BAJO** | — |
| `EN_485` | 32 | OUT | ALTO = TX | 0 |
| `SS_TX` (CS W5500) | 17 | OUT | **BAJO** | **1** |
| Entradas contacto seco | — | IN | **BAJO** (pull-up 10K) | — |

**Regla de oro del bypass:** nunca asumir que el bypass está activo por haber escrito `BP_S = 0`. **Siempre confirmar leyendo `BPS_STATUS`**, que refleja el estado físico real del nodo. Esa es la razón de existir de esa señal.

---

## 12. Nets sin función confirmada

| Net | Origen | Estado |
|---|---|---|
| `P1` | GPIO39 | **Reservado, sin uso.** Su red RC no se puebla. Solo-entrada — descartado para la UART de la pantalla por eso mismo (ver §14). |
| `P2` | GPIO25 | **Asignado: IRQ del W5500** (ver §4.1). Sigue colisionando de nombre con el designador del header `P2` (§6.1) — son cosas distintas, no confundir. |
| `P3`, `P4` (nets) | — | Aparecen en el bloque RC despoblado y en el header 2×2. **No llegan a ningún pin del ESP32.** |
| `BP_H` | externo | Bypass por hardware. Falta ver de dónde viene (¿header `P3`/`P4`?). |
| `S1` | colector Q5 | Posible net huérfano (ver §7.4). |
| `S2` | colector Q9 | Posible net huérfano (ver §7.4). |
| `ST1`–`ST4` | ADC `U2` | Canales de temperatura (NTC B3950). Falta el valor nominal del NTC, el divisor, y qué punto físico mide cada uno. |
| `REF` | ADC `U2` | Canal de referencia. |
| `OBL1`–`OBL3` | ADC `U2` | Sin identificar. |
| `P3`, `P4` | headers 18 pines | Regletas de E/S. Nets ilegibles a esta resolución. **No usar para la UART de la pantalla mientras no se confirmen** (ver §14). |
| `P6` | GPIO26 | **Asignado: UART2 TX** hacia la pantalla braindlab (ver §14). |
| Header 2×2 (`P6`) | — | Etiquetas ilegibles. **Colisiona con el net `P6`.** |

> ⚠️ **Colisiones de nombres a corregir:** `P2` y `P6` son a la vez nets y designadores de conector. Vale la pena renombrar los nets a algo descriptivo (`IN_DRY1`, `SPARE_26`, etc.) antes de que cause un error de ERC o una confusión en el firmware.

---

## 14. Enlace UART con la pantalla braindlab

Punto que no existía en versiones anteriores de este documento: esta placa
(el "controlador_braindlab") habla por UART con la pantalla táctil
braindlab (ESP32-S3 + LCD RGB 800×480, ver `braindlab/main/braindlab.c`),
siguiendo el mismo esquema físico que `blink` ↔ `controlador_labgeo`
(cruzado TX/RX + GND común, ver `PROTOCOLO_UART.md`).

| Señal | GPIO | Net | Dirección |
|---|---|---|---|
| TX (hacia RX de la pantalla) | 26 | `P6` | OUT |
| RX (desde TX de la pantalla) | 39 | `P1` | IN |

**Actualizado:** el RX se movió de GPIO25 (`P2`) a GPIO39 (`P1`) — GPIO25
quedó libre para el IRQ del W5500 en su lugar (mejor candidato ahí porque
tiene pull-up interno, que ese pin open-drain necesita; GPIO39 no tiene
pull-up interno pero como RX no le hace falta, solo necesita ser entrada).
Ver conversación de reparto de pines y `red_eth.c`/`sdkconfig.defaults`
(`CONFIG_ETHERNET_SPI_INT0_GPIO`).

**Ya implementado:** protocolo binario propio (`protocolo_braindlab.h`,
idéntico en `controlador_braindlab/main/` y `braindlab/main/`;
`uart_pantalla.c` / `uart_braindlab.c`), mismo framing que
`PROTOCOLO_UART.md` (SOF 0xAA, CRC8, 115200 8N1) pero con el payload propio
de climatización (4 relés `AA1`–`AA4`, 4 NTC, humedad del AM2301A, alarma
AT, bypass, hora del RTC). Ver `PROTOCOLO_UART_BRAINDLAB.md` para el
detalle completo. Pendiente: parámetros eléctricos del medidor por RS-485
(todavía no hay RS-485 implementado en el firmware).

---

## 15. Pendientes — orden de prioridad

**Bloqueantes para fabricar:**

0. **Marcar `U7` como DNP** en el BOM y en la variante de montaje de Altium.
1. Corregir la lógica de bypass a paralelo (OR) si ese es el comportamiento deseado (§7.3).
2. Verificar la numeración de pines de los símbolos S8550 (Q7/Q8) (§7.3).
3. Añadir diodos flyback a todas las bobinas de relé (§2.4).
4. Resolver el margen de dropout del AMS1117 desde 4 V (§2.2).

**Información que falta para completar este documento:**

6. ~~Bloque de las seis redes RC.~~ **Resuelto: no se puebla (§9).**
7. Recorte ampliado de los headers `P3` y `P4` (18 pines cada uno) y del header 2×2 — es donde deben estar las entradas de contacto seco y el origen de `BP_H`.
8. ~~Confirmar el part number del módulo ESP32.~~ **Resuelto: ESP32-WROOM-32D.**
9. Tensión de entrada real en `P1` y existencia del convertidor 12 V → 4 V.
10. Referencias exactas: módulo ADC (`U2`), módulo RTC (`U9`), módulo Ethernet (`U6`), transistores Q1–Q13, relés.
10b. **Valor nominal de los NTC B3950** (10 kΩ o 100 kΩ), valor y topología del divisor, y qué mide el canal `REF` (§4.2). Sin esto no se puede escribir la conversión a grados.
11. Valores de `R22` y `C3`.
12. Origen de `BP_H` y destino de `S1`/`S2`.
13. Presencia y valor de los pull-ups de I²C y del pull-up del sensor `HUM`.
14. Resistencia de terminación de 120 Ω en el bus RS-485.

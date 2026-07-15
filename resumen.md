# Resumen del proyecto — controlador_labgeo (dial digital)

## Objetivo
Sistema de control para ensayos geotécnicos de laboratorio con ESP32
(firmware nativo ESP-IDF, proyecto `controlador_labgeo`). El problema que se
está depurando: un calibre digital (dial) con protocolo BCD de 3 líneas
(REQ/CLK/DATA + GND) no responde correctamente al leerlo desde el ESP32.

## Protocolo del dial (confirmado, no es genérico)
- REQ (salida del ESP32, activo en alto) le pide al calibre que mande una
  lectura.
- CLK y DATA son entradas generadas por el calibre (el ESP32 NO controla el
  reloj, solo lo sondea/"polling").
- El calibre manda 13 dígitos de 4 bits cada uno (52 flancos de CLK en
  total). Cada grupo de 4 bits es BCD con pesos 1,2,4,8 en el orden que se
  clockea (bit menos significativo primero). `k==15` es un caso especial de
  "dígito en blanco" → se trata como 0.
- El DATA se lee DESPUÉS de que CLK vuelve a bajo (flanco de bajada), no
  cerca de cuando sube.
- El valor final ("DD.DDD" mm, ya en resolución de micrómetros) se arma con
  los dígitos 6 al 10. No se extrae el signo todavía (dígitos 0-5/11-12 no
  se usan, ahí podría estar esa info si hiciera falta más adelante).

## Punto crítico de timing
Como el CLK lo genera el calibre (no el ESP32), el RITMO de sondeo importa
tanto como la lógica de decodificación de bits. Los valores ya probados en
Arduino (y que funcionaban en producción, `LABGEO2.ino`/`labgeo2025.ino`)
son:
- Timeout de **100000 intentos por semiflanco** de CLK.
- **10µs de espera entre cada sondeo** (`delayMicroseconds(10)`).
- **2000 ciclos vacíos** de espera de asentamiento antes de leer DATA tras
  el flanco de bajada (no un delay fijo en µs, un busy-loop vacío).

Sondear más rápido o más lento que eso puede hacer perder flancos o leer
DATA antes de que se estabilice.

## Pines usados en las pruebas
- REQ  = GPIO13 (etiqueta placa: OUT-AT)
- CLK  = GPIO25 (P2)
- DATA = GPIO27 (HUM)

## Estado actual de los archivos

### `E:\ESP\controlador_labgeo\main\dial_caliper.c` / `.h`
Es la librería real/reutilizable del firmware. Se acaba de sincronizar para
usar EXACTAMENTE la misma lógica de timing que el `.ino` de referencia
probado (antes usaba un timeout genérico por wall-clock con
`esp_timer_get_time()`, que se reemplazó por el esquema de
iteraciones/polling de arriba). Constantes: `DIAL_TIMEOUT_ITERACIONES=100000`,
`DIAL_POLL_DELAY_US=10`, `DIAL_SETTLE_ITERACIONES=2000`. El parámetro
`timeout_ms` de `dial_caliper_leer()`/`dial_caliper_leer_digitos()` quedó
sin uso (se mantiene en la firma por compatibilidad, documentado en el
header).

### `E:\ESP\controlador_labgeo\main\app_main.c`
**ESTO ES UNA VERSIÓN DE DIAGNÓSTICO, NO EL FIRMWARE NORMAL.** Contiene una
copia casi literal de la función `dial()` original de Arduino, portada
línea por línea a ESP-IDF como `dial_original()`, sin pasar por
`dial_caliper.c`. Se usó para descartar si el problema era del driver
(`dial_caliper.c`) o del hardware. Incluye:
- Logging detallado por dígito y por timeout (qué dígito/bit falló).
- `vTaskDelay(1)` periódico dentro de los busy-waits para no disparar el
  Task Watchdog Timer (pasó una vez al subir mucho el timeout sin ceder
  CPU — el core se bloqueaba tanto que la tarea IDLE0 no alimentaba el
  watchdog).
- Pull-down interno en CLK/DATA (`gpio_set_pull_mode(..., GPIO_PULLDOWN_ONLY)`)
  agregado como prueba barata contra ruido de línea flotante — actualmente
  **comentado/desactivado**.

Para volver al firmware real hay que restaurar `app_main.c` desde git
(quedó commiteado antes de este cambio) — **pendiente, no se ha hecho
todavía**.

### `E:\ESP\prueba_dial_arduino\prueba_dial_arduino.ino`
Sketch standalone para Arduino IDE (framework Arduino, NO ESP-IDF) con los
mismos pines, para descartar que el problema fuera específico de ESP-IDF.
Usa el mismo protocolo con logs por Serial. Al final del archivo (líneas
~119-170) quedó comentada una copia literal, sin modificar, de la función
`dial()` original — esa es la referencia de timing "canónica" que se usó
para sincronizar `dial_caliper.c`.

## Conclusión de las pruebas hasta ahora
El usuario probó tanto el diagnóstico en ESP-IDF (`app_main.c` /
`dial_original()`) como el sketch aislado en Arduino
(`prueba_dial_arduino.ino`) y **ninguno de los dos logró leer el calibre
correctamente** ("no funciona, es por el dial"). Esto apunta fuertemente a
que el problema es del calibre físico / cableado / alimentación, y no del
software o del framework (se descartó ESP-IDF como causa al fallar también
en Arduino puro).

A pesar de esa conclusión, se pidió igual sincronizar `dial_caliper.c`/`.h`
(la librería real) con el timing ya probado, como paso de corrección final
— esto se acaba de completar (ver arriba) pero todavía no fue verificado
contra hardware real por el usuario.

## Pendientes (no iniciados o no confirmados en esta sesión)
1. **Verificar en hardware** si `dial_caliper.c` sincronizado cambia algo
   una vez que se pruebe.
2. Diagnóstico físico/eléctrico del calibre (continuidad, niveles de
   voltaje en REQ/CLK/DATA con multímetro, GND compartido) — probable
   siguiente paso dado que se sospecha del hardware, pero no pedido
   explícitamente todavía.
3. Restaurar/actualizar el firmware real completo de `app_main.c` (ahora
   mismo solo tiene el código de diagnóstico) una vez que la lectura del
   dial esté confirmada funcionando.
4. Resolver problema de conectividad Ethernet/ping — no tocado en esta
   sesión.
5. Resolver la discrepancia de arquitectura: ¿son 2 diales digitales, o 1
   dial + 1 sensor de presión analógico vía ADS1115? (`labgeo2025.ino`
   parece sugerir esto último). Pendiente de definir pines finales.

## Nota sobre errores no relacionados al código
Hubo un error de flasheo con esptool ("Invalid head of packet") en algún
punto — no confirmado si se resolvió, no parece estar relacionado al
código. Causas típicas: monitor serie abierto en otra app, ruido/hub USB,
o el cableado del calibre metiendo ruido durante el flasheo.

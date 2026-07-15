# `dial_caliper.c` / `dial_caliper.h` — driver del calibre digital ("dial")

Este documento explica, para alguien que recien se suma al proyecto, que hace
este archivo, como funciona el protocolo que implementa, que hace cada
funcion exportada y por que la lectura es **sincrona / bloqueante** (y por
que eso no es un problema en este firmware).

Archivos relacionados:
- `controlador_labgeo/main/dial_caliper.h` — la interfaz publica (lo que otros
  modulos pueden usar).
- `controlador_labgeo/main/dial_caliper.c` — la implementacion (el protocolo
  bit a bit).
- `controlador_labgeo/main/app_main.c` — quien usa este driver en la practica
  (`tarea_sensores`).

---

## 1. Que es un "dial caliper" en este proyecto

Es un calibre (pie de rey) digital con salida de 3 cables de datos + GND:

| Señal | Direccion (desde el ESP32) | Para que sirve |
|-------|------------------------------|-----------------|
| `REQ` | **salida** del ESP32 hacia el calibre | "boton" simulado: al ponerlo en alto le pedimos al calibre que empiece a mandar sus datos |
| `CLK` | **entrada** al ESP32, generado por el calibre | reloj que marca cuando hay un bit nuevo listo para leer |
| `DATA`| **entrada** al ESP32, generado por el calibre | el bit en si (0 o 1) |

Esto **no** es el protocolo "24 bits binarios" que usan otros calibres
chinos genericos. Este calibre manda **13 dígitos** de 4 bits cada uno (52
flancos de reloj en total), y cada grupo de 4 bits es un dígito en BCD
(Binary-Coded Decimal: cada dígito decimal 0-9 codificado en binario, en
vez de un numero binario "puro" de 13x4=52 bits).

El protocolo esta portado tal cual desde un `.ino` de Arduino que ya
funcionaba en produccion (ver el comentario del header) — no es algo
inventado ni adivinado, asi que si algo del timing parece "mágico" (los
10us, los 100000 intentos, etc.), es intencional: son los mismos numeros
que ya se probaron en el hardware real.

---

## 2. El protocolo, explicado paso a paso

Cuando el ESP32 pone `REQ` en alto, el calibre empieza a "clockear" sus 13
dígitos, uno detras de otro. Por cada dígito pasa esto 4 veces (una vez por
cada bit del dígito):

1. `CLK` esta en reposo en **bajo**. El calibre lo sube a **alto** cuando
   tiene un bit listo → esto es el **flanco de subida**.
2. Un rato despues, el calibre vuelve a bajar `CLK` a **bajo** → esto es el
   **flanco de bajada**.
3. **Recien despues del flanco de bajada** (no cerca de la subida) hay que
   leer `DATA`. Este es el detalle mas importante y el que, segun el
   comentario del código, probablemente fallaba en una version anterior del
   driver: si lees `DATA` en el momento equivocado, lees basura.
4. Los 4 bits de un dígito se combinan con pesos **1, 2, 4, 8** (orden
   normal de BCD), y se arma un numero 0-9. Si el resultado da **15** (un
   caso especial del calibre), se interpreta como dígito "en blanco" y se
   guarda como `0`.

Esto se repite 13 veces → se arma un array de 13 caracteres `'0'`-`'9'`.

De esos 13 dígitos, **solo los índices 6 a 10** (5 dígitos) forman el valor
que nos interesa, con formato `DD.DDD` (2 enteros + 3 decimales, en
milímetros). Como ya viene con 3 decimales, el valor final que devuelve el
driver **ya está en micrómetros** (0.001 mm) sin necesidad de reescalar. Los
demás dígitos (0-5 y 11-12) no se usan en esta version (podrían tener
información de signo, pero eso queda para mas adelante si hace falta leer
valores negativos).

---

## 3. Funciones exportadas (la API publica)

### `void dial_caliper_init(dial_caliper_t *d, gpio_num_t pin_req, gpio_num_t pin_clk, gpio_num_t pin_data);`

**Que hace:** configura los 3 pines GPIO y deja el struct `dial_caliper_t`
listo para usarse. Se llama **una sola vez**, al arrancar el firmware (en
`app_main()`), una vez por cada calibre físico que haya conectado (en este
proyecto hay dos: `s_dial1` y `s_dial2`).

**Como se usa:**

```c
static dial_caliper_t s_dial1;

// en app_main(), durante la inicializacion:
dial_caliper_init(&s_dial1, PIN_DIAL1_REQ, PIN_DIAL1_CLK, PIN_DIAL1_DATA);
```

**Que hace por dentro:**
- `pin_req` se configura como **salida** y se deja en el estado "liberado"
  (REQ inactivo) — todavia no le pedimos nada al calibre.
- `pin_clk` y `pin_data` se configuran como **entrada**, porque son señales
  que genera el calibre, no el ESP32.

Importante: `dial_caliper_init` **no lee nada todavía**. Solo deja los
pines en el modo correcto. La lectura real pasa cuando se llama a
`dial_caliper_leer` o `dial_caliper_leer_digitos`.

---

### `bool dial_caliper_leer_digitos(dial_caliper_t *d, char digitos[13], uint32_t timeout_ms);`

**Que hace:** ejecuta el protocolo completo descripto en la sección 2 y
devuelve los **13 dígitos crudos**, sin decodificar a un número final. Es la
función de bajo nivel — la usa internamente `dial_caliper_leer()`, pero
también se puede llamar directo para **depurar**: si el valor final no
cierra (por ejemplo el número final no tiene sentido), conviene mirar los
13 dígitos crudos para entender qué está mandando el calibre en cada
posición, no solo los 5 que se usan para el valor final.

**Como se usa:**

```c
char digitos[13];
if (dial_caliper_leer_digitos(&s_dial1, digitos, 50)) {
    // digitos[0..12] son caracteres '0'-'9'
    ESP_LOGI(TAG, "digitos crudos: %.13s", digitos);
} else {
    ESP_LOGW(TAG, "timeout leyendo el calibre");
}
```

**Parametro `timeout_ms`:** ⚠️ **atención**, este parámetro está en la firma
pero **no se usa** (hay un `(void)timeout_ms;` explícito en el código). Se
mantiene solo para no romper la compatibilidad de quien ya llama a esta
función con ese argumento. El timeout real está fijado internamente (ver
sección 4) y es mucho más específico que un simple timeout en milisegundos
de reloj de pared — está pensado en "intentos de sondeo", no en tiempo.

**Valor de retorno:**
- `true` → se leyeron los 13 dígitos correctamente, `digitos[]` es válido.
- `false` → se agotó el timeout esperando un flanco de `CLK` (el calibre no
  respondió, está desconectado, o el cable está suelto). En este caso
  `digitos[]` puede quedar parcialmente escrito, no hay que usarlo.

**Efecto secundario importante:** esta función pone `REQ` en alto al
empezar y lo vuelve a poner en el estado "liberado" al final (tanto si sale
por éxito como si sale por timeout). O sea, cada llamada es una
transacción completa: pide, lee, libera.

---

### `bool dial_caliper_leer(dial_caliper_t *d, int32_t *valor_um, uint32_t timeout_ms);`

**Que hace:** es la función que se usa normalmente en el firmware. Llama
internamente a `dial_caliper_leer_digitos()` y después decodifica los
dígitos 6 a 10 (`DD.DDD` mm) a un entero en **micrómetros**.

**Como se usa** (tal como se usa hoy en `app_main.c`, dentro de
`tarea_sensores`):

```c
int32_t dial1_crudo = 0;
bool ok1 = dial_caliper_leer(&s_dial1, &dial1_crudo, 50);

if (!ok1) {
    ESP_LOGW(TAG, "Dial 1: timeout de lectura");
} else {
    // dial1_crudo esta en micrometros (0.001 mm)
}
```

**Parametro `timeout_ms`:** igual que en `dial_caliper_leer_digitos`, no se
usa realmente (se pasa de largo). Queda en la firma por compatibilidad.

**Valor de retorno:**
- `true` → `*valor_um` tiene la posición leída, en micrómetros.
- `false` → timeout, `*valor_um` no se toca (queda con el valor que tenía
  antes de llamar a la función — por eso en `app_main.c` se inicializa en
  `0` antes de llamar).

**Nota sobre el nombre de la variable vs. la unidad real:** en
`app_main.c` la variable que recibe este valor se llama `dial1_crudo` y
luego se divide por `100.0f` antes de calibrar — es decir, en ese punto del
código se la trata como si viniera en centésimas de mm. Vale la pena
tenerlo presente si se toca esa parte: `dial_caliper_leer` ya entrega
micrómetros (3 decimales), así que hay que revisar con cuidado esa cuenta
si se cambia algo ahí (no es parte de este archivo, pero es el consumidor
inmediato).

---

## 4. La lógica interna de `dial_caliper_leer_digitos` (línea por línea, en criollo)

```c
req_activar(d);                     // 1. "Che calibre, mandame tus datos"

for (int i = 0; i < 13; i++) {      // 2. Por cada uno de los 13 digitos...
    int k = 0;

    for (int j = 0; j < 4; j++) {   // 3. ...leer sus 4 bits (uno por vez)

        // 4. Esperar flanco de SUBIDA de CLK (el calibre avisa "bit listo")
        while (gpio_get_level(d->pin_clk) == 0) {
            esp_rom_delay_us(10);
            if (timeout--) == 0) return false;  // el calibre no respondio
        }

        // 5. Esperar flanco de BAJADA de CLK
        while (gpio_get_level(d->pin_clk) == 1) {
            esp_rom_delay_us(10);
            if (timeout--) == 0) return false;
        }

        // 6. Esperar un ratito mas (2000 vueltas de un loop vacio) a que
        //    la señal DATA se "asiente" (settle) antes de leerla
        espera = 2000; while(espera--);

        // 7. RECIEN ACA se lee el bit
        int bit = gpio_get_level(d->pin_data);
        if (bit) { /* sumar el peso 1,2,4 u 8 segun j al digito k */ }
    }

    if (k == 15) k = 0;             // 8. caso especial: "digito en blanco"
    digitos[i] = k + '0';           // 9. convertir a caracter '0'-'9'
}

req_liberar(d);                     // 10. "Ya termine, gracias"
return true;
```

**Por qué el sondeo (polling) tiene que ser a un ritmo específico:**

El ESP32 **no genera** el reloj (`CLK`) — lo genera el calibre. El ESP32
solo lo está mirando todo el tiempo (polling) para darse cuenta cuándo
cambia. Esto tiene una consecuencia importante: si el ESP32 mira muy poco
seguido (por ejemplo cada 1ms en vez de cada 10us), puede que se "pierda"
un flanco entero porque el calibre ya subió y bajó `CLK` entre dos
sondeos. Y si mira demasiado seguido puede leer `DATA` antes de que la
señal se haya estabilizado eléctricamente después del flanco. Por eso los
3 números mágicos del código son fijos y calcados del `.ino` que ya
funcionaba, y **no** son un timeout genérico "por reloj de pared":

- `DIAL_POLL_DELAY_US = 10` → cada cuánto se vuelve a mirar el pin mientras
  se espera un flanco (10 microsegundos).
- `DIAL_TIMEOUT_ITERACIONES = 100000` → cuántas veces como máximo se
  reintenta antes de rendirse y devolver `false` (100000 x 10us ≈ 1
  segundo de espera máxima **por cada semiflanco** — hay 2 semiflancos por
  bit y 4 bits por dígito, así que en el peor caso el timeout total podría
  ser bastante más largo que 1 segundo si cada semiflanco individual tarda
  casi el máximo).
- `DIAL_SETTLE_ITERACIONES = 2000` → después del flanco de bajada, cuántas
  vueltas de un loop vacío se esperan antes de leer `DATA`, para darle
  tiempo a la señal de estabilizarse. Es un loop vacío (no un delay en
  microsegundos) marcado `volatile` a propósito, para que el compilador no
  lo "optimice" y lo borre (sin `volatile`, el compilador podría darse
  cuenta de que ese loop no tiene ningún efecto observable y eliminarlo
  entero).

---

## 5. ¿Es síncrono? Sí — y por qué eso está bien acá

**Sí, es completamente síncrono / bloqueante.** Cuando se llama a
`dial_caliper_leer()` (o `_leer_digitos()`), la función **no vuelve** hasta
que:
- terminó de leer los 13 dígitos completos (caso éxito), o
- se agotó el timeout esperando algún flanco (caso `false`).

No hay interrupciones (`gpio_isr_...`), no hay callbacks, no hay nada
asíncrono: es un `while` que consulta el pin una y otra vez (esto se llama
**polling** o **busy-wait**) hasta que pasa lo que se está esperando.

### ¿Por qué no usar interrupciones, que suena más "moderno"?

Se podría, pero:
1. El código ya estaba probado en producción como polling (en el `.ino` de
   Arduino) y portarlo tal cual reduce el riesgo de introducir un bug
   nuevo por cambiar el mecanismo de sincronización.
2. El timing es muy ajustado y específico (10us entre sondeos), algo que
   con interrupciones habría que replicar igual de fino, sin ganar mucho.
3. El calibre solo se lee unas pocas veces por segundo (ver más abajo), no
   hace falta la eficiencia extra de un esquema por interrupciones.

### ¿No bloquea todo el firmware entonces?

**No**, y esta es la parte importante para entender el diseño completo.
Mirá `app_main.c`: la lectura de los calibres pasa **dentro de su propia
tarea de FreeRTOS**, `tarea_sensores()`, creada con `xTaskCreate` aparte de
la tarea que atiende el UART y la que atiende el servidor web:

```c
// Corre en su propia tarea de FreeRTOS (ver xTaskCreate en app_main),
// separada de la tarea que atiende el UART y de la que atiende el
// servidor web, para que ninguna bloquee a las otras.
static void tarea_sensores(void *arg) { ... }
```

O sea:
- **Dentro de `tarea_sensores`**, sí, la ejecución es 100% secuencial y
  bloqueante: se lee el dial 1, después el dial 2, después la celda de
  carga, uno detrás del otro, esperando cada uno.
- **Pero `tarea_sensores` es una tarea de FreeRTOS entre varias.** El ESP32
  corre un sistema operativo de tiempo real (FreeRTOS) con *scheduler*
  preemptivo: mientras `tarea_sensores` está en uno de esos `while`
  esperando un flanco, en cada "tick" del sistema el scheduler puede
  darle tiempo de CPU a las otras tareas (UART, servidor web) si tienen
  igual o mayor prioridad. Así que aunque leer un calibre tarde varios
  milisegundos, **el resto del firmware sigue respondiendo**.
- Además cada lectura individual tiene, en la práctica, un timeout acotado
  (se llama con `50` como argumento, aunque como vimos ese número
  puntual no se usa — el timeout real interno es del orden de fracciones
  de segundo por semiflanco). Si un calibre está desconectado, la función
  devuelve `false` rápido en vez de trabarse para siempre, y el `while(1)`
  de `tarea_sensores` sigue dando la vuelta.

**Nota técnica sobre `esp_rom_delay_us`:** este delay de 10us es un
**busy-wait real** (quema CPU activamente, no le devuelve el control al
scheduler mientras espera), a diferencia de `vTaskDelay()` que sí cede la
CPU a otras tareas. Pero como son solo 10 microsegundos por vez (muy corto
comparado con un tick típico de FreeRTOS, que suele ser de 1ms o más), en
la práctica no le quita una porción de tiempo perceptible a las demás
tareas — el scheduler retoma el control normalmente en el siguiente tick.

**En resumen:** la lectura del calibre en sí es síncrona porque así estaba
probado y funciona bien para este caso (se lee unas pocas veces por
segundo, no hace falta más), pero el *diseño general* del firmware evita
que ese bloqueo afecte al resto del sistema, corriéndolo en su propia
tarea de FreeRTOS separada de todo lo demás.

---

## 6. Resumen rápido para usar este driver en código nuevo

1. Declarar un `dial_caliper_t` (uno por cada calibre físico).
2. Llamar `dial_caliper_init()` una vez, al arrancar, pasando los 3 pines.
3. En un loop (idealmente en su propia tarea de FreeRTOS si se va a leer
   seguido), llamar `dial_caliper_leer()` para obtener el valor en
   micrómetros, o `dial_caliper_leer_digitos()` si se necesita depurar
   viendo los 13 dígitos crudos.
4. Siempre chequear el valor de retorno (`bool`) antes de usar el
   resultado — `false` significa que no llegó nada del calibre a tiempo.
5. No hace falta preocuparse por dejar `REQ` en algún estado en
   particular entre llamadas: cada llamada a `dial_caliper_leer[_digitos]`
   activa y libera `REQ` por su cuenta.

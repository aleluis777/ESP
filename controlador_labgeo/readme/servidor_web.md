# `servidor_web.c` / `servidor_web.h` — servidor HTTP + WebSocket

Este documento explica, para alguien que recién se suma al proyecto, qué
hace este módulo, cómo se usan sus funciones exportadas, y si el envío de
datos por WebSocket es síncrono o asíncrono.

Archivos relacionados:
- `controlador_labgeo/main/servidor_web.h` — la interfaz pública (2
  funciones).
- `controlador_labgeo/main/servidor_web.c` — la implementación.
- `controlador_labgeo/main/red_eth.c` — debe inicializarse **antes** que
  este módulo (ver `red_eth.md`), porque sin red no hay servidor HTTP
  posible.
- `controlador_labgeo/main/app_main.c` — quién lo usa (`app_main()` para
  iniciarlo, `tarea_sensores()` para mandar datos en vivo).
- `controlador_labgeo/web/` — el sitio estático (`index.html`, `style.css`,
  `script.js`) que este módulo sirve.

---

## 1. Qué es y para qué sirve

Este módulo levanta un servidor HTTP normal (usando `esp_http_server`,
el componente estándar de ESP-IDF) que hace **dos cosas** al mismo tiempo:

1. **Sirve un sitio web estático** (`index.html`, `style.css`,
   `script.js`) — la interfaz que ve un usuario si abre la IP del
   controlador en un navegador. Estos archivos no viven en el código
   fuente compilado: se graban en una partición flash separada llamada
   SPIFFS (`www`) al momento de compilar/flashear, a partir de la carpeta
   `../web`.
2. **Ofrece un WebSocket en `/ws`** — un canal en tiempo real por el cual
   el controlador empuja datos de los sensores (peso, posición de los dos
   calibres, estado de la corrida) a cualquier navegador conectado, varias
   veces por segundo, sin que el navegador tenga que estar pidiéndolos
   (a diferencia de HTTP normal, acá el controlador "empuja" los datos).

Lo que **no** hace todavía (aclarado en el propio header): los endpoints
REST del proyecto anterior (OTA, configuración, historial) no están
portados — esto solo trae el servidor de archivos + el canal en vivo.

---

## 2. Funciones exportadas (la API pública)

### `esp_err_t servidor_web_init(void);`

**Qué hace:** monta el sitio estático desde SPIFFS, arranca el servidor
HTTP y registra todas las rutas (`/`, `/index.html`, `/style.css`,
`/script.js`, `/ws`). Se llama **una sola vez**, al arrancar el firmware,
**después** de `red_eth_init()` (sin red, no tiene sentido tener un
servidor HTTP).

**Cómo se usa** (tal cual está en `app_main.c`):

```c
if (red_eth_init() == ESP_OK) {
    servidor_web_init();
} else {
    ESP_LOGE(TAG, "Ethernet no disponible, el servidor web no se inicia");
}
```

**Valor de retorno:**
- `ESP_OK` → el servidor HTTP arrancó y las rutas quedaron registradas.
- distinto de `ESP_OK` → falló `httpd_start()` (por ejemplo, sin memoria o
  sin sockets disponibles). En este caso, `servidor_web_enviar_ws()` sigue
  siendo segura de llamar (ver más abajo, chequea `s_servidor == NULL`),
  simplemente no hace nada.

**Nota:** si el sitio estático no se pudo montar (`montar_www()` falla —
por ejemplo, porque nunca se flasheó la imagen de `../web`), el servidor
**igual arranca**. Solo que pedir `/` va a devolver un 404 en vez de la
página. El WebSocket funciona igual, independiente de si el sitio estático
está disponible o no — son dos cosas separadas dentro del mismo servidor.

---

### `void servidor_web_enviar_ws(const char *json);`

**Qué hace:** manda el string `json` (ya armado por quien llama, terminado
en `'\0'`) como un frame de texto a **todos** los clientes WebSocket
conectados en `/ws` en ese momento. Está pensada para llamarse a la misma
cadencia que se manda por UART hacia la pantalla — hasta 5 veces por
segundo — para que cualquier navegador conectado vea los datos "en vivo".

**Cómo se usa** (tal cual está en `app_main.c`, dentro de
`tarea_sensores`):

```c
char json[256];
snprintf(json, sizeof(json),
         "{\"run_id\":%u,\"activa\":%s,\"dial1_mm\":%.3f,\"dial2_mm\":%.3f,"
         "\"peso_N\":%.3f,\"tiempo_ms\":%" PRIu32 "}",
         (unsigned)s_prog.run_id, s_prog.activa ? "true" : "false",
         dial1_mm, dial2_mm, peso_N, tiempo_ms);
servidor_web_enviar_ws(json);

vTaskDelay(pdMS_TO_TICKS(200)); // ~5 Hz
```

O sea: en cada vuelta del loop de sensores, después de leer y calibrar los
3 sensores, se arma un JSON a mano con `snprintf` y se lo manda tanto por
UART (a la pantalla física) como por este WebSocket (a cualquier navegador
que esté mirando).

**Si no hay clientes conectados:** no pasa nada, la función simplemente no
tiene a quién mandarle nada — internamente recorre la lista de sockets
activos y no encuentra ninguno marcado como WebSocket, así que el `for`
no itera. No hay que chequear "¿hay algún cliente?" antes de llamarla.

**Si el servidor nunca arrancó** (`servidor_web_init()` falló o no se
llamó, por ejemplo porque no había Ethernet): la función chequea
`s_servidor == NULL` al principio y vuelve inmediatamente. Es segura de
llamar siempre, sin necesidad de que quien la usa sepa si el servidor está
realmente arriba.

---

## 3. Qué hace por dentro, lo importante

### Servir archivos estáticos (`archivo_estatico_handler`)

Los 3 archivos del sitio (`index.html`, `style.css`, `script.js`) se sirven
todos con el **mismo handler genérico**, que recibe por `user_ctx` un
struct chiquito (`archivo_estatico_t`) con la ruta en SPIFFS y el
`Content-Type` a mandar. El archivo se lee y se manda **en pedazos de 512
bytes** (`httpd_resp_send_chunk`), no se carga entero en RAM — así funciona
igual de bien para un archivo de 1KB que para uno de 100KB, sin arriesgarse
a quedarse sin memoria con archivos grandes.

`"/"` y `"/index.html"` apuntan al mismo handler y al mismo archivo, para
que entrar directo a la IP del controlador ya muestre la página sin tener
que escribir la ruta completa.

### El WebSocket (`ws_handler` + `servidor_web_enviar_ws`)

Este canal es **de una sola vía**: controlador → navegador. El servidor
**no espera nada del cliente**. `ws_handler` solo hace dos cosas según el
tipo de pedido:
- Si es el **handshake** inicial (`HTTP_GET`), solo loguea que se conectó
  un cliente nuevo y devuelve `ESP_OK` — acepta la conexión.
- Si llega un **frame** del cliente después de conectado, se lee y se
  descarta (`httpd_ws_recv_frame`) — no importa qué mande el navegador, se
  vacía el buffer y ya. Esto es necesario aunque no nos interese el
  contenido: si no se lee el frame entrante, puede quedar trabado el
  socket.

El envío en sí (`servidor_web_enviar_ws`) usa
`httpd_ws_send_frame_async()`, que — como dice el comentario del código —
se puede llamar **desde cualquier tarea de FreeRTOS**, no hace falta estar
"dentro" de un handler HTTP del propio servidor. Por eso `tarea_sensores`
(que es una tarea completamente distinta, separada del servidor web) puede
invocar esta función directamente sin ningún truco extra.

### Límite de clientes simultáneos

`WS_MAX_CLIENTES = 4`. El comentario explica por qué es un número bajo: el
proyecto usa la configuración por default de `LWIP_MAX_SOCKETS` (7
sockets en total), y `esp_http_server` ya se queda con 3 para sí mismo, así
que queda poco margen para clientes WebSocket simultáneos. Si hiciera
falta soportar más clientes a la vez, hay que subir
`CONFIG_LWIP_MAX_SOCKETS` en `sdkconfig.defaults` — no alcanza con tocar
este `.c`.

---

## 4. ¿Es síncrono o asíncrono?

Hay que distinguir **dos partes bien distintas** del módulo:

### `servidor_web_init()` — síncrona

Es una función normal: monta SPIFFS, arranca el servidor, registra rutas,
y **vuelve** cuando terminó. No hay nada asíncrono en la inicialización en
sí. Como toda esta secuencia es rápida (no hay esperas de red ni de
hardware externo, a diferencia de `red_eth_init()`), no bloquea de forma
perceptible el arranque del resto del firmware.

### El servidor en sí, una vez arrancado — asíncrono / dirigido por eventos

Acá está el punto más importante para entender el diseño: **una vez que
`servidor_web_init()` retorna, el servidor HTTP corre completamente solo,
en su propia tarea interna de FreeRTOS que crea `esp_http_server`**. Nadie
en este proyecto tiene que "atenderlo" en un loop — cuando llega un pedido
HTTP o un mensaje de WebSocket, ESP-IDF despierta esa tarea interna, llama
al handler correspondiente (`archivo_estatico_handler` o `ws_handler`), y
se vuelve a dormir hasta el próximo pedido. Esto es un modelo **orientado a
eventos**: el código de este archivo no decide *cuándo* se ejecutan los
handlers, solo *qué* hacen cuando se ejecutan.

Esto es justamente lo que permite que `tarea_sensores` (otra tarea
completamente distinta, ver `dial_caliper.md` sección 5 para más detalle
sobre esa tarea) le pueda **empujar** datos al servidor con
`servidor_web_enviar_ws()` sin coordinarse con él de ninguna manera
especial: son dos tareas de FreeRTOS separadas, cada una con su propio
ritmo:

- La tarea interna del servidor HTTP reacciona a eventos externos
  (pedidos de navegadores) — no tiene un ritmo fijo propio.
- `tarea_sensores` corre en un loop propio a ~5 Hz (`vTaskDelay(200ms)`) y
  en cada vuelta le "avisa" al servidor que mande el JSON más reciente,
  vía una llamada a función que — como ya vimos — está diseñada para
  poder invocarse desde cualquier tarea.

`servidor_web_enviar_ws()` en sí misma sí es una llamada síncrona (no
devuelve una promesa ni nada por el estilo — hace su trabajo y vuelve), pero
lo que hace puertas adentro (`httpd_ws_send_frame_async`) está pensado para
no bloquear a `tarea_sensores` esperando que cada cliente WebSocket reciba
el frame: encola el envío y vuelve. Por eso el comentario del header dice
explícitamente "no bloquea si no hay clientes" — y en la práctica, tampoco
bloquea de forma relevante aunque sí los haya.

**En resumen:** iniciar el servidor es síncrono y rápido; una vez arriba,
todo lo que pasa con clientes HTTP/WebSocket es asíncrono y manejado por
la tarea interna de `esp_http_server`, y mandar datos hacia los clientes
desde `tarea_sensores` es una llamada que no bloquea esperando que lleguen
a destino.

---

## 5. Resumen rápido para usar este módulo en código nuevo

1. Llamar `red_eth_init()` primero, y solo si devuelve `ESP_OK`, llamar
   `servidor_web_init()` — sin red no hay servidor útil.
2. `servidor_web_init()` se llama una sola vez al arrancar. No hay que
   "atender" el servidor en ningún loop propio: ESP-IDF lo hace solo en
   su tarea interna.
3. Para mandar datos en vivo a los navegadores conectados, armar un JSON
   (por ejemplo con `snprintf`) y llamar `servidor_web_enviar_ws(json)` —
   es seguro llamarla siempre, aunque no haya clientes conectados o el
   servidor no haya arrancado.
4. Si se necesita agregar una ruta HTTP nueva (por ejemplo, un endpoint
   REST), el patrón es: crear un handler `esp_err_t mi_handler(httpd_req_t
   *req)`, declarar un `httpd_uri_t` con la ruta/método/handler, y
   registrarlo con `httpd_register_uri_handler()` dentro de
   `servidor_web_init()`, igual que las rutas existentes.
5. Si hace falta soportar más de 4 clientes WebSocket a la vez, subir
   `CONFIG_LWIP_MAX_SOCKETS` en `sdkconfig.defaults` (y ajustar
   `WS_MAX_CLIENTES` acá) — no alcanza con cambiar solo este archivo.

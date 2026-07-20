# controlador_labgeo

Firmware del ESP32 "controlador": lee los sensores (celda de carga + 2 diales),
corre el cronograma de cada corrida (consolidación/edómetro), guarda el
historial en flash, y expone todo por dos canales: UART hacia la pantalla
LabGeo (ESP32-S3) y WebSocket hacia cualquier navegador en la red local.

No hay una sola "clase" que haga todo -- cada archivo es un modulo chico con
una responsabilidad, y `app_main.c` es el unico que los conoce a todos y los
conecta entre si. Si querés entender el programa, `app_main.c` es el punto
de entrada correcto: lee ese archivo de arriba a abajo y andá a cada módulo
cuando necesites más detalle.

## Mapa del flujo de datos

```
                    ┌─────────────┐        ┌──────────────────┐
   HX711  ────────► │             │        │  Pantalla ESP32-S3│
   (hx711.c)        │             │  UART  │  (btn Start/Stop, │
                     │             │◄──────►│   pide graficas)  │
   Dial 1 ────────► │             │        └──────────────────┘
   Dial 2            │ app_main.c │
   (dial_caliper.c) │  (tarea_    │        ┌──────────────────┐
                     │  sensores) │   WS   │  Navegador web    │
                     │             │───────►│  (monitoreo live) │
                     │             │        └──────────────────┘
                     └──────┬──────┘
                            │
              ┌─────────────┼─────────────┐
              ▼             ▼             ▼
      config_labgeo.c  programador_   almacenamiento.c
      (calibracion,    corrida.c     (historial de cada
       NVS)            (cuando       corrida, SPIFFS)
                        guardar un
                        punto)
```

`red_eth.c` levanta el Ethernet (W5500) para que `servidor_web.c` (el
WebSocket) tenga red disponible. Son independientes del resto -- si el
Ethernet falla, sensores y UART siguen funcionando igual (ver `app_main.c`,
el `if (red_eth_init() == ESP_OK)`).

## `app_main.c` paso a paso

### `app_main()` -- arranque, corre una sola vez

1. `config_labgeo_init()` + `config_labgeo_cargar()`: levanta NVS y carga la
   calibración guardada (pendiente/offset de cada sensor) en `s_cal`. Si
   nunca se calibró nada, usa valores neutros (pendiente=1, offset=0).
2. `almacenamiento_init()`: monta SPIFFS (la partición `storage` de
   `partitions.csv`), donde se van a guardar los archivos `corrida1.dat` /
   `corrida2.dat` con el historial.
3. `hx711_init()` / `dial_caliper_init()` x2: configuran los pines de cada
   sensor como entrada/salida. No leen nada todavía, solo dejan el hardware
   listo.
4. `uart_link_set_callbacks()` + `uart_link_init()`: registra qué función
   llamar cuando llega un `START`/`STOP`/`REQUEST_RUN` desde la pantalla
   (`on_start`, `on_stop`, `on_request_run`, definidas arriba), y recién
   ahí abre el UART y lanza la tarea que escucha.
5. `red_eth_init()` + `servidor_web_init()`: si el Ethernet levanta bien,
   arranca el servidor HTTP/WebSocket. Si no, solo loguea el error y sigue
   (no es fatal -- sensores y pantalla no dependen de esto).
6. `xTaskCreate(tarea_sensores, ...)`: recién acá arranca el loop que lee
   sensores en repetición. Todo lo anterior es puro setup.

### `tarea_sensores()` -- el loop principal, corre para siempre a ~5 Hz

Es una tarea de FreeRTOS separada (no bloquea `app_main`). En cada vuelta:

1. Lee los 3 sensores (`dial_caliper_leer()` x2, `hx711_leer()`). Cada
   lectura tiene un timeout de 50ms -- si el sensor no responde, la función
   devuelve `false` y se loguea un warning, pero el loop sigue (no se
   traba esperando un sensor desconectado).
2. Convierte de unidades crudas a unidades de ingeniería aplicando la
   calibración (`s_cal`): los diales dan centésimas de mm crudas, se pasan
   a mm y se aplica `pendiente`/`offset`; la celda aplica la misma fórmula
   que usaba el `.ino` viejo (`(crudo - offset) / pendiente`).
3. Convierte esos valores de ingeniería a las unidades enteras que usa el
   protocolo (micrómetros para los diales, milinewtons para el peso) --
   ver `protocolo_labgeo.h` / `PROTOCOLO_UART.md` para el porqué de enteros
   en vez de floats en el cable.
4. `programador_actualizar()`: le pasa los valores actuales al programador
   de la corrida activa. Internamente decide si "toca" un checkpoint (por
   tiempo en la Corrida 1, por desplazamiento del Dial 2 en la Corrida 2) y,
   si toca, guarda el punto en el archivo de esa corrida.
5. Si hay una corrida activa, manda `SENSOR_UPDATE` por UART a la pantalla
   (en reposo no manda nada -- ver `PROTOCOLO_UART.md`).
6. Siempre (haya o no corrida activa) arma un JSON chico y lo manda por
   WebSocket a quien esté conectado en `/ws` -- este es el canal "siempre
   encendido" para monitoreo remoto.
7. `vTaskDelay(200ms)` y vuelve a empezar (~5 lecturas por segundo).

### Los 3 callbacks (`on_start`, `on_stop`, `on_request_run`)

Se los pasás a `uart_link_set_callbacks()` y `uart_link.c` los invoca solo
cuando le llega la trama correspondiente por UART -- `app_main.c` no sabe
nada del formato de las tramas, eso es responsabilidad de `uart_link.c`.

- `on_start(run_id)`: le dice a `programador_corrida.c` que arranque el
  cronómetro y el archivo de esa corrida.
- `on_stop(run_id)`: para el cronómetro (el archivo con lo ya guardado
  queda como está, no se borra).
- `on_request_run(run_id)`: lee el archivo de esa corrida
  (`almacenamiento_leer_corrida`) y, a medida que va leyendo bloques de 16
  puntos, los reenvía por UART como tramas `RUN_CHUNK`
  (`uart_link_enviar_run_chunk`) vía el callback `chunk_cb`.

## Los demás archivos, uno por uno

| Archivo | Qué hace |
|---|---|
| `protocolo_labgeo.h` | Constantes y helpers del protocolo UART (frame, CRC8, lectura/escritura little-endian). **Debe ser idéntico** al mismo archivo del lado de la pantalla. |
| `uart_link.c/.h` | Arma y parsea las tramas UART. Tarea propia que lee byte a byte con una máquina de estados; llama a los callbacks de `app_main.c` cuando reconoce un comando válido (CRC ok). |
| `hx711.c/.h` | Driver bit-bang del HX711 (celda de carga): 24 pulsos de reloj, lee 24 bits con signo. |
| `dial_caliper.c/.h` | Driver bit-bang del protocolo de calibre digital (REQ/CLK/DATA). **Sin validar contra hardware real todavía** -- tiene `#define` al principio del `.c` para ajustar polaridad/orden de bits una vez que se pruebe. |
| `config_labgeo.c/.h` | Calibración (pendiente/offset de cada sensor) guardada en NVS. Reemplaza la EEPROM del `.ino` viejo. |
| `almacenamiento.c/.h` | Guarda/lee el historial de cada corrida en SPIFFS. Un archivo por corrida, 16 bytes por punto (mismo layout que la trama `RUN_CHUNK`, para no tener que reempacar al mandarlo). |
| `programador_corrida.c/.h` | El cronograma de cada corrida: tiempos fijos (Corrida 1) o umbrales de desplazamiento (Corrida 2). Decide *cuándo* se guarda un punto. |
| `red_eth.c/.h` | Levanta el W5500 (Ethernet) usando el componente `espressif/ethernet_init`. IP estática. |
| `servidor_web.c/.h` | Servidor HTTP + endpoint WebSocket `/ws`. `servidor_web_enviar_ws()` le manda un string a todos los clientes conectados. Loguea (INFO) cada peticion HTTP que entra: metodo, URI e IP del cliente. |
| `ping_monitor.c/.h` | Observador de pings ICMP: loguea cada echo request que le llega al controlador (IP de origen, id, seq). No contesta nada -- lwIP ya responde el ping por su cuenta, esto solo mira via un socket RAW aparte. |

## Cosas que todavía no están (a propósito, no por olvido)

- El sitio web estático (html/css/js) y el resto de los endpoints REST del
  `LinaresETH` viejo (OTA, `/config.json`, calibración por HTTP) no están
  portados -- solo el WebSocket de monitoreo en vivo.
- El protocolo del dial digital no está validado contra hardware real (ver
  el comentario grande en `dial_caliper.h`).
- Detectamos que `Tiempo_corrida()` en `labgeo2025.ino` (la función que
  realmente corre para la Corrida 1) usa un esquema distinto al que
  implementamos (`programador_corrida.c` usa el esquema clásico por
  tiempos fijos, confirmado como el correcto en la conversación).

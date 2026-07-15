# `red_eth.c` / `red_eth.h` — arranque de Ethernet (W5500)

Este documento explica, para alguien que recién se suma al proyecto, qué
hace este módulo, cómo se usa su única función exportada, y si su
inicialización es síncrona o asíncrona (y qué implica eso).

Archivos relacionados:
- `controlador_labgeo/main/red_eth.h` — la interfaz pública (una sola
  función).
- `controlador_labgeo/main/red_eth.c` — la implementación.
- `controlador_labgeo/main/app_main.c` — quién lo usa (`app_main()`, antes
  de levantar el servidor web).
- `controlador_labgeo/sdkconfig.defaults` — dónde vive la configuración de
  pines del chip W5500 (no en este `.c`, ver sección 4).

---

## 1. Qué es y para qué sirve

El controlador tiene conectividad de red por **cable** (Ethernet), no por
WiFi. El chip que hace de puente entre el ESP32 y el cable de red es un
**W5500**, un controlador Ethernet que se comunica con el ESP32 por **SPI**
(no tiene stack TCP/IP propio expuesto — solo hace de "tarjeta de red").

Este archivo se encarga de:
1. Inicializar el chip W5500 (vía un componente externo de Espressif).
2. Conectarlo al stack de red genérico de ESP-IDF (`esp_netif`), que es lo
   que le permite a lo demás (servidor HTTP, sockets, etc.) usarlo como
   cualquier interfaz de red.
3. Asignarle una **IP fija** (no usa DHCP), la misma que ya usaba el
   firmware Arduino anterior (`labgeo2025.ino`), para que el controlador
   siempre aparezca en la misma dirección de la red.

Es el paso previo obligatorio para que `servidor_web_init()` (ver
`servidor_web.md`) tenga red disponible — por eso en `app_main.c` se llama
**antes**.

---

## 2. La única función exportada

### `esp_err_t red_eth_init(void);`

**Qué hace:** deja el W5500 arrancado, con IP estática fija, listo para que
cualquier otro componente de ESP-IDF (el servidor HTTP, por ejemplo) pueda
usar la red. Se llama **una sola vez**, al arrancar el firmware.

**Cómo se usa** (tal cual está en `app_main.c`):

```c
// 5) Ethernet + servidor web: si el W5500 no levanta (cable
// desconectado, config mala, etc.) no es fatal -- sensores y UART
// siguen funcionando igual, solo no hay WebSocket disponible.
if (red_eth_init() == ESP_OK) {
    servidor_web_init();
} else {
    ESP_LOGE(TAG, "Ethernet no disponible, el servidor web no se inicia");
}
```

**Valor de retorno:**
- `ESP_OK` → el W5500 respondió, quedó configurado con la IP fija y el
  link (chip) arrancado. Recién ahí tiene sentido llamar a
  `servidor_web_init()`.
- `ESP_FAIL` → no se detectó ningún módulo Ethernet (`eth_port_cnt == 0`) o
  falló `ethernet_init_all()` — por ejemplo, el chip no responde en el bus
  SPI (cable mal puesto, chip sin alimentación, pines mal configurados en
  Kconfig, etc.). El resto del firmware (lectura de sensores por UART) no
  se ve afectado: como dice el comentario en `app_main.c`, "no es fatal".

**Nota importante:** casi ningún otro dato "hardcodeado" (pines CS, RST,
SCLK, MISO, MOSI del W5500) está en este `.c`. Esa configuración vive en
`sdkconfig.defaults` como opciones de Kconfig del componente
`ethernet_init` (`CONFIG_ETHERNET_SPI_*`). Si algún día hay que cambiar de
pines físicos, hay que tocar ahí, no este archivo — `red_eth_init()` solo
llama a `ethernet_init_all()`, que ya arma el driver leyendo esa config.

---

## 3. Qué hace `red_eth_init()` por dentro, paso a paso

```c
esp_netif_init();                       // 1. arranca el stack de red de ESP-IDF
esp_event_loop_create_default();        // 2. arranca el bus de eventos del sistema

gpio_install_isr_service(0);            // 3. necesario porque el driver W5500
                                         //    engancha una interrupcion (INT) por GPIO

diag_pulso_reset_w5500();               // 4. pulso manual de RST, solo diagnostico

ethernet_init_all(&eth_handles, &cnt);  // 5. arma el driver del W5500 completo
                                         //    (SPI, CS, RST) segun sdkconfig.defaults

esp_netif_t *eth_netif = esp_netif_new(...);         // 6. crea la interfaz de red generica
esp_eth_new_netif_glue(eth_handles[0]);              //    y la "pega" al driver Ethernet
esp_netif_attach(eth_netif, glue);

esp_eth_ioctl(eth_handles[0], ETH_CMD_S_MAC_ADDR, s_mac);  // 7. fuerza la MAC fija

esp_netif_dhcpc_stop(eth_netif);        // 8. apaga DHCP (hace falta antes de IP fija)
esp_netif_set_ip_info(eth_netif, &ip_info);  // 9. asigna 192.168.18.91/24, gw .18.1

esp_event_handler_register(ETH_EVENT, ...);     // 10. logs de link up/down
esp_event_handler_register(IP_EVENT, ...);      //     (ver seccion 5, este ultimo casi nunca dispara)

esp_eth_start(eth_handles[0]);          // 11. RECIEN ACA arranca de verdad el chip

vTaskDelay(pdMS_TO_TICKS(500));         // 12. le da medio segundo al link
verificar_ip_activa(eth_netif, ...);    //     y vuelve a chequear que la IP siga siendo la fija
```

Algunos puntos que vale la pena entender bien:

- **Paso 3 (`gpio_install_isr_service`):** el driver del W5500 usa el pin
  de `INT` (interrupción) para enterarse de eventos del chip vía
  `gpio_isr_handler_add()` internamente. Esa llamada necesita que el
  "servicio de ISR de GPIO" ya esté instalado, si no, falla en silencio
  (no chequea el código de retorno) y la interrupción queda sin
  engancharse. Por eso acá se instala explícitamente antes de llamar a
  `ethernet_init_all()`. `ESP_ERR_INVALID_STATE` significa que otro módulo
  ya lo había instalado antes — no es un error real, por eso se filtra
  aparte del `ESP_ERROR_CHECK`.
- **Paso 4 (`diag_pulso_reset_w5500`):** *no es necesario* para que
  funcione — es un pulso manual del pin RST solo para poder ver en el
  monitor serie (o medir con multímetro/LED) que el pin responde, **antes**
  de que `ethernet_init_all()` tome control real de ese mismo pin según
  Kconfig. Al final del pulso, el pin se devuelve "en blanco"
  (`gpio_reset_pin`) para no interferir con la configuración real que
  viene después.
- **Paso 7 (MAC fija):** el chip no trae la MAC seteada sola — se le manda
  a mano la misma MAC que usaba el firmware Arduino viejo, para que si el
  router tiene una reserva de IP fija por MAC, siga funcionando igual.
- **Pasos 8-9 (IP estática):** hay que **apagar el cliente DHCP primero**
  (`esp_netif_dhcpc_stop`) antes de poder fijar la IP a mano — si el DHCP
  sigue activo, intentaría pisar la IP que uno configura.

---

## 4. ¿Es síncrona o asíncrona la inicialización?

**Es síncrona / bloqueante en su mayor parte, con una espera fija al
final.** `red_eth_init()` es una función común y corriente: se llama desde
`app_main()` y no vuelve hasta haber terminado toda la secuencia de arriba,
incluyendo un `vTaskDelay(pdMS_TO_TICKS(500))` (medio segundo) al final
para darle tiempo al link físico de levantar antes de loguear la IP de
nuevo. Como se llama desde `app_main()` (que corre en la tarea "main" de
FreeRTOS, antes de crear las demás tareas del sistema como
`tarea_sensores`), ese medio segundo de espera retrasa el arranque del
resto del firmware, pero solo una vez, al principio — no se repite durante
el funcionamiento normal.

Dentro de esa secuencia hay, sin embargo, una parte que sí es
**verdaderamente asíncrona**: el chequeo de que el **link físico** (el
cable) esté conectado y de que se haya asignado una IP no se resuelve con
una función que "espera hasta que pase" — se resuelve con **eventos**:

- `on_eth_event()` se dispara cuando cambia el estado del link
  (`ETHERNET_EVENT_CONNECTED` / `DISCONNECTED` / `START` / `STOP`). Esto
  puede pasar en cualquier momento, incluso mucho después de que
  `red_eth_init()` ya terminó y devolvió `ESP_OK` (por ejemplo, si en ese
  momento el cable todavía no estaba conectado del todo).
- `on_got_ip()` se dispara cuando el cliente **DHCP** consigue una IP —
  pero como este proyecto usa **IP fija con DHCP apagado**, este handler
  en la práctica **nunca se llama**. El comentario del código lo aclara
  explícitamente: no es un bug no verlo en el log, es el comportamiento
  esperado. La confirmación real de que la IP fija quedó aplicada la hace
  `verificar_ip_activa()`, llamada de forma síncrona y directa dos veces
  (justo después de `set_ip_info` y 500ms después de `esp_eth_start`).

**En resumen:**
- `red_eth_init()` en sí misma es una función síncrona que bloquea a quien
  la llama (unos ~500ms+ en el peor caso) y devuelve un resultado claro
  (`ESP_OK`/`ESP_FAIL`) sobre si el **chip** se pudo inicializar.
- Pero **no garantiza que el cable esté enchufado ni que haya
  conectividad real** en el momento en que retorna — eso se sigue
  informando de forma asíncrona, vía el bus de eventos de ESP-IDF
  (`on_eth_event`), durante toda la vida del programa, no solo al
  arrancar. Por eso el firmware sigue funcionando igual (sensores, UART)
  aunque el cable se desconecte después de haber arrancado: no hay ningún
  código que dependa de sondear el estado del link en un loop, todo pasa
  por esos callbacks de log.

---

## 5. Resumen rápido para usar este módulo en código nuevo

1. Llamar `red_eth_init()` una sola vez, al arrancar, **antes** de
   cualquier cosa que necesite red (por ejemplo `servidor_web_init()`).
2. Chequear el `esp_err_t` de retorno, pero tratarlo como **no fatal**: si
   falla, loguear y seguir — el resto del firmware (sensores, UART) no
   depende de la red.
3. No hace falta pollear ni esperar activamente a que el link esté
   arriba — si hace falta reaccionar a que se conecte/desconecte el cable
   en tiempo real, hay que registrar un handler propio sobre `ETH_EVENT`
   (como hace `on_eth_event`, que hoy solo loguea).
4. Si hay que cambiar la IP fija, la MAC, o algún pin del W5500: la IP/MAC
   están hardcodeadas en este `.c` (`s_mac`, `ip_info`); los **pines**
   físicos del chip están en `sdkconfig.defaults`, no acá.

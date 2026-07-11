# Clase 1: Instalación de ESP-IDF en Windows y primer programa en ESP32

> **Objetivo de la clase:** Al terminar, cada alumno tendrá el entorno de desarrollo oficial de Espressif (ESP-IDF) instalado y funcionando en Windows, habrá compilado su primer proyecto, entenderá cómo se graba un firmware en la placa y verá la salida por consola serial.

> **Duración estimada:** 2 a 3 horas (la instalación descarga varios GB, así que conviene tener buen internet o traerla adelantada).

---

## 1. ¿Qué es ESP-IDF y por qué lo usamos?

**ESP-IDF** (Espressif IoT Development Framework) es el framework oficial de Espressif para programar los chips ESP32 en C/C++. Es lo que se usa en la industria. Existe la alternativa de Arduino, que es más simple para empezar, pero ESP-IDF nos da control total del chip: FreeRTOS, todos los periféricos, WiFi, Bluetooth, y las librerías oficiales para pantallas, sensores, etc.

Piezas que vamos a instalar (todas vienen juntas, pero conviene saber qué son):

- **ESP-IDF**: el framework en sí (código fuente, librerías, ejemplos).
- **Toolchain**: los compiladores cruzados (xtensa para ESP32/S3, riscv para C3/C6). Compilan en tu PC código que corre en el chip.
- **Python + entorno virtual**: las herramientas de ESP-IDF (`idf.py`, `esptool.py`) están escritas en Python.
- **CMake y Ninja**: el sistema de construcción.
- **EIM (ESP-IDF Installation Manager)**: el instalador oficial que gestiona todo lo anterior.

Un detalle conceptual importante desde ya: **EIM es solo el instalador, no el framework**. Instalar EIM no significa tener ESP-IDF. Es como descargar el instalador de un juego: todavía falta instalar el juego.

---

## 2. Instalación de EIM con winget

Abrimos **PowerShell** y ejecutamos:

```powershell
winget install Espressif.EIM
```

Para verificar que quedó instalado:

```powershell
winget list Espressif
```

Debería aparecer algo como:

```
Name Id            Version Source
----------------------------------
eim  Espressif.eim 0.17.1  winget
```

### ⚠️ Problema típico #1: "eim no se reconoce como comando"

Es muy probable que al escribir `eim --version` salga este error:

```
eim : The term 'eim' is not recognized as the name of a cmdlet...
```

**¿Por qué pasa?** Winget instaló el programa pero no lo agregó al PATH (la lista de carpetas donde Windows busca comandos). El programa existe, solo que la terminal no sabe dónde está.

**Cómo encontrarlo:**

```powershell
Get-ChildItem -Path "C:\Program Files" -Recurse -Filter "eim*.exe" -ErrorAction SilentlyContinue | Select-Object FullName
```

En nuestro caso apareció en `C:\Program Files\eim\eim.exe`.

**Dos soluciones posibles:**

Opción A — Ejecutarlo con la ruta completa (nota el `&` adelante, es el operador de PowerShell para ejecutar rutas entre comillas):

```powershell
& "C:\Program Files\eim\eim.exe" --version
```

Opción B — Agregarlo al PATH del usuario de una vez:

```powershell
[Environment]::SetEnvironmentVariable("Path", [Environment]::GetEnvironmentVariable("Path", "User") + ";C:\Program Files\eim", "User")
```

Después de la opción B hay que **cerrar y abrir una PowerShell nueva** para que tome el cambio. Este detalle de "abrir terminal nueva después de cambiar el PATH" va a aparecer varias veces hoy: las terminales cargan el PATH al abrirse, no en vivo.

---

## 3. Instalación de ESP-IDF con EIM

Ahora sí instalamos el framework:

```powershell
eim install
```

El asistente pregunta la versión (elegimos la última estable, en nuestro caso quedó **v6.0.2**) y la ruta de instalación (dejamos la de por defecto). Empieza a descargar y esto tarda: son varios GB entre el framework, los submódulos de git (lwip, protobuf-c, unity...), el toolchain, Python y unos 85 componentes extra del registro de Espressif (entre ellos LVGL, la librería gráfica que usaremos más adelante con pantallas).

Al terminar deberíamos ver:

```
You have successfully installed ESP-IDF
the installer placed shortcuts for PowerShell terminal with activated ESP-IDF environment to your desktop
```

Rutas que quedan creadas (anótenlas, las vamos a necesitar):

| Qué | Dónde |
|---|---|
| Framework ESP-IDF | `C:\esp\v6.0.2\esp-idf` |
| Herramientas y toolchain | `C:\Espressif\tools` |
| Script de activación del entorno | `C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1` |

---

## 4. El concepto clave de la clase: el entorno activado

Aquí está la idea más importante del día, y la fuente del 80% de los problemas de los principiantes:

> **`idf.py` no es un comando global de Windows. Solo existe dentro de una terminal donde el entorno de ESP-IDF fue activado. Cada terminal nueva arranca "limpia" y hay que activarlo de nuevo.**

Si abren una PowerShell normal y escriben `idf.py --version`, van a ver:

```
idf.py : The term 'idf.py' is not recognized...
```

No se rompió nada. Simplemente esa terminal no tiene el entorno cargado.

**Formas de tener el entorno activo:**

1. **El acceso directo del escritorio** que creó el instalador ("IDF PowerShell"). Es el método recomendado: doble clic y listo.
2. **Activarlo a mano** en cualquier PowerShell:
   ```powershell
   & "C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"
   ```
3. **La terminal integrada de VS Code** con la extensión de ESP-IDF (lo vemos al final).

Cuando el entorno carga bien, se imprime un bloque así:

```
IDF PowerShell Environment
--------------------------
Environment variables set:
IDF_PATH: C:\esp\v6.0.2\esp-idf
IDF_TOOLS_PATH: C:\Espressif\tools
...
Custom commands available:
idf.py - Use this to run IDF commands (e.g., idf.py build)
esptool.py
...
```

### ⚠️ Problema típico #2: "running scripts is disabled on this system"

Al intentar ejecutar el script de activación, Windows puede bloquearlo:

```
cannot be loaded because running scripts is disabled on this system
```

**¿Por qué pasa?** PowerShell trae por defecto una política de ejecución que bloquea todos los scripts, por seguridad. Este mismo bloqueo es la razón por la que a veces el acceso directo del escritorio "no funciona" en silencio.

**Solución** (una sola vez, no requiere administrador):

```powershell
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
```

`RemoteSigned` permite ejecutar scripts locales (como los de ESP-IDF) pero sigue exigiendo firma a los descargados de internet. Es la configuración estándar de cualquier desarrollador en Windows; no estamos desactivando la seguridad.

### ⚠️ Problema típico #3: copiar el `$` de los tutoriales

Si en un tutorial ven algo como:

```
$ idf.py build
```

Ese `$` es el símbolo del prompt (indica "aquí escribe el usuario"), **no es parte del comando**. Si lo copian, PowerShell lanza errores confusos tipo "The ampersand (&) character is not allowed". Copien solo el comando.

**Verificación final de esta sección:** con el entorno activado, esto debe funcionar:

```powershell
idf.py --version
```

Salida esperada: `ESP-IDF v6.0.2` (o la versión instalada).

---

## 5. Primer proyecto

Con el entorno activado, creamos y entramos al proyecto:

```powershell
cd E:\ESP          # o la carpeta que prefieran para sus proyectos
idf.py create-project blink
cd blink
```

### Elegir el target (el chip correcto)

Cada chip de Espressif es un "target" distinto, con compilador distinto incluso (los ESP32 y S3 usan núcleos Xtensa; los C3 y C6 usan RISC-V). Hay que decirle al proyecto para cuál compilamos:

```powershell
idf.py set-target esp32s3    # para ESP32-S3
# idf.py set-target esp32    # para el ESP32 clásico (WROOM-32)
# idf.py set-target esp32c3  # para ESP32-C3
```

**¿No están seguros de qué chip tienen?** No confíen en la etiqueta ni en lo que dice el vendedor. Conecten la placa y pregúntenle directamente al chip:

```powershell
esptool.py chip_id
```

Esptool escanea los puertos, encuentra la placa y responde algo como "Detecting chip type... ESP32-S3". De paso les dice en qué puerto COM está. (Anécdota real de esta guía: creíamos tener un C3 y resultó ser un S3. Verificar toma 10 segundos y evita compilar todo dos veces.)

### El código

El proyecto se crea con un `main/blink.c` casi vacío:

```c
#include <stdio.h>

void app_main(void)
{

}
```

`app_main()` es el punto de entrada en ESP-IDF (el equivalente al `main()` de C de escritorio). Vacío, la placa no hace nada. Lo reemplazamos por un programa que imprime información del chip y un contador por la consola serial. Esta es la **versión final que compila**, después de resolver los tres errores que documentamos más abajo (¡léanlos, son la mitad de la clase!):

```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "sdkconfig.h"
#include "esp_psram.h"

static const char *TAG = "MI_APP";

void app_main(void)
{
    // ---- Información del chip ----
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);

    printf("\n");
    printf("=========================================\n");
    printf("   Hola desde mi ESP32-S3!\n");
    printf("   Nucleos:  %d\n", chip_info.cores);
    printf("   Revision: v%d.%d\n", chip_info.revision / 100, chip_info.revision % 100);
    printf("   Flash:    %lu MB\n", flash_size / (1024 * 1024));
    printf("   WiFi:     %s\n", (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "Si" : "No");
    printf("   BLE:      %s\n", (chip_info.features & CHIP_FEATURE_BLE) ? "Si" : "No");
    printf("=========================================\n\n");

    // ---- PSRAM (solo si esta habilitada en la configuracion) ----
#if CONFIG_SPIRAM
    if (esp_psram_is_initialized()) {
        size_t psram_size = esp_psram_get_size();
        ESP_LOGI(TAG, "PSRAM detectada: %u MB", psram_size / (1024 * 1024));
    } else {
        ESP_LOGW(TAG, "PSRAM habilitada en config pero no inicializada");
    }
#else
    ESP_LOGW(TAG, "PSRAM desactivada en la configuracion del proyecto");
#endif

    // ---- Bucle principal ----
    int contador = 0;

    while (1) {
        printf("printf clasico -> contador: %d\n", contador);
        ESP_LOGI(TAG, "Log ESP-IDF -> contador: %d", contador);

        contador++;
        vTaskDelay(1000 / portTICK_PERIOD_MS);  // 1 segundo
    }
}
```

Y su `main/CMakeLists.txt` correspondiente (el porqué de esta línea se explica en los errores de abajo):

```cmake
idf_component_register(SRCS "blink.c"
                       REQUIRES esp_psram spi_flash
                       INCLUDE_DIRS ".")
```

Tres cosas para explicar en pizarra:

- **`app_main()` no debe retornar nunca**: por eso el `while(1)`. Si la función termina, la tarea principal muere.
- **`vTaskDelay()` en lugar de esperas bloqueantes**: ESP-IDF corre sobre **FreeRTOS**, un sistema operativo de tiempo real. Este delay le cede el CPU a otras tareas (WiFi, sistema...). Un bucle de espera "a lo bruto" haría que el watchdog reinicie el chip.
- **Dos formas de imprimir**: `printf()` clásico y `ESP_LOGI()`, el sistema de logging de ESP-IDF (con colores, timestamp, etiqueta y niveles: Info, Warning, Error, Debug). En proyectos serios se usa el segundo.

### Compilar

```powershell
idf.py build
```

La **primera compilación tarda entre 3 y 10 minutos** porque compila el framework completo. Las siguientes son de segundos (solo recompila lo que cambió). Si termina bien, verán "Project build complete".

---

## 6. Los tres errores de compilación (la parte más valiosa de la clase)

Estos tres errores ocurrieron de verdad, en este orden, escribiendo el programa de arriba. Son tres caras del mismo tema —cómo se organiza ESP-IDF en componentes— más una lección de disciplina al leer errores. Recomendación docente: dejen que los alumnos los sufran unos minutos antes de dar la solución.

### ⚠️ Error #1: `undefined reference to esp_psram_...` (falta el componente)

```
undefined reference to `esp_psram_is_initialized'
collect2.exe: error: ld returned 1 exit status
```

**Quién se queja:** el **linker** (fíjense en el `ld.exe` y el `collect2`). El header `esp_psram.h` se encontró, el .c compiló, pero la librería con esas funciones no se enlazó.

**¿Por qué?** En ESP-IDF todo está organizado en **componentes** (esp_psram, driver, esp_wifi, nvs_flash...) y cada componente debe declarar explícitamente de qué otros depende, en su `CMakeLists.txt`.

**Cura:** agregar el componente al `REQUIRES` de `main/CMakeLists.txt`:

```cmake
idf_component_register(SRCS "blink.c"
                       REQUIRES esp_psram
                       INCLUDE_DIRS ".")
```

### ⚠️ Error #2: `fatal error: esp_flash.h: No such file or directory` (misma causa, otra cara)

```
fatal error: esp_flash.h: No such file or directory
```

**Quién se queja:** esta vez el **compilador**, y más temprano: ni siquiera encuentra el header. Misma causa raíz que el error #1 (dependencia de componente no declarada), solo que este componente falla antes, en compilación, porque sus headers no están en las rutas de include.

**¿De qué componente viene un header?** Truco para averiguarlo:

```powershell
Get-ChildItem -Path "C:\esp\v6.0.2\esp-idf\components" -Recurse -Filter "esp_flash.h" | Select-Object FullName
```

Devuelve `...\components\spi_flash\include\esp_flash.h` → el componente es `spi_flash`.

**Cura:** agregarlo al `REQUIRES` (los componentes se separan con espacio, no coma):

```cmake
REQUIRES esp_psram spi_flash
```

Chuleta de los headers usados en esta clase:

| Header | Componente | ¿Hay que declararlo? |
|---|---|---|
| `esp_flash.h` | `spi_flash` | Sí |
| `esp_psram.h` | `esp_psram` | Sí |
| `esp_chip_info.h` | `esp_hw_support` | No (viene por defecto) |
| `esp_log.h` | `log` | No |
| `freertos/*` | `freertos` | No |

**El patrón general, para siempre:** cada vez que usen una API nueva y salga `No such file or directory` o `undefined reference`, la cura casi siempre es agregar el componente correspondiente al `REQUIRES`. Cuando usemos la pantalla agregaremos `esp_lcd`; para WiFi, `esp_wifi` y `nvs_flash`; y así.

### ⚠️ Error #3: `undefined reference` OTRA VEZ, con el componente ya declarado

Después de arreglar los dos anteriores... volvió el mismo error de PSRAM. ¿Cómo, si `esp_psram` ya estaba en REQUIRES? Aquí hay **dos lecciones distintas** escondidas:

**Lección 3a — Componente enlazado ≠ funcionalidad habilitada.** Las funciones `esp_psram_*` solo se compilan dentro del componente si la PSRAM está **habilitada en la configuración del proyecto** (`CONFIG_SPIRAM`, que por defecto está apagada). El header existe, la función no. La solución profesional es condicionar el código a la configuración:

```c
#include "sdkconfig.h"   // para tener acceso a las macros CONFIG_*

#if CONFIG_SPIRAM
    // codigo que usa esp_psram_*
#else
    ESP_LOGW(TAG, "PSRAM desactivada en la configuracion del proyecto");
#endif
```

Así el código compila siempre, esté o no habilitada la PSRAM. (Habilitarla de verdad se hace con `idf.py menuconfig` → Component config → ESP PSRAM. Ojo: hay que saber si el módulo es Quad u Octal; elegir mal provoca un bucle de reinicios. Lo haremos en la clase de la pantalla.)

**Lección 3b — Lean la línea que señala el error.** Tras aplicar el `#if`, el error persistió... apuntando a la **línea 38**, cuando el bloque protegido estaba en la línea 15. ¿Qué había en la 38? **Un duplicado del bloque viejo sin protección**, que quedó olvidado más abajo al reordenar el código. El linker estaba diciendo exactamente dónde estaba el culpable; solo había que ir a mirar esa línea en vez de releer el bloque que "ya habíamos arreglado".

Truco de verificación: `Ctrl+F` en el editor → buscar el nombre de la función → contar cuántas veces aparece. Si son más de las esperadas, hay un duplicado.

> **Moraleja de la sección:** los errores de compilación traen la dirección del problema (archivo:línea). El instinto de principiante es leer solo la primera línea del error y adivinar; la respuesta suele estar escrita.

---

## 7. Anatomía del firmware: qué produce el build

Cuando el build termina bien, no genera "un archivo" sino **tres imágenes**, y cada una se graba en una dirección física distinta de la memoria flash:

| Imagen | Dirección en flash | Qué es |
|---|---|---|
| `bootloader.bin` | `0x0` | El cargador de arranque de segunda etapa |
| `partition-table.bin` | `0x8000` | El mapa de la flash: dónde vive cada cosa |
| `blink.bin` | `0x10000` | Tu aplicación |

La secuencia de arranque del chip es: la **ROM interna** (grabada de fábrica, imborrable) carga el bootloader desde `0x0` → el bootloader lee la tabla de particiones → localiza y salta a tu aplicación. Este trío es la anatomía estándar de todo firmware ESP-IDF.

Un matiz conceptual importante: **el ESP32 no tiene sistema de archivos donde "se copia" el programa**. El binario se graba byte a byte en direcciones físicas de la flash, sobreescribiendo lo que hubiera. "Subir el programa" es literalmente grabar memoria.

Dato del build de esta guía: nuestra app pesó ~165 KB (`0x283e0` bytes) con 84% de la partición libre. El propio log dice cuánto espacio queda — mírenlo siempre.

---

## 8. Flashear y ver la consola

### Encontrar el puerto COM

Al conectar la placa por USB, Windows le asigna un puerto COM. Formas de saber cuál:

- **Administrador de dispositivos** → "Puertos (COM y LPT)". Buscar "USB Serial Device", "CP210x" o "CH340". Truco infalible: desconectar la placa y ver cuál desaparece.
- **Desde la terminal:**
  ```powershell
  Get-WmiObject Win32_SerialPort | Select-Object DeviceID, Description
  ```
- **Que esptool lo encuentre solo** (además confirma el chip):
  ```powershell
  esptool.py chip_id
  ```

Si la placa **no aparece** al conectarla, los sospechosos en orden son: (1) el cable USB — muchísimos cables son solo de carga y no llevan datos, es el error #1 de todos los principiantes; (2) falta el driver CP210x o CH340. Las placas S3 con USB nativo no suelen necesitar driver.

Nota para placas S3: algunas exponen **dos** puertos COM (USB nativo + puente serial). Cualquiera sirve para flashear; la consola suele salir por el que dice "USB Serial Device" o "USB JTAG/serial debug unit".

### Qué hace realmente `idf.py flash monitor`

Son dos acciones encadenadas:

**Fase `flash`:**
1. Verifica si hay algo que recompilar (si tocaron código, recompila solo — no hace falta `build` aparte).
2. Encuentra el puerto (o usa el de `-p`).
3. Pone el chip en **modo descarga** vía las líneas del USB-serial.
4. Graba las tres imágenes, cada una en su dirección (`0x0`, `0x8000`, `0x10000`).
5. Resetea la placa, que arranca con el firmware nuevo.

**Fase `monitor`:** abre la conexión serial y muestra en vivo todo lo que la placa imprime (`printf`, `ESP_LOGI`, logs del sistema). No descarga nada: es una ventana en tiempo real. Al encadenarlos, ves el arranque desde el primer instante.

**Para salir del monitor: `Ctrl + ]`**

```powershell
idf.py flash monitor
```

Salida esperada: log del bootloader → la ficha del chip → el warning amarillo de PSRAM desactivada (correcto por ahora) → el contador subiendo cada segundo.

### La caja de herramientas de flasheo completa

```powershell
idf.py flash            # graba todo (compilando antes si hace falta)
idf.py monitor          # solo abre la consola, sin grabar nada
idf.py flash monitor    # el combo del primer flash
idf.py app-flash        # graba SOLO la app (rápido: salta bootloader y particiones)
idf.py app-flash monitor# el combo de iteración diaria
idf.py erase-flash      # borra TODA la flash (reset nuclear)
```

**¿Cuándo usar cuál?** La regla práctica:

> **Primer flash de un proyecto (o tras cambiar menuconfig / particiones / target) → `flash` completo. Iteraciones de código sobre lo mismo → `app-flash`, que es mucho más rápido.**

¿Por qué no `app-flash` desde el principio? Porque graba solo la app **asumiendo** que el bootloader y la tabla de particiones que ya están en la flash son compatibles con tu build. En una placa recién sacada de la caja, lo que hay grabado es el demo del fabricante, con quién sabe qué particiones — el `flash` completo garantiza coherencia. Incluso puede convenir un `erase-flash` previo la primera vez, para partir de cero sin restos del firmware anterior.

El ciclo de desarrollo diario queda así: editar código → `idf.py app-flash monitor` → observar → `Ctrl+]` → repetir.

### Si el flasheo falla ("Failed to connect")

Algunas placas necesitan entrar en modo descarga manualmente: mantener presionado el botón **BOOT**, dar un toque a **RESET** (o EN), soltar BOOT, y reintentar el flash.

Y si flashea bien pero el monitor no muestra nada: en las placas S3/C3 la consola puede salir por el USB nativo o por el UART0 según la configuración; se ajusta en `menuconfig` (Component config → ESP System Settings → Channel for console output).

---

## 9. Bonus: trabajar desde VS Code

Para no vivir en la terminal, instalamos VS Code y la extensión oficial:

1. Instalar VS Code: `winget install Microsoft.VisualStudioCode`
2. En VS Code → Extensiones (`Ctrl+Shift+X`) → buscar **"ESP-IDF"** → instalar la de **Espressif Systems**.
3. `Ctrl+Shift+P` → **"ESP-IDF: Configure ESP-IDF Extension"** → elegir **"Use Existing Setup"**.

El punto 3 es crítico: **"Use Existing Setup"** hace que la extensión use la instalación que ya tenemos. Si eligen "Express" o "Advanced", descarga ESP-IDF completo otra vez (varios GB duplicados). Si no detecta la instalación sola, se le indican las rutas:

- ESP-IDF path: `C:\esp\v6.0.2\esp-idf`
- Tools path: `C:\Espressif\tools`

Con eso, la barra inferior de VS Code tiene botones para todo: seleccionar target, seleccionar puerto, build 🛠️, flash ⚡, monitor 🖥️, y el "todo en uno" 🔥. Además la extensión activa el entorno automáticamente, con lo que se acaba el problema de "idf.py no se reconoce". También da autocompletado de todas las funciones de ESP-IDF.

(La extensión necesita la ExecutionPolicy que arreglamos en la sección 4; si alguien saltó ese paso, la extensión falla en silencio.)

---

## 10. Resumen de errores y sus curas (chuleta para el alumno)

| Síntoma | Causa | Cura |
|---|---|---|
| `eim` no se reconoce | Winget no lo agregó al PATH | Ejecutar con ruta completa o agregar `C:\Program Files\eim` al PATH y abrir terminal nueva |
| `idf.py` no se reconoce | Terminal sin entorno activado | Usar el acceso directo del escritorio o ejecutar el script `PowerShell_profile.ps1` |
| "running scripts is disabled" | Política de ejecución de PowerShell | `Set-ExecutionPolicy RemoteSigned -Scope CurrentUser` (una sola vez) |
| "The ampersand (&) is not allowed" | Se copió el `$` del prompt de un tutorial | Copiar el comando sin el `$` |
| `fatal error: X.h: No such file or directory` | Componente no declarado (falla el compilador) | Buscar de qué componente es el header y agregarlo a `REQUIRES` en `main/CMakeLists.txt` |
| `undefined reference to ...` | Componente no declarado (falla el linker) | Igual que el anterior: `REQUIRES <componente>` |
| `undefined reference` con el componente ya en REQUIRES | Funcionalidad deshabilitada en config (ej. `CONFIG_SPIRAM`) o código duplicado sin protección | Condicionar con `#if CONFIG_X`; ir a la línea exacta que señala el error y buscar duplicados con Ctrl+F |
| La placa no aparece como COM | Cable solo de carga, o falta driver | Cambiar cable; instalar driver CP210x/CH340 |
| "Failed to connect" al flashear | El chip no entró en modo descarga | Mantener BOOT, tocar RESET, soltar BOOT, reintentar |
| Flashea pero el monitor no muestra nada | Consola configurada en el puerto equivocado (USB vs UART) | `menuconfig` → ESP System Settings → Channel for console output |
| No sé qué chip tengo | Etiquetas poco confiables | `esptool.py chip_id` le pregunta al chip directamente |
| Compilé para el chip equivocado | Target mal setteado | `idf.py set-target <chip correcto>` (regenera el build, es normal que tarde) |
| La placa se comporta raro tras varios flasheos | Restos de firmwares anteriores | `idf.py erase-flash` y luego `flash` completo |

---

## 11. Conceptos clave para llevarse (repaso de 2 minutos)

1. **EIM instala; ESP-IDF es el framework.** Son dos cosas.
2. **El entorno se activa por terminal.** Terminal nueva = activar de nuevo (o usar el acceso directo / VS Code).
3. **ESP-IDF se organiza en componentes** y las dependencias se declaran en `REQUIRES`. Tres errores distintos de hoy, una misma causa.
4. **Componente enlazado ≠ funcionalidad habilitada.** Mucho código depende de `menuconfig` (`CONFIG_*`); protéjanlo con `#if`.
5. **Los errores traen la dirección del culpable** (archivo:línea). Léanla antes de adivinar.
6. **El firmware son tres imágenes en direcciones fijas de la flash.** No hay "copiar archivos": es grabar memoria.
7. **`flash` completo la primera vez; `app-flash` para iterar.** Y `erase-flash` cuando quieran partir de cero.
8. **`app_main()` nunca retorna y las esperas se hacen con `vTaskDelay()`.** FreeRTOS manda.

---

## 12. Tarea / siguiente clase

- Modificar el programa para que el contador cambie de velocidad (probar con 250 ms y 2000 ms) usando `app-flash monitor` para iterar rápido.
- Provocar a propósito el error de componente: incluir `driver/gpio.h`, ver qué pasa, y arreglarlo con `REQUIRES`.
- Investigar qué GPIO tiene el LED integrado de su placa y hacer el "blink" clásico con `gpio_set_level()`.
- **Siguiente clase:** GPIO y el LED parpadeante; habilitar la PSRAM en `menuconfig` (Quad vs Octal); y para las placas con pantalla, introducción a `esp_lcd` y **LVGL** para dibujar la primera interfaz gráfica.

---

*Material basado en una instalación real sobre Windows con ESP-IDF v6.0.2, EIM 0.17.1 y una placa ESP32-S3 con pantalla de 7". Todos los errores documentados ocurrieron de verdad durante el proceso — y esa es justamente la gracia: son los que van a aparecer en clase.*
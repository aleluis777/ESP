# Chuleta: reactivar ESP-IDF cuando la terminal no reconoce `idf.py`

> **El problema:** abriste una PowerShell nueva (o reiniciaste la PC) y sale:
> ```
> idf.py : The term 'idf.py' is not recognized...
> ```
> **No se rompió nada.** El entorno de ESP-IDF se activa por terminal: cada terminal nueva arranca "limpia" y hay que cargarlo de nuevo. Esto es normal y pasa siempre.

---

## Solución rápida (copiar y pegar)

En la PowerShell nueva, ejecutar:

```powershell
& "C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"
```

Si carga bien, se imprime el bloque "IDF PowerShell Environment" con las variables (IDF_PATH, etc.). Verificar con:

```powershell
idf.py --version
```

Debe responder: `ESP-IDF v6.0.2`. Listo, a trabajar.

---

## Después de activar: fijar el puerto de la placa

Para no escribir `-p COM5` en cada comando, fijarlo una vez por sesión:

```powershell
$env:ESPPORT = "COM5"
```

(Esto también se pierde al cerrar la terminal, igual que el entorno. Son dos líneas al abrir la terminal y ya.)

**Rutina completa al abrir una terminal nueva:**

```powershell
& "C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"
$env:ESPPORT = "COM5"
cd E:\ESP\blink\blink
```

---

## Comandos del día a día (ya con el entorno activo)

```powershell
idf.py build                    # compilar
idf.py flash monitor            # flash completo + consola (primera vez / tras menuconfig)
idf.py app-flash monitor        # solo la app + consola (iteracion rapida diaria)
idf.py monitor                  # solo consola, reseteando la placa
idf.py monitor --no-reset       # solo consola, SIN interrumpir lo que corre
idf.py erase-flash              # borrar toda la flash (reset nuclear)
idf.py menuconfig               # configuracion del proyecto
esptool.py chip_id              # que chip tengo y en que puerto esta
```

**Dentro del monitor:** `Ctrl+]` = salir | `Ctrl+T` luego `Ctrl+R` = resetear la placa

**Ojo con el orden de los argumentos:** el `-p` va antes del comando, las opciones del comando van después:

```powershell
idf.py -p COM5 monitor --no-reset
```

---

## Alternativas para no hacer esto a mano

### Opción 1: Acceso directo del escritorio

El instalador creó un acceso directo tipo "IDF PowerShell" en el escritorio. Abrir la terminal desde ahí = entorno ya cargado. Si no existe, crearlo: clic derecho en escritorio → Nuevo → Acceso directo → destino:

```
powershell.exe -NoExit -ExecutionPolicy Bypass -File "C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1"
```

### Opción 2: VS Code (la mejor para trabajar)

Con la extensión **ESP-IDF** de Espressif configurada (modo "Use Existing Setup"), el entorno se activa solo. La terminal integrada de la extensión (`Ctrl+Shift+P` → "ESP-IDF: Open ESP-IDF Terminal") ya viene con todo cargado, y el puerto se fija con un clic en la barra inferior y queda guardado.

---

## Si tampoco funciona el script de activación

**"running scripts is disabled on this system"** → la política de ejecución se restableció. Arreglar una vez:

```powershell
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
```

**"The ampersand (&) character is not allowed"** → se coló un `$` u otro carácter al copiar. El comando empieza directamente con `&`.

**El script no existe en esa ruta** → verificar la versión instalada (la ruta incluye `v6.0.2`; si se actualizó ESP-IDF, cambia). Buscarlo:

```powershell
Get-ChildItem -Path "C:\Espressif\tools" -Filter "*profile.ps1" | Select-Object FullName
```

---

## Datos de mi instalación (referencia)

| Qué | Valor |
|---|---|
| Versión ESP-IDF | v6.0.2 |
| Framework | `C:\esp\v6.0.2\esp-idf` |
| Herramientas | `C:\Espressif\tools` |
| Script de activación | `C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1` |
| Placa | ESP32-S3 con pantalla LCD 7" |
| Target | `esp32s3` |
| Puerto | COM5 |
| Proyectos | `E:\ESP\` |
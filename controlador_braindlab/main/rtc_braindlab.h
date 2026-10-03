#pragma once

// Reloj de tiempo real DS1307 (I2C, SDA=GPIO21 SCL=GPIO22, mismo bus que el
// resto de I2C de la placa -- ver HARDWARE.md §4.2). Confirmado por
// investigacion del firmware viejo (neuvov2.ino, E:\ESP\neuvov2): usa
// RTC_DS1307 de la libreria Adafruit RTClib, no un DS3231 ni un PCF8563.
// Este modulo usa el driver de esp-idf-lib (componente "ds1307", ver
// idf_component.yml) en vez de RTClib (esa es Arduino, aca es ESP-IDF puro).

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Inicializa el bus I2C (i2cdev_init() -- idempotente, lo puede llamar
// cualquier otro modulo que tambien use I2C, como sensores_temp.c con el
// ADS1115, sin coordinar el orden) y el DS1307. Si el chip esta parado (por
// ejemplo, se le agoto la pila de respaldo), lo arranca solo pero la hora va
// a quedar en el ultimo valor guardado hasta que alguien la ajuste (todavia
// no hay endpoint para eso).
esp_err_t rtc_braindlab_init(void);

// Lee la hora actual del DS1307 en 'tiempo'. Devuelve ESP_ERR_INVALID_STATE
// si rtc_braindlab_init() no se llamo o fallo.
esp_err_t rtc_braindlab_leer(struct tm *tiempo);

// Ajusta la hora del DS1307 a 'tiempo' -- tm_year/tm_mon en la convencion
// estandar de <time.h> (years since 1900, mes 0..11). tm_wday no hace falta
// que venga seteado por quien llama: esta funcion lo recalcula con mktime()
// antes de escribir, para no depender de que el que arma el struct tm se
// acuerde de calcular el dia de la semana a mano.
esp_err_t rtc_braindlab_ajustar(const struct tm *tiempo);

// true si el DS1307 responde por I2C pero su hora NO avanza (mismos
// segundos durante RTC_DETENIDO_MS) -- el oscilador de 32.768 kHz no corre.
// Es un problema de hardware (alimentacion a 3.3 V en un chip de 5 V, VBAT
// sin pila ni a GND, cristal), no de software: escribir la hora ya limpia
// el bit CH. Se actualiza en cada rtc_braindlab_leer(); ajustar la hora lo
// resetea (vuelve a esperar RTC_DETENIDO_MS antes de declararlo detenido).
bool rtc_braindlab_detenido(void);

#ifdef __cplusplus
}
#endif

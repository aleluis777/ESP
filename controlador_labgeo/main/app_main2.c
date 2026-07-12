// ---------------------------------------------------------------------------
// VERSION DE DIAGNOSTICO -- no es el firmware normal.
//
// Copia CASI LITERAL de la funcion dial() de LABGEO2.ino/labgeo2025.ino (la
// que ya funcionaba en produccion con Arduino), traducida linea por linea a
// ESP-IDF -- NO pasa por dial_caliper.c. Se mantienen los mismos "numeros
// magicos" del original: timeout de 3000000 iteraciones de 3us por cada
// flanco de CLK, y una espera de 2000 iteraciones vacias despues del flanco
// de bajada antes de leer DATA.
//
// Objetivo: si ESTO tampoco lee el calibre, el problema es de
// cableado/alimentacion del calibre (hardware), no de como esta escrito
// dial_caliper.c.
//
// Pines de esta prueba: REQ=GPIO13 (OUT-AT), CLK=GPIO25 (P2), DATA=GPIO27 (HUM).
//
// Para volver al firmware normal: "git checkout -- main/app_main.c" (esta
// version quedo commiteada antes de este cambio).
// ---------------------------------------------------------------------------

#include <stdint.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"

static const char *TAG = "DIAG_DIAL";

#define REQ  GPIO_NUM_13  // OUT-AT
#define CLK  GPIO_NUM_25  // P2
#define DATA GPIO_NUM_27  // HUM

// Traduccion casi literal de dial(REQ, CLK, DATA) del .ino original.
// Diferencias deliberadas frente al original:
//   - digitalWrite/digitalRead -> gpio_set_level/gpio_get_level.
//   - delayMicroseconds(3) -> esp_rom_delay_us(3).
//   - El busy-wait de "espera" (equivalente al segundo timeout=2000 del
//     original) se declara volatile para que el compilador de ESP-IDF no
//     lo elimine por optimizacion (Arduino con avr-gcc no lo hacia).
// Devuelve NULL si hubo timeout esperando algun flanco de CLK (igual que el
// original devolvia el string "ERROR!").
static const char *dial_original(void)
{
    uint32_t timeout = 0;
    int i = 0, j = 0, k = 0;
    static char mydata[14];
    static char distancia[20];

    ESP_LOGI(TAG, "dial_original(): REQ=1, esperando tren de 13 digitos...");
    gpio_set_level(REQ, 1);

    for (i = 0; i < 13; i++) {
        k = 0;
        for (j = 0; j < 4; j++) {
            timeout = 3000000;
            while (gpio_get_level(CLK) == 0) {
                esp_rom_delay_us(3);
                if ((timeout--) == 0) {
                    ESP_LOGW(TAG, "timeout esperando CLK=1 (digito %d, bit %d)", i, j);
                    gpio_set_level(REQ, 0);
                    return NULL;
                }
                // Cede CPU cada 1000 iteraciones (~3ms de trabajo) para que
                // la tarea IDLE del core 0 pueda correr y alimentar el
                // watchdog -- sin esto, un timeout grande sin flancos de CLK
                // bloquea el core y el task watchdog panickea a los ~5s.
                if ((timeout % 1000) == 0) {
                    vTaskDelay(1);
                }
            }
            timeout = 3000000;
            while (gpio_get_level(CLK) == 1) {
                esp_rom_delay_us(3);
                if ((timeout--) == 0) {
                    ESP_LOGW(TAG, "timeout esperando CLK=0 (digito %d, bit %d)", i, j);
                    gpio_set_level(REQ, 0);
                    return NULL;
                }
                if ((timeout % 1000) == 0) {
                    vTaskDelay(1);
                }
            }

            volatile uint32_t espera = 2000;
            while ((espera--) != 0) {
                ;
            }

            if (gpio_get_level(DATA) == 1 && j == 0) k = 1;
            else if (gpio_get_level(DATA) == 1 && j == 1) k = 2 + k;
            else if (gpio_get_level(DATA) == 1 && j == 2) k = 4 + k;
            else if (gpio_get_level(DATA) == 1 && j == 3) k = 8 + k;
            if (k == 15) k = 0;
        }

        mydata[i] = (char)(k + '0');
        ESP_LOGI(TAG, "digito %d = %c", i, mydata[i]);
    }

    snprintf(distancia, sizeof(distancia), "%c%c.%c%c%c",
             mydata[6], mydata[7], mydata[8], mydata[9], mydata[10]);

    ESP_LOGI(TAG, "dial_original(): REQ=0, tren completo -> \"%s\"", distancia);
    gpio_set_level(REQ, 0);
    return distancia;
}

void app_main(void)
{
    gpio_reset_pin(REQ);
    gpio_set_direction(REQ, GPIO_MODE_OUTPUT);
    gpio_set_level(REQ, 0);

    // PRUEBA: pull-down interno en CLK/DATA. Si las lecturas antes daban
    // digitos invalidos (>9, ej. ':' ';' '<') o el valor cambiaba solo sin
    // mover el calibre, es sintoma de linea flotando/ruidosa entre
    // transiciones -- esto es lo primero barato para descartarlo antes de
    // pensar en resistencias externas o level-shifting.
    gpio_reset_pin(CLK);
    gpio_set_direction(CLK, GPIO_MODE_INPUT);
    // gpio_set_pull_mode(CLK, GPIO_PULLDOWN_ONLY);

    gpio_reset_pin(DATA);
    gpio_set_direction(DATA, GPIO_MODE_INPUT);
    // gpio_set_pull_mode(DATA, GPIO_PULLDOWN_ONLY);

    ESP_LOGI(TAG, "Dial listo: REQ=GPIO%d CLK=GPIO%d DATA=GPIO%d", REQ, CLK, DATA);

    while (1) {
        const char *resultado = dial_original();
        if (resultado == NULL) {
            ESP_LOGW(TAG, "ERROR! (timeout esperando un flanco de CLK -- igual que el .ino original)");
        } else {
            ESP_LOGI(TAG, "distancia = %s", resultado);
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

#include "registro_sd.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/spi_master.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "REGISTRO_SD";

// Bus SPI: mismos pines/host que el W5500 (sdkconfig.defaults,
// CONFIG_ETHERNET_SPI_*), si hay que tocarlos es ahi. El /CS es propio.
#define SD_SPI_HOST    CONFIG_ETHERNET_SPI_HOST
#define SD_PIN_CS      GPIO_NUM_0 // ver registro_sd.h / HARDWARE.md §3
#define SD_PUNTO_MONT  "/sd"
#define SD_DIR_LOG     SD_PUNTO_MONT "/LOG"

#define COLA_LARGO     8 // 8 minutos de margen si la SD se traba

// La cola lleva muestras y, por la misma via, el pedido de formateo: asi
// todo lo que toca la SD lo hace un solo task, en orden.
typedef enum { ITEM_MUESTRA, ITEM_FORMATEAR } tipo_item_t;
typedef struct {
    tipo_item_t tipo;
    registro_sd_muestra_t muestra; // solo si tipo == ITEM_MUESTRA
} item_cola_t;

static QueueHandle_t s_cola = NULL;
static SemaphoreHandle_t s_fin_formateo = NULL;   // el task avisa que termino
static volatile bool s_formateando = false;
static volatile esp_err_t s_resultado_formateo = ESP_OK;
static sdmmc_card_t *s_tarjeta = NULL;
static volatile bool s_montada = false;
static volatile registro_sd_estado_t s_estado = REGISTRO_SD_SIN_TARJETA;

#define REINTENTO_MONTAJE_MS 60000 // sin tarjeta: probar de nuevo cada minuto

static const char *CSV_ENCABEZADO =
    "fecha_hora,t1,t2,t3,t4,temp_gestor,humedad_gestor,"
    "aa1,aa2,aa3,aa4,alarma_at,bypass_activo,ciclo,"
    "v_r,v_s,v_t,i_r,i_s,i_t\n";

// 'formatear_si_falla': true solo cuando lo pide registro_sd_formatear()
// con la tarjeta sin FAT (estado SIN_FORMATO). En uso normal es false: una
// tarjeta con otro formato NO se borra sola.
static esp_err_t montar(bool formatear_si_falla)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_SPI_HOST;
    slot.gpio_cs = SD_PIN_CS;

    esp_vfs_fat_sdmmc_mount_config_t cfg_mont = {
        .format_if_mount_failed = formatear_si_falla,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_PUNTO_MONT, &host, &slot, &cfg_mont, &s_tarjeta);
    if (err != ESP_OK) {
        s_tarjeta = NULL;
        // ESP_FAIL = la tarjeta respondio pero f_mount fallo (sin FAT32).
        // Cualquier otro error viene de la inicializacion de la tarjeta:
        // no esta, o no contesta.
        s_estado = (err == ESP_FAIL) ? REGISTRO_SD_SIN_FORMATO : REGISTRO_SD_SIN_TARJETA;
        return err;
    }
    s_montada = true;
    // Un ERROR_ESCRITURA previo (tarjeta llena, por ejemplo) se mantiene
    // hasta que una escritura salga bien -- montar sola no lo prueba.
    if (s_estado != REGISTRO_SD_ERROR_ESCRITURA) {
        s_estado = REGISTRO_SD_OK;
    }
    ESP_LOGI(TAG, "SD montada en " SD_PUNTO_MONT " (%s, %llu MB)", s_tarjeta->cid.name,
             ((uint64_t)s_tarjeta->csd.capacity * s_tarjeta->csd.sector_size) / (1024 * 1024));
    return ESP_OK;
}

static void desmontar(void)
{
    if (s_tarjeta) {
        esp_vfs_fat_sdcard_unmount(SD_PUNTO_MONT, s_tarjeta);
        s_tarjeta = NULL;
    }
    s_montada = false;
}

// mkdir que no falla si la carpeta ya existe.
static bool crear_dir(const char *ruta)
{
    if (mkdir(ruta, 0775) == 0 || errno == EEXIST) {
        return true;
    }
    ESP_LOGE(TAG, "No se pudo crear %s (errno %d)", ruta, errno);
    return false;
}

// Escribe una muestra en /sd/LOG/AAAA/MM/DD.CSV, creando las carpetas y el
// encabezado si es el primer dato del dia. Abre-escribe-cierra en cada
// muestra: si se corta la luz se pierde como mucho la fila en curso.
static esp_err_t escribir_muestra(const registro_sd_muestra_t *m)
{
    const struct tm *t = &m->fecha_hora;
    char ruta[48];

    snprintf(ruta, sizeof(ruta), SD_DIR_LOG);
    if (!crear_dir(ruta)) return ESP_FAIL;
    snprintf(ruta, sizeof(ruta), SD_DIR_LOG "/%04d", t->tm_year + 1900);
    if (!crear_dir(ruta)) return ESP_FAIL;
    snprintf(ruta, sizeof(ruta), SD_DIR_LOG "/%04d/%02d", t->tm_year + 1900, t->tm_mon + 1);
    if (!crear_dir(ruta)) return ESP_FAIL;
    snprintf(ruta, sizeof(ruta), SD_DIR_LOG "/%04d/%02d/%02d.CSV",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);

    struct stat st;
    bool nuevo = stat(ruta, &st) != 0;

    FILE *f = fopen(ruta, "a");
    if (!f) {
        ESP_LOGE(TAG, "No se pudo abrir %s (errno %d)", ruta, errno);
        return ESP_FAIL;
    }
    if (nuevo) {
        fputs(CSV_ENCABEZADO, f);
    }

    // Sensores que fallaron -> campo vacio (no un 0.0 que se grafique como
    // dato real).
    // Por canal: un NTC desconectado deja vacia solo su columna.
    char temps[64] = "";
    size_t largo = 0;
    for (int i = 0; i < 4; i++) {
        const char *sep = (i == 0) ? "" : ",";
        if (m->temp_ok[i]) {
            largo += snprintf(temps + largo, sizeof(temps) - largo, "%s%.1f", sep, m->temperaturas[i]);
        } else {
            largo += snprintf(temps + largo, sizeof(temps) - largo, "%s", sep);
        }
    }
    char gestor[24] = ",";
    if (m->gestor_ok) {
        snprintf(gestor, sizeof(gestor), "%.1f,%.1f", m->temp_gestor, m->humedad_gestor);
    }
    char energia[80] = ",,,,,";
    if (m->energia_ok) {
        snprintf(energia, sizeof(energia), "%.1f,%.1f,%.1f,%.2f,%.2f,%.2f",
                 m->voltajes[0], m->voltajes[1], m->voltajes[2],
                 m->corrientes[0], m->corrientes[1], m->corrientes[2]);
    }

    int n = fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d,%s,%s,%d,%d,%d,%d,%d,%d,%u,%s\n",
                    t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec,
                    temps, gestor,
                    m->salida_aire[0], m->salida_aire[1], m->salida_aire[2], m->salida_aire[3],
                    m->alarma_at, m->bypass_activo, m->ciclo, energia);
    bool ok = n > 0 && fclose(f) == 0;
    if (!ok) {
        ESP_LOGE(TAG, "Fallo escribiendo %s", ruta);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Lo corre tarea_registro_sd cuando llega ITEM_FORMATEAR.
static esp_err_t ejecutar_formateo(void)
{
    s_estado = REGISTRO_SD_FORMATEANDO;
    ESP_LOGW(TAG, "Formateando la SD -- se borra todo el historico");

    esp_err_t err;
    if (s_montada) {
        err = esp_vfs_fat_sdcard_format(SD_PUNTO_MONT, s_tarjeta);
        if (err != ESP_OK) {
            desmontar();
        }
    } else {
        // Sin montar: o no hay tarjeta, o no tiene FAT. Se intenta montar
        // formateando si hace falta; si la tarjeta no responde, montar()
        // devuelve error de inicializacion (no ESP_FAIL).
        err = montar(true);
        if (err != ESP_OK && err != ESP_FAIL) {
            s_estado = REGISTRO_SD_SIN_TARJETA;
            ESP_LOGW(TAG, "Formateo cancelado: no hay tarjeta (%s)", esp_err_to_name(err));
            return ESP_ERR_NOT_FOUND;
        }
    }

    if (err == ESP_OK) {
        s_estado = REGISTRO_SD_OK; // borra tambien un ERROR_ESCRITURA previo
        ESP_LOGI(TAG, "SD formateada OK");
    } else {
        // La tarjeta responde pero no se pudo formatear/escribir (dañada,
        // protegida): mismo aviso que una escritura fallida.
        s_estado = REGISTRO_SD_ERROR_ESCRITURA;
        ESP_LOGE(TAG, "Fallo el formateo (%s)", esp_err_to_name(err));
    }
    return err;
}

static void tarea_registro_sd(void *arg)
{
    item_cola_t item;
    registro_sd_muestra_t m;
    while (1) {
        // Timeout en vez de portMAX_DELAY: asi se reintenta montar cada
        // minuto aunque no lleguen muestras (por ejemplo, sin RTC) y el
        // estado que ven la web/pantalla no queda viejo.
        bool hay_item = xQueueReceive(s_cola, &item, pdMS_TO_TICKS(REINTENTO_MONTAJE_MS)) == pdTRUE;

        if (hay_item && item.tipo == ITEM_FORMATEAR) {
            s_resultado_formateo = ejecutar_formateo();
            xSemaphoreGive(s_fin_formateo);
            continue;
        }
        bool hay_muestra = hay_item;
        if (hay_muestra) {
            m = item.muestra;
        }

        // Sin tarjeta (no estaba al arrancar, o se saco): se reintenta
        // montar, como mucho una vez por minuto.
        if (!s_montada && montar(false) != ESP_OK) {
            if (hay_muestra) {
                ESP_LOGW(TAG, "Sin SD (estado %d) -- muestra de las %02d:%02d descartada",
                         (int)s_estado, m.fecha_hora.tm_hour, m.fecha_hora.tm_min);
            }
            continue;
        }
        if (!hay_muestra) {
            continue;
        }
        if (escribir_muestra(&m) == ESP_OK) {
            s_estado = REGISTRO_SD_OK;
        } else {
            // Lo mas probable es que se haya sacado la tarjeta: desmontar
            // para que la proxima vuelta intente montar de cero.
            ESP_LOGW(TAG, "Fallo la escritura -- se desmonta la SD y se reintenta en la proxima vuelta");
            s_estado = REGISTRO_SD_ERROR_ESCRITURA;
            desmontar();
        }
    }
}

esp_err_t registro_sd_init(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = CONFIG_ETHERNET_SPI_MOSI_GPIO,
        .miso_io_num = CONFIG_ETHERNET_SPI_MISO_GPIO,
        .sclk_io_num = CONFIG_ETHERNET_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000, // sectores de 512 B de la SD + margen
    };
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize fallo (%s)", esp_err_to_name(err));
        return err;
    }

    s_cola = xQueueCreate(COLA_LARGO, sizeof(item_cola_t));
    s_fin_formateo = xSemaphoreCreateBinary();
    if (!s_cola || !s_fin_formateo) {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(tarea_registro_sd, "tarea_registro_sd", 4096, NULL, 3, NULL);

    err = montar(false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo montar la SD (%s) -- se reintenta en cada muestra", esp_err_to_name(err));
    }
    return err;
}

void registro_sd_encolar(const registro_sd_muestra_t *muestra)
{
    if (!s_cola) {
        return;
    }
    item_cola_t item = { .tipo = ITEM_MUESTRA, .muestra = *muestra };
    if (xQueueSend(s_cola, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Cola llena (SD trabada?) -- muestra descartada");
    }
}

bool registro_sd_montada(void)
{
    return s_montada;
}

registro_sd_estado_t registro_sd_estado(void)
{
    return s_estado;
}

bool registro_sd_ruta_dia(int anio, int mes, int dia, char *ruta, size_t largo)
{
    if (!s_montada || anio < 2000 || anio > 2099 || mes < 1 || mes > 12 || dia < 1 || dia > 31) {
        return false;
    }
    snprintf(ruta, largo, SD_DIR_LOG "/%04d/%02d/%02d.CSV", anio, mes, dia);
    return true;
}

int registro_sd_dias_con_datos(int anio, int mes, uint8_t dias[31], uint32_t kb[31])
{
    if (!s_montada) {
        return -1;
    }
    if (anio < 2000 || anio > 2099 || mes < 1 || mes > 12) {
        return 0;
    }
    char ruta[48];
    snprintf(ruta, sizeof(ruta), SD_DIR_LOG "/%04d/%02d", anio, mes);
    DIR *dir = opendir(ruta);
    if (!dir) {
        return 0; // ese mes no tiene carpeta = no hay datos
    }

    // Por dia en vez de en el orden del directorio: asi la lista sale
    // ordenada sin tener que ordenarla despues.
    uint32_t tam_por_dia[32] = { 0 };
    bool hay[32] = { false };
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        // Solo "DD.CSV" -- ignora cualquier otra cosa que haya en la carpeta.
        int d = atoi(e->d_name);
        if (d < 1 || d > 31 || strlen(e->d_name) != 6 || strcasecmp(e->d_name + 2, ".CSV") != 0) {
            continue;
        }
        char ruta_archivo[64];
        snprintf(ruta_archivo, sizeof(ruta_archivo), "%s/%.6s", ruta, e->d_name); // ya se valido: 6 chars
        struct stat st;
        if (stat(ruta_archivo, &st) == 0 && st.st_size > 0) {
            hay[d] = true;
            tam_por_dia[d] = (uint32_t)st.st_size;
        }
    }
    closedir(dir);

    int n = 0;
    for (int d = 1; d <= 31; d++) {
        if (hay[d]) {
            dias[n] = (uint8_t)d;
            kb[n] = (tam_por_dia[d] + 1023) / 1024;
            n++;
        }
    }
    return n;
}

esp_err_t registro_sd_formatear(uint32_t timeout_ms)
{
    if (!s_cola) {
        return ESP_ERR_INVALID_STATE;
    }
    // Un solo formateo a la vez (dos pestañas del navegador, doble clic...).
    // Chequeo + marca sin lock: los pedidos llegan todos del task del
    // servidor HTTP, que atiende de a uno.
    if (s_formateando) {
        return ESP_ERR_INVALID_STATE;
    }
    s_formateando = true;
    xSemaphoreTake(s_fin_formateo, 0); // descarta un aviso viejo (formateo anterior que vencio por timeout)

    item_cola_t item = { .tipo = ITEM_FORMATEAR };
    // Al frente de la cola: no espera a que se escriban las muestras
    // pendientes (igual se van a borrar).
    if (xQueueSendToFront(s_cola, &item, pdMS_TO_TICKS(1000)) != pdTRUE) {
        s_formateando = false;
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(s_fin_formateo, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        err = s_resultado_formateo;
    }
    s_formateando = false;
    return err;
}

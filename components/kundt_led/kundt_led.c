/*
 * kundt_led.c - ver kundt_led.h.
 */

#include "kundt_led.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "kundt_led";

static int               s_gpio  = KUNDT_LED_DEFAULT_GPIO;
static kundt_led_state_t s_state = KUNDT_LED_BOOT;

/*
 * Patrón por estado, 16 ranuras de 100 ms: un ciclo completo dura 1,6 s.
 *
 * Antes la tarea alternaba a 1 Hz ignorando s_state, asi que el LED solo decia
 * "el firmware corre". Distinguir los estados a simple vista es lo unico que
 * queda cuando la placa esta montada y no hay puerto serie a mano.
 *
 * Bit 0 = primera ranura. Se leen de derecha a izquierda al escribirlos en
 * binario, por eso los literales van con el destello al final.
 */
#define LED_SLOT_MS 100
#define LED_SLOTS   16

static const uint16_t PATRONES[] = {
    [KUNDT_LED_BOOT]      = 0xAAAA, /* 1010... parpadeo rapido continuo */
    [KUNDT_LED_NO_WIFI]   = 0x0005, /* dos destellos y pausa larga */
    [KUNDT_LED_NO_SERVER] = 0x0015, /* tres destellos y pausa larga */
    [KUNDT_LED_RUNNING]   = 0x0001, /* un latido corto por ciclo */
    [KUNDT_LED_SELFTEST]  = 0x00FF, /* mitad encendido, mitad apagado */
    [KUNDT_LED_NO_DATA]   = 0x0F0F, /* largo encendido, largo apagado */
    [KUNDT_LED_FAULT]     = 0xFFFF, /* fijo */
    [KUNDT_LED_BUSY]      = 0x3333, /* 200 ms si, 200 ms no: se ve "trabajando" */
};

static void led_task(void *arg)
{
    (void)arg;

    unsigned slot = 0;
    for (;;) {
        kundt_led_state_t st = s_state;
        uint16_t patron = (st < (sizeof(PATRONES) / sizeof(PATRONES[0])))
                          ? PATRONES[st] : 0xAAAA;
        gpio_set_level(s_gpio, (patron >> slot) & 1u);
        slot = (slot + 1u) % LED_SLOTS;
        vTaskDelay(pdMS_TO_TICKS(LED_SLOT_MS));
    }
}

esp_err_t kundt_led_init(int gpio)
{
    s_gpio = gpio;

    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << gpio,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(led_task, "kundt_led", 2560, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LED de estado en GPIO%d", gpio);
    return ESP_OK;
}

void kundt_led_mark_selftest(void)
{
    ESP_LOGW(TAG, "COMPILACIÓN DE AUTOPRUEBA - los pines difieren del "
                  "esquemático, no instalar esto en un equipo");
}

void kundt_led_set_state(kundt_led_state_t state)
{
    if (state != s_state) {
        ESP_LOGI(TAG, "estado -> %s", kundt_led_state_name(state));
        s_state = state;
    }
}

kundt_led_state_t kundt_led_get_state(void)
{
    return s_state;
}

const char *kundt_led_state_name(kundt_led_state_t state)
{
    switch (state) {
    case KUNDT_LED_BOOT:       return "ARRANQUE";
    case KUNDT_LED_NO_WIFI:    return "SIN WIFI";
    case KUNDT_LED_NO_SERVER:  return "SIN SERVIDOR";
    case KUNDT_LED_RUNNING:    return "EN MARCHA";
    case KUNDT_LED_SELFTEST:   return "AUTOPRUEBA";
    case KUNDT_LED_NO_DATA:    return "SIN DATOS";
    case KUNDT_LED_FAULT:      return "FALLO";
    case KUNDT_LED_BUSY:       return "OCUPADO";
    default:                   return "desconocido";
    }
}

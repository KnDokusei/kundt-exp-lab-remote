/*
 * E2-SineGen - Generación de audio del tubo de Kundt.
 *
 * MQTT manda frecuencia y volumen; el módulo los aplica y confirma lo que quedó
 * puesto. Nada más: aquí no se mide, sólo se actúa.
 *
 *   AD9833 (SPI) --> potenciómetro (servo) --> LM358 seguidor --> TPA3118 --> parlante
 *
 * La frecuencia la sintetiza un DDS AD9833 y el volumen es el ángulo de un servo
 * que gira el potenciómetro de la cadena. "volumen" son GRADOS, de 0 a 180, no
 * un porcentaje: así lo nombró el firmware original y así lo espera el servidor.
 *
 * Una sola tarea, fsm_task, corre la máquina de estados:
 *
 *   INIT       -> SIN_WIFI    los periféricos están arriba y ya sale el tono
 *   SIN_WIFI   -> SIN_BROKER  hay enlace
 *   SIN_BROKER -> SIN_WIFI    se cayó el enlace
 *   SIN_BROKER -> IDLE        el broker responde
 *   IDLE       -> SIN_BROKER  se cayó MQTT; la consigna NO se toca
 *   IDLE       -> SIN_WIFI    se cayó el WiFi; la consigna NO se toca
 *   IDLE       -> FALLA       un driver devolvió error al actuar
 *   IDLE       -> ACTUALIZANDO  hay una imagen nueva esperando
 *
 * ACTUALIZANDO sólo se alcanza desde IDLE: es donde se sabe que no hay una
 * consigna a medio aplicar. Si la descarga falla se vuelve a IDLE con el
 * firmware intacto; si sale bien, la placa reinicia.
 *
 * Que SIN_WIFI y SIN_BROKER no toquen la consigna es el hallazgo A4: en la
 * versión Arduino una respuesta mala se convertía en silencio en 0 Hz y 0
 * grados, así que un tropiezo del backend apagaba el tono y mandaba el servo
 * contra un extremo. Aquí esos estados existen precisamente para no hacerlo.
 *
 * El ritmo lo marca la consigna, no un reloj: xQueueReceive espera hasta
 * STATUS_INTERVAL_MS, así que una consigna se atiende al instante y, si no
 * llega ninguna, el vencimiento dispara el latido de estado. E3 usa un tick
 * fijo porque vigila un motor y dos finales de carrera; aquí no hay nada que
 * vigilar entre consignas, y un tick fijo sólo añadiría latencia.
 */

#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ad9833.h"
#include "kundt_config.h"
#include "kundt_led.h"
#include "kundt_mqtt.h"
#include "kundt_ota.h"
#include "kundt_wifi.h"
#include "servo.h"

static const char *TAG = "E2-SineGen";

/* Latido de estado: cada cuánto se reporta lo que está puesto aunque nadie haya
 * cambiado nada. También es el vencimiento de la espera de consignas. */
#define STATUS_INTERVAL_MS 5000

/* Frecuencia inicial del AD9833; la misma que traía la versión Arduino. */
#define DEFAULT_FREQ_HZ 1200

/*
 * Límites de la banda de audio. El tubo es un experimento de física, no una
 * fuente de señal arbitraria: un valor fuera de este rango es un error del
 * backend, no una petición. La versión Arduino dejaba pasar cualquier cosa, y en
 * particular un parseo JSON fallido daba 0 Hz (hallazgo A4).
 */
#define FREQ_MIN_HZ 20
#define FREQ_MAX_HZ 20000

/* Cada cuántas vueltas se recuerda que el módulo está en falla. */
#define FALLA_CADA 12  /* 12 x 5 s = 1 min */

typedef enum {
    E2_INIT,
    E2_SIN_WIFI,
    E2_SIN_BROKER,
    E2_IDLE,
    E2_ACTUALIZANDO,
    E2_FALLA,
} estado_t;

static const char *const NOMBRE[] = {
    [E2_INIT]       = "INIT",
    [E2_SIN_WIFI]   = "SIN_WIFI",
    [E2_SIN_BROKER] = "SIN_BROKER",
    [E2_IDLE]       = "IDLE",
    [E2_ACTUALIZANDO] = "ACTUALIZANDO",
    [E2_FALLA]      = "FALLA",
};

static estado_t        s_estado = E2_INIT;
static ad9833_handle_t s_dds;
static int             s_freq_hz = DEFAULT_FREQ_HZ; /* lo que el DDS tiene puesto */
static QueueHandle_t   s_cola;

/* --------------------------------------------------------------------- */

static void cambiar(estado_t nuevo)
{
    ESP_LOGI(TAG, "%s -> %s", NOMBRE[s_estado], NOMBRE[nuevo]);
    s_estado = nuevo;
}

static int clamp_freq(int hz)
{
    if (hz < FREQ_MIN_HZ) {
        return FREQ_MIN_HZ;
    }
    if (hz > FREQ_MAX_HZ) {
        return FREQ_MAX_HZ;
    }
    return hz;
}

/*
 * Cola de una sola entrada, sobrescribible. El callback corre en la tarea de
 * eventos del cliente MQTT y no debe bloquear, así que sólo deja aquí la última
 * consigna; aplicarla es cosa de la FSM.
 *
 * Se sobrescribe a propósito: si llegan tres consignas mientras la tarea está
 * ocupada, la que importa es la última. Encolarlas todas haría que el generador
 * recorriera frecuencias que nadie pidió.
 */
static void on_actuators(const kundt_mqtt_actuators_t *act, void *ctx)
{
    (void)ctx;
    xQueueOverwrite(s_cola, act);
}

/* Publica lo que está puesto de verdad, no lo que se pidió: si el servo no
 * llegó al ángulo o la frecuencia se acotó, el servidor debe ver el valor real. */
static void publicar(void)
{
    const kundt_mqtt_actuators_t estado = {
        .frequency     = s_freq_hz,         .has_frequency = true,
        .volume        = servo_get_angle(), .has_volume    = true,
    };
    kundt_mqtt_publish(NULL, &estado);
}

/* Aplica una consigna. Devuelve false si un driver falló al actuar. */
static bool aplicar(const kundt_mqtt_actuators_t *a)
{
    bool ok = true;

    if (a->has_frequency) {
        const int want = clamp_freq((int)a->frequency);
        if (want != (int)a->frequency) {
            ESP_LOGW(TAG, "frecuencia %ld Hz fuera de rango, acotada a %d Hz",
                     (long)a->frequency, want);
        }
        /* Reprogramar sólo si cambió: el original reescribía el DDS en cada
         * ciclo aunque el valor fuera idéntico. */
        if (want != s_freq_hz) {
            uint32_t actual = 0;
            const esp_err_t err = ad9833_set_frequency(s_dds, (uint32_t)want, &actual);
            if (err == ESP_OK) {
                s_freq_hz = want;
                ESP_LOGI(TAG, "frecuencia -> %d Hz (el DDS sintetiza %lu Hz)",
                         want, (unsigned long)actual);
            } else {
                /*
                 * El AD9833 es de sólo escritura y la palabra de frecuencia sale
                 * en tres transferencias: un fallo a mitad deja FREQ0 con una
                 * mitad nueva y otra vieja, o sea el chip emitiendo algo que
                 * nadie pidió. Antes esto no se registraba y el módulo seguía
                 * publicando la frecuencia anterior como si nada.
                 */
                ESP_LOGE(TAG, "el DDS rechazó %d Hz (%s); la salida del chip es incierta",
                         want, esp_err_to_name(err));
                ok = false;
            }
        }
    }

    if (a->has_volume) {
        /* "volumen" es un ángulo de servo en grados, no un porcentaje (M1). */
        if ((int)a->volume != servo_get_angle()) {
            const esp_err_t err = servo_set_angle((int)a->volume);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "volumen -> %d grados", servo_get_angle());
            } else {
                ESP_LOGE(TAG, "el servo rechazó %ld grados (%s)",
                         (long)a->volume, esp_err_to_name(err));
                ok = false;
            }
        }
    }

    return ok;
}

/* --------------------------------------------------------------------- */

static void fsm_task(void *arg)
{
    (void)arg;
    kundt_mqtt_actuators_t cmd;
    uint32_t vueltas = 0;

    ESP_LOGI(TAG, "lazo de control iniciado (latido cada %d ms)", STATUS_INTERVAL_MS);

    for (;;) {
        /* El vencimiento ES el tick: una consigna se atiende al instante, y si
         * no llega ninguna dispara el latido. */
        const bool hay = xQueueReceive(s_cola, &cmd,
                                       pdMS_TO_TICKS(STATUS_INTERVAL_MS)) == pdTRUE;
        vueltas++;

        switch (s_estado) {
        case E2_INIT:
            /* app_main dejó el DDS emitiendo y el servo en su ángulo inicial. */
            cambiar(E2_SIN_WIFI);
            break;

        case E2_SIN_WIFI:
            kundt_led_set_state(KUNDT_LED_NO_WIFI);
            if (kundt_wifi_is_connected()) {
                cambiar(E2_SIN_BROKER);
            }
            break;

        case E2_SIN_BROKER:
            kundt_led_set_state(KUNDT_LED_NO_SERVER);
            if (!kundt_wifi_is_connected()) {
                cambiar(E2_SIN_WIFI);
            } else if (kundt_mqtt_is_connected()) {
                cambiar(E2_IDLE);
            }
            break;

        case E2_IDLE:
            if (!kundt_wifi_is_connected()) {
                cambiar(E2_SIN_WIFI);
                break;
            }
            if (!kundt_mqtt_is_connected()) {
                cambiar(E2_SIN_BROKER);
                break;
            }
            kundt_led_set_state(KUNDT_LED_RUNNING);
            kundt_ota_confirmar();  /* la imagen ha demostrado que conecta */

            if (kundt_ota_pendiente()) {
                cambiar(E2_ACTUALIZANDO);
                break;
            }
            if (hay && !aplicar(&cmd)) {
                cambiar(E2_FALLA);
                break;
            }
            publicar();

            if ((kundt_mqtt_published() % 60) == 0) {
                ESP_LOGI(TAG, "consignas=%lu publicados=%lu | %d Hz, %d grados | wifi=%s",
                         (unsigned long)kundt_mqtt_received(),
                         (unsigned long)kundt_mqtt_published(),
                         s_freq_hz, servo_get_angle(), kundt_wifi_ip());
            }
            break;

        case E2_ACTUALIZANDO:
            /* Bloquea mientras descarga. Si vuelve, es que falló y el firmware
             * actual sigue en pie. */
            kundt_ota_aplicar();
            cambiar(E2_IDLE);
            break;

        case E2_FALLA:
            /*
             * Se sigue publicando a propósito. Si el módulo callara, el servidor
             * no podría distinguir una avería de una placa desenchufada, y lo
             * que se publica aquí es justamente lo último que se sabe cierto.
             */
            if (kundt_mqtt_is_connected()) {
                publicar();
            }
            if ((vueltas % FALLA_CADA) == 0) {
                ESP_LOGE(TAG, "FALLA: un driver rechazó la última consigna; revisar SPI y servo");
            }
            break;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Tubo de Kundt - módulo E2 de generación de audio (ESP-IDF)");

    /* Se arranca primero para que el LED dé señales aunque falle la provisión. */
    ESP_ERROR_CHECK(kundt_led_init(KUNDT_LED_DEFAULT_GPIO));

    ESP_ERROR_CHECK(kundt_config_init());
    kundt_config_log();

    if (!kundt_config_is_provisioned()) {
        ESP_LOGE(TAG, "Falta el SSID de WiFi o la IP del servidor.");
        ESP_LOGE(TAG, "Configúralos con 'idf.py menuconfig', menú 'Kundt tube configuration',");
        ESP_LOGE(TAG, "y luego borra NVS una vez con 'idf.py erase-flash' para que carguen.");
        return;
    }

    kundt_config_t cfg;
    ESP_ERROR_CHECK(kundt_config_get(&cfg));

    /* Levantar el hardware antes que la red: el tubo debe estar emitiendo tono
     * aunque el backend sea inalcanzable. */
    const ad9833_config_t dds_cfg = AD9833_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(ad9833_init(&dds_cfg, &s_dds));
    ESP_ERROR_CHECK(ad9833_set_waveform(s_dds, AD9833_WAVE_SINE));

    uint32_t actual = 0;
    ESP_ERROR_CHECK(ad9833_set_frequency(s_dds, DEFAULT_FREQ_HZ, &actual));
    ESP_LOGI(TAG, "tono por defecto: %d Hz pedidos, %lu Hz sintetizados",
             DEFAULT_FREQ_HZ, (unsigned long)actual);

    const servo_config_t servo_cfg = SERVO_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(servo_init(&servo_cfg));

    /* La cola se crea antes de arrancar MQTT: si no, el primer mensaje entrante
     * la encontraría en NULL. */
    s_cola = xQueueCreate(1, sizeof(kundt_mqtt_actuators_t));
    configASSERT(s_cola);

    ESP_ERROR_CHECK(kundt_wifi_init());
    ESP_ERROR_CHECK(kundt_wifi_connect(cfg.wifi_ssid, cfg.wifi_password));

    /* El cliente MQTT se arranca sin esperar al enlace: reintenta la conexión
     * TCP por su cuenta y así el arranque no se queda bloqueado si la red tarda
     * o si el broker todavía no está levantado. */
    char broker[48];
    ESP_ERROR_CHECK(kundt_config_broker_uri(broker, sizeof(broker)));
    ESP_ERROR_CHECK(kundt_mqtt_start(broker, cfg.platform_id, cfg.controller_id,
                                     on_actuators, NULL));

    ESP_ERROR_CHECK(kundt_ota_start("e2", broker, cfg.platform_id));
    kundt_ota_log();

    kundt_led_set_state(KUNDT_LED_NO_WIFI);
    xTaskCreate(fsm_task, "e2_fsm", 4096, NULL, 5, NULL);
}

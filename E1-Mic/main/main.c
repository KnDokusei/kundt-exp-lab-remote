/*
 * E1-Mic - Micrófono del tubo de Kundt.
 *
 * El micrófono está fijo en la entrada del tubo. La MCU se reduce a producir la
 * codificación del audio para el servidor: un Goertzel a la frecuencia que E2
 * está emitiendo, y tres escalares por ventana. Aquí no sale audio.
 *
 *   micrófono -> preamp -> LM324 -> GPIO34 (ADC1_CH6) -> DMA -> Goertzel -> MQTT
 *
 * Por qué Goertzel y no el RMS a secas: el RMS de banda ancha mide el motor
 * paso a paso, los ventiladores, el rizado de la fuente y el tono, todo sumado.
 * Como E2 fija la frecuencia, se conoce, y mirar sólo esa componente rechaza el
 * resto: 54 dB medidos en banco. El cociente amplitud/rms dice qué fracción del
 * nivel es de verdad el tono.
 *
 * La frecuencia a la que mirar llega por el mismo tópico que las consignas de
 * E2: los tres módulos comparten controlador y cada uno se queda con los campos
 * que le tocan.
 *
 * Una sola tarea, fsm_task, corre la máquina de estados:
 *
 *   INIT       -> MIDIENDO     el ADC arrancó
 *   INIT       -> FALLA        mic_capture_start() falló
 *   MIDIENDO   -> REAFINANDO   llegó una frecuencia distinta de la afinada
 *   MIDIENDO   -> SIN_DATOS    varias tramas seguidas sin muestras
 *   REAFINANDO -> MIDIENDO     coeficiente nuevo; la ventana a medias se pierde
 *   SIN_DATOS  -> MIDIENDO     volvió a llegar audio
 *   MIDIENDO   -> ACTUALIZANDO hay una imagen nueva esperando
 *   cualquiera -> FALLA        el ADC devolvió un error que no es un vencimiento
 *
 * ACTUALIZANDO sólo se alcanza desde MIDIENDO y entre ventanas: una ventana a
 * medias se pierde igual que al reafinar, y no hay nada físico en marcha.
 *
 * No hay estados de WiFi ni de broker, a diferencia de E3. Allí la caída del
 * enlace DEBE parar el motor, porque un motor moviéndose a ciegas es peligroso.
 * Aquí no ocurre nada físico: la medida sigue con red o sin ella, y publicar sin
 * sesión ya lo descarta el propio cliente MQTT. El estado del enlace se ve en el
 * LED, que es donde sirve.
 *
 * El tick es la trama del DMA, no un reloj: mic_capture_read() es el único punto
 * bloqueante del lazo, y de ahí salen las ~10,8 ventanas por segundo.
 */

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "debug_stream.h"
#include "kundt_config.h"
#include "kundt_led.h"
#include "kundt_mqtt.h"
#include "kundt_ota.h"
#include "kundt_wifi.h"
#include "mic_capture.h"
#include "mic_dsp.h"

static const char *TAG = "E1-Mic";

/* Muestras por lectura del pool DMA. */
#define PCM_BLOCK_SAMPLES 512

/* Frecuencia de arranque del Goertzel. La misma con la que parte el AD9833 de
 * E2, para que la medida tenga sentido antes de recibir la primera consigna. */
#define DEFAULT_TONE_HZ 1200

/* Suficientemente largo para que un tubo en silencio no llene el log de
 * timeouts, y suficientemente corto para notar pronto un ADC detenido. */
#define ADC_READ_TIMEOUT_MS 200

/* Cadencia del log de avance. */
#define STATS_INTERVAL_MS 10000

/* Tramas vacías seguidas antes de dar el ADC por mudo. A 200 ms de vencimiento
 * son ~5 s: bastante para no gritar por una pausa, poco para notar un ADC
 * parado antes de que nadie se pregunte por qué no llegan medidas. */
#define VACIAS_PARA_MUDO 25

/* Cada cuántas vueltas se recuerda un estado que no avanza. */
#define RECORDAR_CADA 50

typedef enum {
    E1_INIT,
    E1_MIDIENDO,
    E1_REAFINANDO,
    E1_SIN_DATOS,
    E1_ACTUALIZANDO,
    E1_FALLA,
} estado_t;

static const char *const NOMBRE[] = {
    [E1_INIT]       = "INIT",
    [E1_MIDIENDO]   = "MIDIENDO",
    [E1_REAFINANDO] = "REAFINANDO",
    [E1_SIN_DATOS]  = "SIN_DATOS",
    [E1_ACTUALIZANDO] = "ACTUALIZANDO",
    [E1_FALLA]      = "FALLA",
};

static estado_t s_estado = E1_INIT;

/* Buffer de una lectura. Estático: 1 KiB no cabe cómodo en la pila de la tarea. */
static int16_t s_pcm[PCM_BLOCK_SAMPLES];

/* Estado de la medida. Lo toca sólo la tarea de la FSM. */
static mic_goertzel_t s_gz;
static int32_t        s_tono_hz = DEFAULT_TONE_HZ;  /* al que está afinado */
static uint64_t       s_win_sumsq;
static int32_t        s_win_peak;

/* Últimos valores publicados, sólo para el log periódico. */
static float    s_last_amp, s_last_rms;
static uint32_t s_ventanas;

static QueueHandle_t s_cola;

/* --------------------------------------------------------------------- */

static void cambiar(estado_t nuevo)
{
    ESP_LOGI(TAG, "%s -> %s", NOMBRE[s_estado], NOMBRE[nuevo]);
    s_estado = nuevo;
}

/*
 * Corre en la tarea de eventos del cliente MQTT: sólo anota, vale la última.
 * La cola convierte "llegó consigna" en un evento que la FSM consume, en vez de
 * una variable compartida que hay que acordarse de comparar cada vuelta.
 */
static void on_actuators(const kundt_mqtt_actuators_t *act, void *ctx)
{
    (void)ctx;
    if (act->has_frequency && act->frequency > 0) {
        xQueueOverwrite(s_cola, act);
    }
}

static void afinar(int32_t hz)
{
    s_tono_hz = hz;
    mic_goertzel_init(&s_gz, (float)hz, (float)MIC_SAMPLE_RATE_HZ,
                      CONFIG_E1_GOERTZEL_WINDOW);
    s_win_sumsq = 0;
    s_win_peak  = 0;
}

/* Acumula una trama en la ventana. Publica y reinicia si la ventana se cerró. */
static void acumular(size_t samples)
{
    mic_pcm_stats_t bs;
    mic_dsp_analyze(s_pcm, samples, &bs);
    s_win_sumsq += bs.sum_sq;

    /* El pico se toma en valor absoluto: la saturación aparece igual por arriba
     * que por abajo, y -INT16_MIN no cabe en int16. */
    const int32_t hi = bs.max;
    const int32_t lo = -(int32_t)bs.min;
    const int32_t pk = (hi > lo) ? hi : lo;
    if (pk > s_win_peak) {
        s_win_peak = pk;
    }

    mic_goertzel_push_block(&s_gz, s_pcm, samples);

    if (!mic_goertzel_ready(&s_gz)) {
        return;
    }

    const size_t n = s_gz.n;
    s_last_amp = mic_goertzel_rms(&s_gz);
    s_last_rms = sqrtf((float)s_win_sumsq / (float)n);
    s_ventanas++;

    const kundt_mqtt_sensors_t m = {
        .mic_amplitude = s_last_amp,  .has_mic_amplitude = true,
        .mic_rms       = s_last_rms,  .has_mic_rms       = true,
        .mic_peak      = s_win_peak,  .has_mic_peak      = true,
    };
    /* Un fallo no se reintenta: la ventana siguiente llega en ~93 ms y al
     * servidor le sirve el valor de ese momento, no el de antes. */
    kundt_mqtt_publish(&m, NULL);

    mic_goertzel_reset(&s_gz);
    s_win_sumsq = 0;
    s_win_peak  = 0;
}

static void proyectar_led(void)
{
    /* La imagen se confirma cuando ha demostrado que conecta, no con arrancar. */
    if (kundt_wifi_is_connected() && kundt_mqtt_is_connected()) {
        kundt_ota_confirmar();
    }
    kundt_led_set_state(!kundt_wifi_is_connected() ? KUNDT_LED_NO_WIFI
                        : kundt_mqtt_is_connected() ? KUNDT_LED_RUNNING
                                                    : KUNDT_LED_NO_SERVER);
}

static void telemetria(void)
{
    static int64_t last = 0;

    const int64_t now = esp_timer_get_time() / 1000;
    if (now - last < STATS_INTERVAL_MS) {
        return;
    }
    last = now;

    mic_capture_stats_t st;
    mic_capture_get_stats(&st);

    ESP_LOGI(TAG, "ADC  captadas=%llu dc=%ld min=%d max=%d rms=%lu descartes=%llu ovf=%lu",
             (unsigned long long)st.samples_captured, (long)st.dc_offset,
             (int)st.pcm_min, (int)st.pcm_max, (unsigned long)st.pcm_rms,
             (unsigned long long)st.words_dropped, (unsigned long)st.pool_overflows);
    ESP_LOGI(TAG, "MED  %s  %ld Hz  amplitud=%.1f rms=%.1f pico=%ld  ventanas=%lu",
             NOMBRE[s_estado], (long)s_tono_hz, (double)s_last_amp,
             (double)s_last_rms, (long)s_win_peak, (unsigned long)s_ventanas);
    ESP_LOGI(TAG, "RED  wifi=%s(%lu caídas) mqtt=%s publicados=%lu recibidos=%lu",
             kundt_wifi_ip(), (unsigned long)kundt_wifi_disconnect_count(),
             kundt_mqtt_is_connected() ? "arriba" : "abajo",
             (unsigned long)kundt_mqtt_published(),
             (unsigned long)kundt_mqtt_received());
    debug_stream_log();
    /* El heap es la cifra que delata una fuga: una caída lenta a lo largo de
     * horas es lo que deja el equipo del laboratorio fuera de servicio. */
    ESP_LOGI(TAG, "SIS  encendido=%llus heap=%u heap_min=%u",
             (unsigned long long)(esp_timer_get_time() / 1000000),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
}

/* --------------------------------------------------------------------- */

static void fsm_task(void *arg)
{
    (void)arg;
    kundt_mqtt_actuators_t cmd;
    uint32_t vueltas = 0, vacias = 0;

    ESP_LOGI(TAG, "tarea de medida iniciada en el núcleo %d", xPortGetCoreID());

    for (;;) {
        vueltas++;

        size_t    samples = 0;
        esp_err_t err     = ESP_ERR_TIMEOUT;

        /* El tick es la trama del DMA. Los estados que no consumen audio duermen
         * lo mismo, para no ocupar el núcleo dando vueltas. */
        if (s_estado == E1_MIDIENDO || s_estado == E1_SIN_DATOS) {
            err = mic_capture_read(s_pcm, PCM_BLOCK_SAMPLES, &samples,
                                   ADC_READ_TIMEOUT_MS);
        } else {
            vTaskDelay(pdMS_TO_TICKS(ADC_READ_TIMEOUT_MS));
        }

        /* Guarda global, al estilo de los dos finales de carrera de E3: un error
         * que no sea un vencimiento significa que la captura está rota. */
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT && s_estado != E1_FALLA) {
            ESP_LOGE(TAG, "mic_capture_read: %s", esp_err_to_name(err));
            cambiar(E1_FALLA);
        }

        switch (s_estado) {
        case E1_INIT: {
            const mic_capture_config_t mic_cfg = MIC_CAPTURE_DEFAULT_CONFIG();
            const esp_err_t e = mic_capture_start(&mic_cfg);
            if (e != ESP_OK) {
                ESP_LOGE(TAG, "mic_capture_start: %s", esp_err_to_name(e));
                cambiar(E1_FALLA);
                break;
            }
            afinar(DEFAULT_TONE_HZ);
            cambiar(E1_MIDIENDO);
            break;
        }

        case E1_MIDIENDO:
            if (kundt_ota_pendiente()) {
                cambiar(E1_ACTUALIZANDO);
                break;
            }
            if (xQueueReceive(s_cola, &cmd, 0) == pdTRUE && cmd.frequency != s_tono_hz) {
                cambiar(E1_REAFINANDO);
                afinar(cmd.frequency);
                break;
            }
            if (samples == 0) {
                if (++vacias >= VACIAS_PARA_MUDO) {
                    cambiar(E1_SIN_DATOS);
                }
                break;
            }
            vacias = 0;
            debug_stream_send(s_pcm, samples);
            acumular(samples);
            break;

        case E1_REAFINANDO:
            /*
             * Un solo paso. Existe como estado porque tiene una consecuencia que
             * conviene dejar escrita en el log: al cambiar el coeficiente se
             * pierde la ventana a medias, o sea una medida.
             */
            ESP_LOGI(TAG, "midiendo a %ld Hz", (long)s_tono_hz);
            cambiar(E1_MIDIENDO);
            break;

        case E1_SIN_DATOS:
            if (samples > 0) {
                vacias = 0;
                cambiar(E1_MIDIENDO);
                acumular(samples);
                break;
            }
            if ((vueltas % RECORDAR_CADA) == 0) {
                ESP_LOGW(TAG, "el ADC no entrega muestras desde hace %lu vueltas",
                         (unsigned long)vacias);
            }
            vacias++;
            break;

        case E1_ACTUALIZANDO:
            /* Bloquea mientras descarga. Si vuelve, es que falló y la medida
             * continúa con el firmware actual. */
            kundt_ota_aplicar();
            cambiar(E1_MIDIENDO);
            break;

        case E1_FALLA:
            /*
             * Se reintenta indefinidamente, como antes: una placa que hoy se
             * recupera sola debe seguir haciéndolo. Lo único que cambia es que
             * el aviso pasa a ser periódico en vez de diez por segundo.
             */
            if ((vueltas % RECORDAR_CADA) == 0) {
                ESP_LOGE(TAG, "FALLA: la captura de audio está caída; revisar el ADC");
            }
            if (mic_capture_read(s_pcm, PCM_BLOCK_SAMPLES, &samples,
                                 ADC_READ_TIMEOUT_MS) == ESP_OK) {
                cambiar(E1_MIDIENDO);
            }
            break;
        }

        proyectar_led();
        telemetria();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Tubo de Kundt - módulo E1 de micrófono (ESP-IDF)");

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

    ESP_ERROR_CHECK(kundt_wifi_init());
    ESP_ERROR_CHECK(kundt_wifi_connect(cfg.wifi_ssid, cfg.wifi_password));

    /* Esperar la primera asociación para no arrancar el cliente contra una
     * interfaz caída. Si vence el plazo se continúa igual: el componente de WiFi
     * sigue reintentando por su cuenta. */
    if (kundt_wifi_wait_connected(30000) != ESP_OK) {
        ESP_LOGW(TAG, "sin WiFi tras 30 s; se continúa (los reintentos siguen en segundo plano)");
    }

    /* La cola se crea antes de arrancar MQTT: si no, el primer mensaje entrante
     * la encontraría en NULL. */
    s_cola = xQueueCreate(1, sizeof(kundt_mqtt_actuators_t));
    configASSERT(s_cola);

    /* El cliente MQTT reintenta por su cuenta, así que se arranca sin esperar
     * más: si el broker aún no está levantado, se conectará cuando lo esté. */
    char broker[48];
    ESP_ERROR_CHECK(kundt_config_broker_uri(broker, sizeof(broker)));
    ESP_ERROR_CHECK(kundt_mqtt_start(broker, cfg.platform_id, cfg.controller_id,
                                     on_actuators, NULL));

    ESP_ERROR_CHECK(kundt_ota_start("e1", broker, cfg.platform_id));
    kundt_ota_log();

    debug_stream_start();

    /* El núcleo 1 mantiene la cadena de medida fuera del 0, donde corre WiFi. */
    xTaskCreatePinnedToCore(fsm_task, "e1_fsm", 4096, NULL, 5, NULL, 1);
}

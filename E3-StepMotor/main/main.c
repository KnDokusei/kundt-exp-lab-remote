/*
 * E3-StepMotor - Émbolo motorizado del tubo de Kundt.
 *
 * MQTT manda la posición objetivo en cm; el émbolo va directo ahí y se informa
 * la posición alcanzada y el error.
 *
 *   parlante            SW derecho                   SW izquierdo
 *     0 cm --------------- 26 cm ======== riel ======== 84 cm
 *                          STEPPER_DER <--------> STEPPER_IZQ
 *
 * Todo el cálculo es en pasos. La única conversión es la recta pasos = m · cm,
 * al recibir la consigna y al publicar. Los switches son la verdad física.
 *
 * Una sola tarea, fsm_task, corre la máquina de estados:
 *
 *   INIT        -> REFERENCIA   hay span guardado en NVS
 *   INIT        -> BARRIDO_IZQ  no hay span
 *   BARRIDO_IZQ -> BARRIDO_DER  tocó SW izq (posición := 0)
 *   BARRIDO_DER -> IDLE         tocó SW der: span medido y guardado
 *   REFERENCIA  -> IDLE         tocó SW der
 *   IDLE        -> MOVIENDO     llegó una consigna
 *   MOVIENDO    -> IDLE         llegó, o se cayó el WiFi
 *   MOVIENDO    -> BARRIDO_IZQ  tocó un switch con deriva > ERROR_MAX_CM
 *   búsquedas   -> FALLA        el motor paró sin tocar el switch buscado
 *   IDLE        -> ACTUALIZANDO hay una imagen nueva esperando
 *   cualquiera  -> FALLA        ambos switches pulsados a la vez
 *
 * ACTUALIZANDO sólo se alcanza desde IDLE, nunca desde MOVIENDO: reiniciar con
 * el motor en marcha dejaría el émbolo en una posición que nadie conoce, y al
 * arrancar la nueva imagen volvería a barrer el riel para encontrarse.
 */

#include <math.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

#include "kundt_config.h"
#include "kundt_led.h"
#include "kundt_mqtt.h"
#include "kundt_ota.h"
#include "kundt_wifi.h"
#include "stepper.h"
#include "stepper_math.h"

static const char *TAG = "E3-StepMotor";

#define TICK_MS         500   /* ritmo de la FSM y de la publicación */
#define ERROR_MAX_CM    1.0f  /* sobre esto: WARN, y en deriva además re-barrido */
#define BUSQUEDA_PASOS  29000 /* 2 rieles teóricos: sin switch en ese tramo, FALLA */
#define LOG_FALLA_TICKS 30    /* 15 s */

typedef enum {
    E3_INIT,
    E3_BARRIDO_IZQ,
    E3_BARRIDO_DER,
    E3_REFERENCIA,
    E3_IDLE,
    E3_MOVIENDO,
    E3_ACTUALIZANDO,
    E3_FALLA,
} estado_t;

static const char *const NOMBRE[] = {
    [E3_INIT]        = "INIT",
    [E3_BARRIDO_IZQ] = "BARRIDO_IZQ",
    [E3_BARRIDO_DER] = "BARRIDO_DER",
    [E3_REFERENCIA]  = "REFERENCIA",
    [E3_IDLE]        = "IDLE",
    [E3_MOVIENDO]    = "MOVIENDO",
    [E3_ACTUALIZANDO] = "ACTUALIZANDO",
    [E3_FALLA]       = "FALLA",
};

static estado_t       s_estado = E3_INIT;
static int32_t        s_span;   /* pasos entre SW izq y SW der; 0 = sin calibrar */
static int32_t        s_pedido; /* pasos que pidió la última consigna, sin acotar */
static QueueHandle_t  s_cola;
static kundt_config_t s_cfg;
static bool           s_mqtt_arrancado;

/* Pendiente m [pasos/cm] de la recta pasos = m · cm. Dejar UNA sin comentar. */
static float pendiente(void) { return (float)s_span / (STEPPER_SW_IZQ_CM - STEPPER_SW_DER_CM); } /* calibrada */
// static float pendiente(void) { return STEPPER_STEPS_PER_CM_TEORICO; }                           /* teórica: 250 */

static int32_t pasos_sw_der(void) { return stepper_cm_to_steps(STEPPER_SW_DER_CM, pendiente()); }
static int32_t pasos_sw_izq(void) { return stepper_cm_to_steps(STEPPER_SW_IZQ_CM, pendiente()); }

static int32_t span_teorico(void)
{
    return stepper_cm_to_steps(STEPPER_SW_IZQ_CM - STEPPER_SW_DER_CM, STEPPER_STEPS_PER_CM_TEORICO);
}

/* ---- NVS: el span sobrevive a los reinicios ------------------------------ */

static int32_t span_cargar(void)
{
    int32_t      span = 0;
    nvs_handle_t h;
    if (nvs_open("e3", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "span", &span);
        nvs_close(h);
    }
    /* Un valor absurdo (flash corrupta, otro riel) cuenta como ausente. */
    if (span < span_teorico() / 2 || span > span_teorico() * 3 / 2) {
        return 0;
    }
    return span;
}

static void span_guardar(int32_t span)
{
    nvs_handle_t h;
    if (nvs_open("e3", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "no se pudo abrir NVS: el span no queda guardado");
        return;
    }
    nvs_set_i32(h, "span", span);
    nvs_commit(h);
    nvs_close(h);
}

/* ---- MQTT ---------------------------------------------------------------- */

/* Corre en la tarea de eventos de esp_mqtt_client: sólo anota. Vale la última. */
static void on_actuators(const kundt_mqtt_actuators_t *act, void *ctx)
{
    (void)ctx;
    if (act->has_plunger_pos) {
        xQueueOverwrite(s_cola, act);
    }
}

/*
 * El LED es la única vía por la que E3 puede decir algo cuando no ha llegado a
 * IDLE: su cliente MQTT arranca ahí, así que en FALLA o calibrando el módulo es
 * invisible desde el servidor. Antes ni siquiera encendía un patrón distinto.
 *
 * El orden importa: primero los problemas de enlace, luego el estado propio.
 */
static void proyectar_led(void)
{
    kundt_led_state_t led;

    if (s_estado == E3_FALLA) {
        led = KUNDT_LED_FAULT;
    } else if (s_estado == E3_INIT || s_estado == E3_BARRIDO_IZQ
               || s_estado == E3_BARRIDO_DER || s_estado == E3_REFERENCIA) {
        /* Calibrando: el motor se mueve y todavía no se aceptan consignas. */
        led = KUNDT_LED_BUSY;
    } else if (!kundt_wifi_is_connected()) {
        led = KUNDT_LED_NO_WIFI;
    } else if (!kundt_mqtt_is_connected()) {
        led = KUNDT_LED_NO_SERVER;
    } else if (s_estado == E3_MOVIENDO) {
        led = KUNDT_LED_BUSY;
    } else {
        led = KUNDT_LED_RUNNING;
    }

    kundt_led_set_state(led);
}

static void publicar(void)
{
    bool izq, der;
    stepper_read_switches(&izq, &der);
    const kundt_mqtt_sensors_t m = {
        .plunger_actual  = stepper_steps_to_cm(stepper_position(), pendiente()),
        .has_plunger_actual  = true,
        .clockwise_limit = izq, .has_clockwise_limit = true,
        .counter_limit   = der, .has_counter_limit   = true,
    };
    kundt_mqtt_publish(&m, NULL); /* sin conexión no se encola: se ignora */
}

/* ---- Ayudantes de la FSM ------------------------------------------------- */

static void cambiar(estado_t nuevo)
{
    ESP_LOGI(TAG, "%s -> %s", NOMBRE[s_estado], NOMBRE[nuevo]);
    s_estado = nuevo;
}

/* Loguea un error; WARN si supera ERROR_MAX_CM. Devuelve true si lo supera. */
static bool reportar_error(const char *que, int32_t pasos, float cm)
{
    const bool excede = fabsf(cm) > ERROR_MAX_CM;
    if (excede) {
        ESP_LOGW(TAG, "%s: %ld pasos (%+.2f cm), supera %.1f cm",
                 que, (long)pasos, (double)cm, (double)ERROR_MAX_CM);
    } else {
        ESP_LOGI(TAG, "%s: %ld pasos (%+.2f cm)", que, (long)pasos, (double)cm);
    }
    return excede;
}

/* Busca un switch a velocidad lenta. BUSQUEDA_PASOS es el presupuesto, no el destino. */
static void buscar(int dir)
{
    stepper_set_speed(CONFIG_E3_CALIB_SPEED_SPS);
    stepper_move_to(stepper_position() + dir * BUSQUEDA_PASOS);
}

/* True si la búsqueda terminó sin tocar el switch buscado. */
static bool sin_switch(int dir)
{
    if (stepper_switch_hit() == dir) {
        return false;
    }
    ESP_LOGE(TAG, "%d pasos sin tocar el SW %s: revisa el switch y su cableado",
             BUSQUEDA_PASOS, (dir == STEPPER_IZQ) ? "izquierdo" : "derecho");
    return true;
}

static void mover(float cm)
{
    s_pedido = stepper_cm_to_steps(cm, pendiente());
    const int32_t destino = stepper_clamp_steps(s_pedido, pasos_sw_der(), pasos_sw_izq());
    ESP_LOGI(TAG, "consigna %.2f cm -> %ld pasos (desde %ld)",
             (double)cm, (long)destino, (long)stepper_position());
    stepper_move_to(destino);
}

/* Si el movimiento paró en un switch, ese switch manda: se mide la deriva y se
 * re-referencia. Devuelve true si la deriva exige volver a barrer. */
static bool re_referenciar(void)
{
    const int hit = stepper_switch_hit();
    if (hit == 0) {
        return false;
    }
    const int32_t esperado = (hit == STEPPER_IZQ) ? pasos_sw_izq() : pasos_sw_der();
    const int32_t deriva   = stepper_position() - esperado;
    stepper_set_position(esperado);
    return reportar_error("deriva al tocar el switch", deriva,
                          stepper_steps_to_cm(deriva, pendiente()));
}

/* IDLE es el primer estado que acepta consignas: MQTT arranca recién aquí para
 * que ninguna compita con la calibración por el motor. */
static void a_idle(void)
{
    stepper_set_speed(CONFIG_E3_SPEED_SPS);
    cambiar(E3_IDLE);

    if (!s_mqtt_arrancado) {
        char broker[48];
        ESP_ERROR_CHECK(kundt_config_broker_uri(broker, sizeof(broker)));
        ESP_ERROR_CHECK(kundt_mqtt_start(broker, s_cfg.platform_id, s_cfg.controller_id,
                                         on_actuators, NULL));
        ESP_ERROR_CHECK(kundt_ota_start("e3", broker, s_cfg.platform_id));
        kundt_ota_log();
        s_mqtt_arrancado = true;
    }

    /* Después de arrancar el cliente, no antes: publicar sin cliente devuelve
     * ESP_ERR_INVALID_STATE y se descarta. Aun así esta primera llamada falla,
     * porque el enlace TCP tarda unos segundos en establecerse; quien consigue
     * la primera medida es el publicado periódico de IDLE. Sirve para el otro
     * camino que entra aquí: al terminar un movimiento informa la posición
     * final sin esperar un tick. */
    publicar();
}

/* ---- Máquina de estados -------------------------------------------------- */

static void fsm_task(void *arg)
{
    (void)arg;
    kundt_mqtt_actuators_t cmd;
    uint32_t ticks = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        ticks++;

        proyectar_led();

        bool izq, der;
        stepper_read_switches(&izq, &der);
        if (izq && der && s_estado != E3_FALLA) {
            ESP_LOGE(TAG, "ambos switches pulsados: cableado roto o faltan los pull-ups a 3V3");
            stepper_stop();
            cambiar(E3_FALLA);
        }

        switch (s_estado) {
        case E3_INIT:
            s_span = span_cargar();
            if (s_span > 0) {
                ESP_LOGI(TAG, "span guardado: %ld pasos", (long)s_span);
                buscar(STEPPER_DER);
                cambiar(E3_REFERENCIA);
            } else {
                ESP_LOGI(TAG, "sin span guardado: barrido completo");
                buscar(STEPPER_IZQ);
                cambiar(E3_BARRIDO_IZQ);
            }
            break;

        case E3_BARRIDO_IZQ:
            if (stepper_is_moving()) {
                break;
            }
            if (sin_switch(STEPPER_IZQ)) {
                cambiar(E3_FALLA);
                break;
            }
            stepper_set_position(0);
            buscar(STEPPER_DER);
            cambiar(E3_BARRIDO_DER);
            break;

        case E3_BARRIDO_DER:
            if (stepper_is_moving()) {
                break;
            }
            if (sin_switch(STEPPER_DER)) {
                cambiar(E3_FALLA);
                break;
            }
            s_span = -stepper_position();
            span_guardar(s_span);
            ESP_LOGI(TAG, "span medido: %ld pasos, pendiente activa %.2f pasos/cm",
                     (long)s_span, (double)pendiente());
            reportar_error("largo medido - teórico", s_span - span_teorico(),
                           stepper_steps_to_cm(s_span - span_teorico(), STEPPER_STEPS_PER_CM_TEORICO));
            stepper_set_position(pasos_sw_der());
            a_idle();
            break;

        case E3_REFERENCIA:
            if (stepper_is_moving()) {
                break;
            }
            if (sin_switch(STEPPER_DER)) {
                cambiar(E3_FALLA);
                break;
            }
            stepper_set_position(pasos_sw_der());
            a_idle();
            break;

        case E3_IDLE:
            kundt_ota_confirmar();  /* la imagen ha demostrado que conecta */
            if (kundt_ota_pendiente()) {
                stepper_stop();
                cambiar(E3_ACTUALIZANDO);
                break;
            }
            /* Se publica en reposo, no sólo al moverse. Sin esto el servidor no
             * distingue un émbolo quieto y calibrado de una placa colgada: en
             * ambos casos ve silencio. Es además el único camino por el que sale
             * la primera medida, porque la de a_idle() ocurre antes de que MQTT
             * haya terminado de conectar. */
            publicar();
            if (xQueueReceive(s_cola, &cmd, 0) != pdTRUE) {
                break;
            }
            mover(cmd.plunger_pos);
            cambiar(E3_MOVIENDO);
            break;

        case E3_MOVIENDO:
            if (xQueueReceive(s_cola, &cmd, 0) == pdTRUE) {
                mover(cmd.plunger_pos);
            }
            if (!kundt_wifi_is_connected()) {
                ESP_LOGW(TAG, "sin WiFi: se detiene el motor");
                stepper_stop();
            }
            publicar();
            if (stepper_is_moving()) {
                break;
            }
            if (re_referenciar()) {
                buscar(STEPPER_IZQ);
                cambiar(E3_BARRIDO_IZQ);
                break;
            }
            reportar_error("error de posición (pedido - alcanzado)",
                           s_pedido - stepper_position(),
                           stepper_steps_to_cm(s_pedido - stepper_position(), pendiente()));
            a_idle();
            break;

        case E3_ACTUALIZANDO:
            /* Bloquea mientras descarga. Si vuelve, es que falló y el émbolo
             * sigue donde estaba, con el span de NVS intacto. */
            kundt_ota_aplicar();
            cambiar(E3_IDLE);
            break;

        case E3_FALLA:
            if (ticks % LOG_FALLA_TICKS == 0) {
                ESP_LOGE(TAG, "FALLA: motor detenido; revisar y reiniciar");
            }
            break;
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Tubo de Kundt - módulo E3, émbolo motorizado");

    ESP_ERROR_CHECK(kundt_led_init(KUNDT_LED_DEFAULT_GPIO));
    ESP_ERROR_CHECK(kundt_config_init()); /* también inicializa NVS */
    kundt_config_log();

    if (!kundt_config_is_provisioned()) {
        ESP_LOGE(TAG, "Falta el SSID de WiFi o la IP del servidor.");
        ESP_LOGE(TAG, "Configúralos con 'idf.py menuconfig', menú 'Kundt tube configuration',");
        ESP_LOGE(TAG, "y luego borra NVS una vez con 'idf.py erase-flash' para que carguen.");
        return;
    }
    ESP_ERROR_CHECK(kundt_config_get(&s_cfg));

    const stepper_config_t st = STEPPER_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(stepper_init(&st, CONFIG_E3_CALIB_SPEED_SPS));

    s_cola = xQueueCreate(1, sizeof(kundt_mqtt_actuators_t));
    configASSERT(s_cola);

    /* WiFi sube ya porque la OTA lo necesita; MQTT espera al primer IDLE. */
    ESP_ERROR_CHECK(kundt_wifi_init());
    ESP_ERROR_CHECK(kundt_wifi_connect(s_cfg.wifi_ssid, s_cfg.wifi_password));

    xTaskCreate(fsm_task, "e3_fsm", 4096, NULL, 5, NULL);
}

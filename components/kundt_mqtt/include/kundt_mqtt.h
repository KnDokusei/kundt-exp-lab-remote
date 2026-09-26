/*
 * kundt_mqtt.h - Cliente MQTT del tubo de Kundt contra curiousBeagle.
 *
 * Sustituye a kundt_api, que hablaba HTTP REST contra el servidor antiguo con
 * sondeo periódico. curiousBeagle usa MQTT y dos canales:
 *
 *   dev-status/kundt/<platform>/<controller>    dispositivo -> servidor
 *   ctrl-channel/kundt/<platform>/<fila>        servidor -> dispositivo
 *
 * El último segmento no es el mismo número en los dos sentidos: al subir va el
 * controller_id, y al bajar el servidor pone el id de su fila Kundt. Por eso la
 * suscripción es ctrl-channel/kundt/<platform>/+.
 *
 * Los tres módulos comparten un único controlador lógico y se distinguen por
 * los campos que publican, así que los tres usan el MISMO tópico. El manejador
 * del servidor fusiona en vez de reemplazar: escribir un campo ausente deja la
 * columna intacta. Por eso cada campo lleva su propio flag y sólo se serializa
 * lo que se marca; publicar el objeto entero borraría lo que otro módulo acaba
 * de reportar.
 *
 * ASIMETRÍA DEL PAYLOAD, verificada contra el servidor en ejecución y no
 * documentada en ningún repositorio de curiousBeagle:
 *
 *   lo que baja:  {"pattern":"ctrl-channel/kundt/1/1","data":{"id":1,"actuators":{...}}}
 *   lo que sube:  {"device":{"id":1,"platform_id":1},"sensors":{...}}
 *
 * NestJS envuelve lo que emite en {pattern, data} pero acepta objetos planos al
 * recibir. De ahí que al parsear haya que desenvolver y al publicar no.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* El tópico del tipo de experimento, tal como está en src/mqtt/channels.ts de
 * curiousBeagleAPI. Si allí cambia, aquí también. */
#define KUNDT_MQTT_TOPIC "kundt"

/**
 * Consignas que bajan del servidor.
 *
 * Los flags distinguen "ausente" de "presente valiendo cero", que es la misma
 * razón por la que kundt_api los llevaba: un campo que falta no debe llevar el
 * generador a 0 Hz ni el émbolo al origen.
 */
typedef struct {
    int32_t frequency;    bool has_frequency;    /* Hz, E2 */
    int32_t volume;       bool has_volume;       /* grados de servo, E2 */
    float   plunger_pos;  bool has_plunger_pos;  /* cm, E3 */
} kundt_mqtt_actuators_t;

/** Medidas que suben al servidor. */
typedef struct {
    float   mic_amplitude;    bool has_mic_amplitude;    /* Goertzel, E1 */
    float   mic_rms;          bool has_mic_rms;          /* banda ancha, E1 */
    int32_t mic_peak;         bool has_mic_peak;         /* saturación, E1 */
    float   plunger_actual;   bool has_plunger_actual;   /* cm, E3 */
    bool    clockwise_limit;  bool has_clockwise_limit;  /* E3 */
    bool    counter_limit;    bool has_counter_limit;    /* E3 */
} kundt_mqtt_sensors_t;

/**
 * @brief Se invoca al llegar una consigna del servidor.
 *
 * Corre en la tarea de eventos de esp_mqtt_client. **No debe bloquear**: mover
 * un motor o esperar un fin de carrera desde aquí ahoga la recepción de los
 * mensajes siguientes. Anotar la consigna y dejar que la tarea del módulo actúe.
 */
typedef void (*kundt_mqtt_actuators_cb_t)(const kundt_mqtt_actuators_t *act, void *ctx);

/**
 * @brief Arranca el cliente y se suscribe al canal de control.
 *
 * Retorna de inmediato. La conexión sube de forma asíncrona y esp_mqtt_client
 * reintenta solo; la suscripción se rehace en cada reconexión.
 *
 * @param broker_uri  p. ej. "mqtt://192.168.0.100:1883". Usar kundt_config_broker_uri().
 * @param platform_id Identidad en curiousBeagle, distinta de cero.
 * @param controller_id Ídem.
 * @param cb          Opcional. NULL en módulos que sólo publican.
 * @param ctx         Se pasa tal cual al callback.
 */
esp_err_t kundt_mqtt_start(const char *broker_uri,
                           uint8_t     platform_id,
                           uint8_t     controller_id,
                           kundt_mqtt_actuators_cb_t cb,
                           void       *ctx);

/** @brief Detiene el cliente y libera sus recursos. */
esp_err_t kundt_mqtt_stop(void);

/** @brief true si hay sesión MQTT establecida. */
bool kundt_mqtt_is_connected(void);

/**
 * @brief Publica el estado en el canal del dispositivo.
 *
 * Serializa sólo los campos marcados. Ambos punteros son opcionales: E1 pasa
 * NULL en actuadores, y un módulo que sólo confirma consignas pasa NULL en
 * sensores. Si no hay nada marcado no se publica y devuelve ESP_OK.
 *
 * @return ESP_ERR_INVALID_STATE si no hay conexión; el mensaje NO se encola.
 */
esp_err_t kundt_mqtt_publish(const kundt_mqtt_sensors_t   *sensors,
                             const kundt_mqtt_actuators_t *actuators);

/** @brief Mensajes publicados con éxito desde el arranque. */
uint32_t kundt_mqtt_published(void);

/** @brief Consignas recibidas y parseadas desde el arranque. */
uint32_t kundt_mqtt_received(void);

#ifdef __cplusplus
}
#endif

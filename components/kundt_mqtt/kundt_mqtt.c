/*
 * kundt_mqtt.c - ver kundt_mqtt.h.
 */

#include "kundt_mqtt.h"

#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mqtt_client.h"

static const char *TAG = "kundt_mqtt";

/* "ctrl-channel/kundt/255/255" son 26 caracteres; 64 deja margen de sobra. */
#define TOPIC_MAX 64
/* Los payloads reales rondan los 120 bytes. 384 admite todos los campos a la
 * vez con holgura y evita reservar memoria por mensaje. */
#define PAYLOAD_MAX 384

static esp_mqtt_client_handle_t s_client;
static char                     s_topic_pub[TOPIC_MAX];
static char                     s_topic_sub[TOPIC_MAX];
static uint8_t                  s_platform_id;
static uint8_t                  s_controller_id;
static kundt_mqtt_actuators_cb_t s_cb;
static void                    *s_ctx;
static volatile bool            s_connected;
static volatile uint32_t        s_published;
static volatile uint32_t        s_received;

/* --------------------------------------------------------------------- */

/* Extrae un número si está presente y es numérico. Devuelve false si falta,
 * que es lo que distingue "ausente" de "presente valiendo cero". */
static bool json_num(const cJSON *obj, const char *key, double *out)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(it)) {
        return false;
    }
    *out = it->valuedouble;
    return true;
}

static void parse_actuators(const char *data, int len)
{
    /* El payload del broker no termina en cero: cJSON_ParseWithLength evita
     * copiarlo a un buffer intermedio sólo para añadirlo. */
    cJSON *root = cJSON_ParseWithLength(data, (size_t)len);
    if (root == NULL) {
        ESP_LOGW(TAG, "consigna ilegible, se descarta");
        return;
    }

    /* NestJS envuelve lo que emite en {pattern, data}. Se acepta también el
     * objeto plano, para que un mosquitto_pub a mano sirva igual al depurar. */
    const cJSON *body = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(body)) {
        body = root;
    }

    const cJSON *act = cJSON_GetObjectItemCaseSensitive(body, "actuators");
    if (!cJSON_IsObject(act)) {
        ESP_LOGW(TAG, "consigna sin objeto 'actuators', se descarta");
        cJSON_Delete(root);
        return;
    }

    kundt_mqtt_actuators_t a = {0};
    double v;

    if (json_num(act, "frequency", &v))   { a.frequency   = (int32_t)v; a.has_frequency   = true; }
    if (json_num(act, "volume", &v))      { a.volume      = (int32_t)v; a.has_volume      = true; }
    if (json_num(act, "plunger_pos", &v)) { a.plunger_pos = (float)v;   a.has_plunger_pos = true; }

    cJSON_Delete(root);
    s_received++;

    if (s_cb != NULL) {
        s_cb(&a, s_ctx);
    }
}

static void mqtt_event_handler(void *args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    (void)args;
    (void)base;

    const esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        /* La suscripción se rehace en cada reconexión: el cliente no la
         * recuerda entre sesiones salvo que se use sesión persistente. */
        esp_mqtt_client_subscribe(s_client, s_topic_sub, 0);
        ESP_LOGI(TAG, "conectado; suscrito a %s", s_topic_sub);
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "desconectado del broker");
        break;

    case MQTT_EVENT_DATA:
        /* Un payload partido en varios eventos llegaría troceado. Los nuestros
         * caben de sobra en un fragmento, así que si esto salta es que el
         * servidor cambió el formato y conviene enterarse. */
        if (e->data_len != e->total_data_len) {
            ESP_LOGW(TAG, "payload fragmentado (%d de %d), se descarta",
                     e->data_len, e->total_data_len);
            break;
        }
        parse_actuators(e->data, e->data_len);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "error de transporte (tipo %d)", (int)e->error_handle->error_type);
        break;

    default:
        break;
    }
}

/* --------------------------------------------------------------------- */

esp_err_t kundt_mqtt_start(const char *broker_uri,
                           uint8_t     platform_id,
                           uint8_t     controller_id,
                           kundt_mqtt_actuators_cb_t cb,
                           void       *ctx)
{
    if (broker_uri == NULL || platform_id == 0 || controller_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_client != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_platform_id   = platform_id;
    s_controller_id = controller_id;
    s_cb            = cb;
    s_ctx           = ctx;

    snprintf(s_topic_pub, sizeof(s_topic_pub), "dev-status/%s/%u/%u",
             KUNDT_MQTT_TOPIC, (unsigned)platform_id, (unsigned)controller_id);
    /*
     * Comodín en el último segmento. El servidor resuelve lo que sube por
     * controller_id, pero publica las consignas con el id de su fila Kundt
     * (publishUpdate recibe entry.id). En el banco ambos valían 1; en
     * producción no, y las consignas caían en un tópico que ninguna placa
     * escuchaba. Cada plataforma tiene un único controlador lógico de Kundt,
     * así que escuchar toda la plataforma no recoge consignas ajenas.
     */
    snprintf(s_topic_sub, sizeof(s_topic_sub), "ctrl-channel/%s/%u/+",
             KUNDT_MQTT_TOPIC, (unsigned)platform_id);

    /*
     * client_id explícito, derivado del MAC.
     *
     * Sin él, esp-mqtt lo genera solo a partir del MAC y este cliente y el de
     * kundt_ota acaban llamándose igual: el broker aplica "session taken over"
     * y los dos se expulsan en bucle cada pocos segundos. Medido en placa.
     *
     * No vale derivarlo de platform_id/controller_id: E1, E2 y E3 los comparten
     * a propósito (un solo controlador lógico), así que las tres placas del kit
     * colisionarían entre sí.
     */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char client_id[32];
    snprintf(client_id, sizeof(client_id), "kundt-%02x%02x%02x",
             mac[3], mac[4], mac[5]);

    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri    = broker_uri,
        .credentials.client_id = client_id,
        /* Sin sesión persistente: al reconectar se resuscribe explícitamente y
         * no interesa recibir un backlog de consignas viejas. */
        .session.disable_clean_session = false,
        .session.keepalive             = 30,
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL);
    if (err == ESP_OK) {
        err = esp_mqtt_client_start(s_client);
    }
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }

    ESP_LOGI(TAG, "cliente \"%s\" arrancado contra %s", client_id, broker_uri);
    ESP_LOGI(TAG, "publica en %s", s_topic_pub);
    return ESP_OK;
}

esp_err_t kundt_mqtt_stop(void)
{
    if (s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client    = NULL;
    s_connected = false;
    return ESP_OK;
}

bool kundt_mqtt_is_connected(void)
{
    return s_connected;
}

uint32_t kundt_mqtt_published(void) { return s_published; }
uint32_t kundt_mqtt_received(void)  { return s_received; }

esp_err_t kundt_mqtt_publish(const kundt_mqtt_sensors_t   *sensors,
                             const kundt_mqtt_actuators_t *actuators)
{
    if (s_client == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_connected) {
        /* Encolar sin conexión sólo acumularía medidas obsoletas: cuando vuelva
         * el enlace interesa el valor de ese momento, no el de hace un minuto. */
        return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* El servidor identifica la fila por device.id, que es el controller_id.
     * Si no corresponde a ningún controlador, el manejador lanza un TypeError
     * y descarta el mensaje sin avisar al dispositivo. */
    cJSON *dev = cJSON_AddObjectToObject(root, "device");
    cJSON_AddNumberToObject(dev, "id", s_controller_id);
    cJSON_AddNumberToObject(dev, "platform_id", s_platform_id);

    bool any = false;

    if (sensors != NULL) {
        cJSON *s = cJSON_CreateObject();
        if (sensors->has_mic_amplitude)   { cJSON_AddNumberToObject(s, "mic_amplitude", sensors->mic_amplitude); any = true; }
        if (sensors->has_mic_rms)         { cJSON_AddNumberToObject(s, "mic_rms", sensors->mic_rms); any = true; }
        if (sensors->has_mic_peak)        { cJSON_AddNumberToObject(s, "mic_peak", sensors->mic_peak); any = true; }
        if (sensors->has_plunger_actual)  { cJSON_AddNumberToObject(s, "plunger_actual", sensors->plunger_actual); any = true; }
        if (sensors->has_clockwise_limit) { cJSON_AddBoolToObject(s, "clockwise_limit", sensors->clockwise_limit); any = true; }
        if (sensors->has_counter_limit)   { cJSON_AddBoolToObject(s, "counter_limit", sensors->counter_limit); any = true; }
        if (cJSON_GetArraySize(s) > 0) {
            cJSON_AddItemToObject(root, "sensors", s);
        } else {
            cJSON_Delete(s);
        }
    }

    if (actuators != NULL) {
        cJSON *a = cJSON_CreateObject();
        if (actuators->has_frequency)   { cJSON_AddNumberToObject(a, "frequency", actuators->frequency); any = true; }
        if (actuators->has_volume)      { cJSON_AddNumberToObject(a, "volume", actuators->volume); any = true; }
        if (actuators->has_plunger_pos) { cJSON_AddNumberToObject(a, "plunger_pos", actuators->plunger_pos); any = true; }
        if (cJSON_GetArraySize(a) > 0) {
            cJSON_AddItemToObject(root, "actuators", a);
        } else {
            cJSON_Delete(a);
        }
    }

    if (!any) {
        cJSON_Delete(root);
        return ESP_OK;
    }

    char payload[PAYLOAD_MAX];
    const bool ok = cJSON_PrintPreallocated(root, payload, sizeof(payload), false);
    cJSON_Delete(root);

    if (!ok) {
        ESP_LOGE(TAG, "el payload no cabe en %d bytes", PAYLOAD_MAX);
        return ESP_ERR_INVALID_SIZE;
    }

    /* QoS 0: la medida siguiente llega en decenas de milisegundos, así que
     * reintentar una perdida cuesta más de lo que vale. */
    const int id = esp_mqtt_client_publish(s_client, s_topic_pub, payload, 0, 0, 0);
    if (id < 0) {
        return ESP_FAIL;
    }

    s_published++;
    return ESP_OK;
}

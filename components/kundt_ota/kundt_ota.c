/*
 * kundt_ota.c - ver kundt_ota.h.
 *
 * sdkconfig.h va primero y explícito. Lo demás llega por includes transitivos, y
 * un reordenamiento podría dejar guardas de compilación dando falso en silencio.
 */
#include "sdkconfig.h"

#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "kundt_ota.h"

static const char *TAG = "kundt_ota";

/* Dos tópicos: el propio y el colectivo del módulo. */
#define TOPICOS 2
#define TOPICO_MAX 48

static esp_mqtt_client_handle_t s_cliente;
static char                     s_topico[TOPICOS][TOPICO_MAX];
static char                     s_url[KUNDT_OTA_URL_MAX];
static volatile bool            s_pendiente;
static bool                     s_confirmada;

/* --------------------------------------------------------------------- */

/*
 * Anota la orden y vuelve. No descarga aquí: esto corre en la tarea de eventos
 * del cliente MQTT, que no debe bloquear, y sobre todo porque quien decide si
 * este es un momento seguro para actualizar es la máquina de estados del módulo.
 */
static void anotar_orden(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, (size_t)len);
    if (root == NULL) {
        ESP_LOGW(TAG, "orden ilegible, se descarta");
        return;
    }

    const cJSON *url = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (!cJSON_IsString(url) || url->valuestring == NULL) {
        ESP_LOGW(TAG, "orden sin campo 'url', se descarta");
        cJSON_Delete(root);
        return;
    }
    if (strlen(url->valuestring) >= KUNDT_OTA_URL_MAX) {
        ESP_LOGW(TAG, "URL demasiado larga (%d), se descarta", (int)strlen(url->valuestring));
        cJSON_Delete(root);
        return;
    }

    strncpy(s_url, url->valuestring, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    cJSON_Delete(root);

    s_pendiente = true;
    ESP_LOGW(TAG, "orden de actualización recibida: %s", s_url);
    ESP_LOGW(TAG, "se aplicará cuando el módulo esté en un estado seguro");
}

static void ota_event_handler(void *args, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    (void)args;
    (void)base;

    const esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        for (int i = 0; i < TOPICOS; i++) {
            esp_mqtt_client_subscribe(s_cliente, s_topico[i], 0);
        }
        ESP_LOGI(TAG, "suscrito a %s y %s", s_topico[0], s_topico[1]);
        break;

    case MQTT_EVENT_DATA:
        if (e->data_len != e->total_data_len) {
            ESP_LOGW(TAG, "orden fragmentada (%d de %d), se descarta",
                     e->data_len, e->total_data_len);
            break;
        }
        anotar_orden(e->data, e->data_len);
        break;

    default:
        break;
    }
}

/* --------------------------------------------------------------------- */

esp_err_t kundt_ota_start(const char *modulo, const char *broker_uri,
                          uint8_t platform_id)
{
    if (modulo == NULL || broker_uri == NULL || platform_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cliente != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(s_topico[0], TOPICO_MAX, "kundt-ota/%s/%u", modulo, (unsigned)platform_id);
    snprintf(s_topico[1], TOPICO_MAX, "kundt-ota/%s/all", modulo);

    /*
     * Cliente propio, separado del de consignas. Cuesta unos pocos KiB y a
     * cambio una avería del camino de actualización no puede dejar al módulo sin
     * recibir consignas, que es su función real.
     */
    const esp_mqtt_client_config_t cfg = {
        .broker.address.uri = broker_uri,
        .session.keepalive  = 60,
    };

    s_cliente = esp_mqtt_client_init(&cfg);
    if (s_cliente == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(s_cliente, ESP_EVENT_ANY_ID,
                                                   ota_event_handler, NULL);
    if (err == ESP_OK) {
        err = esp_mqtt_client_start(s_cliente);
    }
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_cliente);
        s_cliente = NULL;
        return err;
    }
    return ESP_OK;
}

bool kundt_ota_pendiente(void)
{
    return s_pendiente;
}

esp_err_t kundt_ota_aplicar(void)
{
    if (!s_pendiente) {
        return ESP_ERR_INVALID_STATE;
    }
    s_pendiente = false;  /* Una orden se intenta una vez: si falla, se repite a mano. */

    ESP_LOGW(TAG, "descargando %s", s_url);

    const esp_http_client_config_t http = {
        .url               = s_url,
        .timeout_ms        = 20000,
        .keep_alive_enable = true,
    };
    const esp_https_ota_config_t ota = {
        .http_config = &http,
    };

    const esp_err_t err = esp_https_ota(&ota);
    if (err != ESP_OK) {
        /* La partición activa no se ha tocado: el módulo sigue con su firmware. */
        ESP_LOGE(TAG, "la actualización falló (%s); se sigue con la imagen actual",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "imagen grabada; reiniciando para arrancarla a prueba");
    vTaskDelay(pdMS_TO_TICKS(500));  /* que salga el log antes del reinicio */
    esp_restart();
    return ESP_OK;  /* inalcanzable */
}

void kundt_ota_confirmar(void)
{
    if (s_confirmada) {
        return;
    }
    esp_ota_img_states_t estado;
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (p == NULL || esp_ota_get_state_partition(p, &estado) != ESP_OK) {
        return;
    }
    if (estado != ESP_OTA_IMG_PENDING_VERIFY) {
        s_confirmada = true;  /* no está a prueba: no hay nada que confirmar */
        return;
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_confirmada = true;
        ESP_LOGW(TAG, "imagen confirmada; se cancela la vuelta atrás");
    }
}

bool kundt_ota_a_prueba(void)
{
    esp_ota_img_states_t estado;
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (p == NULL || esp_ota_get_state_partition(p, &estado) != ESP_OK) {
        return false;
    }
    return estado == ESP_OTA_IMG_PENDING_VERIFY;
}

void kundt_ota_log(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    const esp_app_desc_t  *d = esp_app_get_description();

    ESP_LOGI(TAG, "partición \"%s\" | versión %s | %s",
             p != NULL ? p->label : "?",
             d != NULL ? d->version : "?",
             kundt_ota_a_prueba() ? "A PRUEBA, sin confirmar" : "confirmada");
}

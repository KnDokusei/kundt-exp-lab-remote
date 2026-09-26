/*
 * kundt_wifi.c - ver kundt_wifi.h.
 */

#include "kundt_wifi.h"

#include "kundt_config.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"

static const char *TAG = "kundt_wifi";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_events;
static esp_netif_t       *s_netif;
static bool               s_initialised;
static uint32_t           s_retry_delay_ms = KUNDT_WIFI_RETRY_MIN_MS;
static uint32_t           s_disconnects;
static char               s_ip[16] = "0.0.0.0";
static esp_timer_handle_t s_retry_timer;

static void aplicar_ip_fija(void);

static void retry_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "reconectando...");
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect falló: %s", esp_err_to_name(err));
    }
}

/* Espera exponencial con tope. Sin el tope, una caída larga del AP alargaría el
 * intervalo indefinidamente; sin la espera creciente, el log se inunda y la radio
 * nunca descansa. */
static void schedule_retry(void)
{
    esp_timer_stop(s_retry_timer);
    ESP_LOGI(TAG, "reintento en %lu ms", (unsigned long)s_retry_delay_ms);
    esp_timer_start_once(s_retry_timer, (uint64_t)s_retry_delay_ms * 1000);

    s_retry_delay_ms *= 2;
    if (s_retry_delay_ms > KUNDT_WIFI_RETRY_MAX_MS) {
        s_retry_delay_ms = KUNDT_WIFI_RETRY_MAX_MS;
    }
}

static void wifi_event_handler(void            *arg,
                               esp_event_base_t base,
                               int32_t          id,
                               void            *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
        s_disconnects++;
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        strcpy(s_ip, "0.0.0.0");
        ESP_LOGW(TAG, "desconectado (motivo %d), total %lu",
                 ev ? ev->reason : -1, (unsigned long)s_disconnects);
        schedule_retry();
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = (const ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        /* Una asociación exitosa reinicia la espera, para que la próxima caída
         * vuelva a reintentar rápido. */
        s_retry_delay_ms = KUNDT_WIFI_RETRY_MIN_MS;
        esp_timer_stop(s_retry_timer);
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "conectado, ip=%s", s_ip);
        return;
    }
}

esp_err_t kundt_wifi_init(void)
{
    if (s_initialised) {
        return ESP_OK;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        return ESP_FAIL;
    }

    aplicar_ip_fija();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    const esp_timer_create_args_t timer_args = {
        .callback = &retry_timer_cb,
        .name     = "wifi_retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    s_initialised = true;
    return ESP_OK;
}

/*
 * Dirección fija, si la hay en NVS. Sin ella no se toca nada y la placa sigue
 * con DHCP, que es lo que hacen todas las ya desplegadas.
 *
 * Hay que parar el cliente DHCP ANTES de fijar la dirección: con él corriendo,
 * esp_netif_set_ip_info devuelve ESP_ERR_ESP_NETIF_DHCP_NOT_STOPPED y la
 * configuración se pierde en silencio.
 *
 * No se valida que la máscara abarque la puerta de enlace: si no lo hace, la
 * placa asocia al WiFi y queda incomunicada. Es responsabilidad de quien
 * provisiona, y por eso se registra en el log lo que se aplica.
 */
static void aplicar_ip_fija(void)
{
    kundt_config_t cfg_buf;
    if (kundt_config_get(&cfg_buf) != ESP_OK || cfg_buf.static_ip[0] == '\0') {
        return;  /* DHCP */
    }
    const kundt_config_t *cfg = &cfg_buf;

    esp_netif_ip_info_t ip = {0};
    if (esp_netif_str_to_ip4(cfg->static_ip, &ip.ip) != ESP_OK
        || esp_netif_str_to_ip4(cfg->netmask, &ip.netmask) != ESP_OK
        || esp_netif_str_to_ip4(cfg->gateway, &ip.gw) != ESP_OK) {
        ESP_LOGE(TAG, "IP fija mal formada (%s / %s / %s); se sigue con DHCP",
                 cfg->static_ip, cfg->netmask, cfg->gateway);
        return;
    }

    esp_err_t err = esp_netif_dhcpc_stop(s_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "esp_netif_dhcpc_stop: %s", esp_err_to_name(err));
        return;
    }

    err = esp_netif_set_ip_info(s_netif, &ip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_set_ip_info: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGW(TAG, "IP fija: %s máscara %s puerta %s",
             cfg->static_ip, cfg->netmask, cfg->gateway);
}

esp_err_t kundt_wifi_connect(const char *ssid, const char *password)
{
    if (!s_initialised) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == NULL || password == NULL || ssid[0] == '\0') {
        ESP_LOGE(TAG, "SSID sin configurar: hay que provisionarlo primero");
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;  /* Acepta APs abiertos y cifrados. */

    /*
     * Se barren todos los canales en vez de parar en el primer AP que responda.
     * WIFI_FAST_SCAN, que es el valor por defecto, termina "after find SSID
     * match AP": con una red que no anuncia su SSID eso depende de que la sonda
     * dirigida caiga en el canal correcto pronto. Barrer entero cuesta unos
     * segundos más en el primer arranque y quita esa dependencia.
     *
     * De paso, con un AP de doble banda que repite SSID en varios canales, esto
     * elige el de mejor señal en lugar del primero que conteste.
     */
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "conectando a \"%s\"", ssid);
    return ESP_OK;
}

bool kundt_wifi_is_connected(void)
{
    if (s_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t kundt_wifi_wait_connected(uint32_t timeout_ms)
{
    if (s_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        s_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));

    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

const char *kundt_wifi_ip(void)
{
    return s_ip;
}

uint32_t kundt_wifi_disconnect_count(void)
{
    return s_disconnects;
}

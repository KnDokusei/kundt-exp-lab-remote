/*
 * kundt_config.c - ver kundt_config.h.
 */

#include "kundt_config.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "kundt_config";

#define NVS_NAMESPACE "kundt"
#define KEY_SSID      "ssid"
#define KEY_PASSWORD  "pass"
#define KEY_SERVER_IP "srv_ip"
#define KEY_KIT       "kit"
#define KEY_PLATFORM  "plat"
#define KEY_CONTROLLER "ctrl"
#define KEY_STATIC_IP  "ip"
#define KEY_NETMASK    "mask"
#define KEY_GATEWAY    "gw"

static kundt_config_t s_cfg;
static bool           s_loaded;

/* Copia a un buffer fijo y siempre termina la cadena. Devuelve false si el
 * origen no cabe, para que el llamador rechace en vez de truncar en silencio. */
static bool copy_bounded(char *dst, size_t dst_size, const char *src)
{
    if (src == NULL) {
        return false;
    }
    const size_t len = strlen(src);
    if (len >= dst_size) {
        return false;
    }
    memcpy(dst, src, len + 1);
    return true;
}

static esp_err_t nvs_get_str_or_default(nvs_handle_t h,
                                        const char  *key,
                                        char        *dst,
                                        size_t       dst_size,
                                        const char  *fallback)
{
    size_t    len = dst_size;
    esp_err_t err = nvs_get_str(h, key, dst, &len);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        if (!copy_bounded(dst, dst_size, fallback)) {
            dst[0] = '\0';
        }
        return ESP_OK;
    }
    return err;
}

static esp_err_t store_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t kundt_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "la partición NVS necesita borrarse; reformateando");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init falló: %s", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t h;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        /* Sin namespace: primer arranque. Se siembra con los valores de compilación. */
        ESP_LOGI(TAG, "sin configuración guardada; sembrando desde Kconfig");
        copy_bounded(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), CONFIG_KUNDT_DEFAULT_WIFI_SSID);
        copy_bounded(s_cfg.wifi_password, sizeof(s_cfg.wifi_password), CONFIG_KUNDT_DEFAULT_WIFI_PASSWORD);
        copy_bounded(s_cfg.server_ip, sizeof(s_cfg.server_ip), CONFIG_KUNDT_DEFAULT_SERVER_IP);
        s_cfg.kit           = CONFIG_KUNDT_DEFAULT_KIT;
        s_cfg.platform_id   = CONFIG_KUNDT_DEFAULT_PLATFORM_ID;
        s_cfg.controller_id = CONFIG_KUNDT_DEFAULT_CONTROLLER_ID;
        s_loaded  = true;

        kundt_config_set_wifi(s_cfg.wifi_ssid, s_cfg.wifi_password);
        kundt_config_set_server_ip(s_cfg.server_ip);
        kundt_config_set_kit(s_cfg.kit);
        kundt_config_set_ids(s_cfg.platform_id, s_cfg.controller_id);
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open falló: %s", esp_err_to_name(err));
        return err;
    }

    nvs_get_str_or_default(h, KEY_SSID, s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid),
                           CONFIG_KUNDT_DEFAULT_WIFI_SSID);
    nvs_get_str_or_default(h, KEY_PASSWORD, s_cfg.wifi_password, sizeof(s_cfg.wifi_password),
                           CONFIG_KUNDT_DEFAULT_WIFI_PASSWORD);
    /* Sin valor por defecto: la ausencia de la clave significa DHCP. */
    nvs_get_str_or_default(h, KEY_STATIC_IP, s_cfg.static_ip, sizeof(s_cfg.static_ip), "");
    nvs_get_str_or_default(h, KEY_NETMASK,   s_cfg.netmask,   sizeof(s_cfg.netmask),   "");
    nvs_get_str_or_default(h, KEY_GATEWAY,   s_cfg.gateway,   sizeof(s_cfg.gateway),   "");

    nvs_get_str_or_default(h, KEY_SERVER_IP, s_cfg.server_ip, sizeof(s_cfg.server_ip),
                           CONFIG_KUNDT_DEFAULT_SERVER_IP);

    uint8_t kit = 0;
    if (nvs_get_u8(h, KEY_KIT, &kit) != ESP_OK || kit < KUNDT_KIT_MIN || kit > KUNDT_KIT_MAX) {
        kit = CONFIG_KUNDT_DEFAULT_KIT;
    }
    s_cfg.kit = kit;

    /* Un cero guardado no es un id válido: se trata como ausente y se recurre
     * al valor de compilación, igual que hace el kit. */
    uint8_t id = 0;
    s_cfg.platform_id   = (nvs_get_u8(h, KEY_PLATFORM,   &id) == ESP_OK && id != 0)
                          ? id : CONFIG_KUNDT_DEFAULT_PLATFORM_ID;
    id = 0;
    s_cfg.controller_id = (nvs_get_u8(h, KEY_CONTROLLER, &id) == ESP_OK && id != 0)
                          ? id : CONFIG_KUNDT_DEFAULT_CONTROLLER_ID;

    nvs_close(h);
    s_loaded = true;
    return ESP_OK;
}

esp_err_t kundt_config_get(kundt_config_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    *out = s_cfg;
    return ESP_OK;
}

esp_err_t kundt_config_set_wifi(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!copy_bounded(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), ssid) ||
        !copy_bounded(s_cfg.wifi_password, sizeof(s_cfg.wifi_password), password)) {
        ESP_LOGE(TAG, "SSID o contraseña demasiado largos");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = store_str(KEY_SSID, s_cfg.wifi_ssid);
    if (err == ESP_OK) {
        err = store_str(KEY_PASSWORD, s_cfg.wifi_password);
    }
    return err;
}

esp_err_t kundt_config_set_kit(uint8_t kit)
{
    if (kit < KUNDT_KIT_MIN || kit > KUNDT_KIT_MAX) {
        ESP_LOGE(TAG, "kit %u fuera de %d..%d", kit, KUNDT_KIT_MIN, KUNDT_KIT_MAX);
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(h, KEY_KIT, kit);
    if (err == ESP_OK) {
        err = nvs_commit(h);
        s_cfg.kit = kit;
    }
    nvs_close(h);
    return err;
}

esp_err_t kundt_config_set_server_ip(const char *ip)
{
    if (!copy_bounded(s_cfg.server_ip, sizeof(s_cfg.server_ip), ip)) {
        ESP_LOGE(TAG, "la IP del servidor es demasiado larga");
        return ESP_ERR_INVALID_ARG;
    }
    return store_str(KEY_SERVER_IP, s_cfg.server_ip);
}

uint16_t kundt_config_ws_port(void)
{
    return (uint16_t)(KUNDT_WS_PORT_BASE + s_cfg.kit);
}

esp_err_t kundt_config_set_ids(uint8_t platform_id, uint8_t controller_id)
{
    if (platform_id == 0 || controller_id == 0) {
        ESP_LOGE(TAG, "platform_id y controller_id deben ser distintos de cero");
        return ESP_ERR_INVALID_ARG;
    }

    s_cfg.platform_id   = platform_id;
    s_cfg.controller_id = controller_id;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, KEY_PLATFORM, platform_id);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_CONTROLLER, controller_id);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t kundt_config_broker_uri(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int n = snprintf(buf, buf_len, "mqtt://%s:%u",
                           s_cfg.server_ip, (unsigned)KUNDT_MQTT_PORT);
    if (n < 0 || (size_t)n >= buf_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t kundt_config_ws_uri(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int n = snprintf(buf, buf_len, "ws://%s:%u/",
                           s_cfg.server_ip, (unsigned)kundt_config_ws_port());
    if (n < 0 || (size_t)n >= buf_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

bool kundt_config_is_provisioned(void)
{
    return s_loaded && s_cfg.wifi_ssid[0] != '\0' && s_cfg.server_ip[0] != '\0';
}

void kundt_config_log(void)
{
    ESP_LOGI(TAG, "kit=%u  ssid=\"%s\"  server=%s  ws_port=%u",
             (unsigned)s_cfg.kit,
             s_cfg.wifi_ssid,
             s_cfg.server_ip,
             (unsigned)kundt_config_ws_port());
    ESP_LOGI(TAG, "contraseña: %s", s_cfg.wifi_password[0] ? "<definida>" : "<vacía>");
    ESP_LOGI(TAG, "curiousBeagle: platform=%u controller=%u  ->  kundt/%u/%u",
             (unsigned)s_cfg.platform_id, (unsigned)s_cfg.controller_id,
             (unsigned)s_cfg.platform_id, (unsigned)s_cfg.controller_id);
}

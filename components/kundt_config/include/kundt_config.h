/*
 * kundt_config.h - Configuración de ejecución guardada en NVS.
 *
 * La versión Arduino llevaba SSID, contraseña y número de kit como #defines en
 * wifiConfig.h, lo que significaba (a) credenciales dentro del árbol de fuentes y
 * (b) un binario propio por kit: 30 en total entre los cinco kits y los cuatro
 * módulos. Aquí esos mismos valores viven en NVS y se fijan en tiempo de
 * ejecución, así un binario sirve para todos los kits y no se versiona ningún
 * secreto.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KUNDT_SSID_MAX_LEN     32
#define KUNDT_PASSWORD_MAX_LEN 64
#define KUNDT_IP_MAX_LEN       15

#define KUNDT_KIT_MIN 1
#define KUNDT_KIT_MAX 5

/* El endpoint original era "808" concatenado con el dígito del kit, así que el
 * kit 1 escucha en 8081 y el kit 5 en 8085. */
#define KUNDT_WS_PORT_BASE 8080

/* Puerto MQTT del broker. Fijo: Mosquitto escucha en el 1883 tanto en el banco
 * local como en el despliegue de curiousBeagle. Se deja aquí y no en NVS para
 * no añadir una clave que nunca cambia. */
#define KUNDT_MQTT_PORT 1883

typedef struct {
    char     wifi_ssid[KUNDT_SSID_MAX_LEN + 1];
    char     wifi_password[KUNDT_PASSWORD_MAX_LEN + 1];
    char     server_ip[KUNDT_IP_MAX_LEN + 1];
    uint8_t  kit;  /* 1..5 */
    /* Identidad dentro de curiousBeagle. Forma el tópico MQTT
     * dev-status/kundt/<platform_id>/<controller_id>. No coincide con la
     * numeración de kit, por eso va aparte. */
    uint8_t  platform_id;
    uint8_t  controller_id;

    /*
     * Dirección fija opcional. Vacía = DHCP, que es el comportamiento de
     * siempre y el de todas las placas ya desplegadas.
     *
     * Existe porque el laboratorio asigna una dirección fija a cada placa y
     * curiousBeagle registra cada cámara por su IP: con DHCP cambian, y el
     * firmware no tenía forma de tomarlas. OJO: la máscara tiene que abarcar la
     * puerta de enlace, o la placa asocia al WiFi y queda incomunicada.
     */
    char     static_ip[KUNDT_IP_MAX_LEN + 1];
    char     netmask[KUNDT_IP_MAX_LEN + 1];
    char     gateway[KUNDT_IP_MAX_LEN + 1];
} kundt_config_t;

/**
 * @brief Inicializa NVS y carga la configuración almacenada.
 *
 * En el primer arranque (o tras un borrado) recurre a los valores por defecto de
 * Kconfig y los persiste, para que una placa nueva parta en un estado conocido.
 */
esp_err_t kundt_config_init(void);

/** @brief Copia la configuración activa. */
esp_err_t kundt_config_get(kundt_config_t *out);

/** @brief Guarda las credenciales WiFi. Rigen en la próxima conexión. */
esp_err_t kundt_config_set_wifi(const char *ssid, const char *password);

/** @brief Guarda el número de kit; rechaza cualquier valor fuera de 1..5. */
esp_err_t kundt_config_set_kit(uint8_t kit);

/** @brief Guarda la dirección del servidor (cadena en decimal punteado). */
esp_err_t kundt_config_set_server_ip(const char *ip);

/** @brief Fija la identidad de este equipo dentro de curiousBeagle. */
esp_err_t kundt_config_set_ids(uint8_t platform_id, uint8_t controller_id);

/**
 * @brief Arma la URI del broker MQTT, p. ej. "mqtt://192.168.0.100:1883".
 *
 * Reutiliza la IP del servidor: en el banco local y en el despliegue real el
 * broker vive en el mismo host que la API.
 *
 * @return ESP_OK, o ESP_ERR_INVALID_SIZE si @p buf es demasiado pequeño.
 */
esp_err_t kundt_config_broker_uri(char *buf, size_t buf_len);

/** @brief Puerto WebSocket del kit activo (KUNDT_WS_PORT_BASE + kit). */
uint16_t kundt_config_ws_port(void);

/**
 * @brief Arma la URI de WebSocket del kit activo, p. ej. "ws://192.168.0.100:8081/".
 * @return ESP_OK, o ESP_ERR_INVALID_SIZE si @p buf es demasiado pequeño.
 */
esp_err_t kundt_config_ws_uri(char *buf, size_t buf_len);

/** @brief Verdadero cuando el SSID y la dirección del servidor no están vacíos. */
bool kundt_config_is_provisioned(void);

/** @brief Registra la configuración activa. La contraseña nunca se imprime. */
void kundt_config_log(void);

#ifdef __cplusplus
}
#endif

/*
 * kundt_led.h - LED de estado compartido por los módulos del tubo de Kundt.
 *
 * El LED parpadea a 1 Hz como heartbeat: confirma que el firmware sigue vivo,
 * sin codificar el estado en el patrón. El estado actual (kundt_led_get_state)
 * y las transiciones (kundt_led_set_state) van por log/serial.
 *
 * En la DOIT DEVKIT V1, GPIO2 es el LED azul.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KUNDT_LED_DEFAULT_GPIO 2

/*
 * Cada estado tiene su patrón de parpadeo, contable a simple vista. Ver la
 * tabla PATRONES en kundt_led.c: la ranura son 100 ms y el ciclo 1,6 s.
 *
 *   ARRANQUE      parpadeo rápido continuo
 *   SIN WIFI      2 destellos y pausa
 *   SIN SERVIDOR  3 destellos y pausa
 *   EN MARCHA     1 destello corto cada 1,6 s, como un latido
 *   SIN DATOS     encendido largo y apagado largo
 *   FALLO         encendido fijo
 *   OCUPADO       parpadeo medio continuo, 200 ms
 */
typedef enum {
    KUNDT_LED_BOOT = 0,   /* Arrancando */
    KUNDT_LED_NO_WIFI,    /* Sin red */
    KUNDT_LED_NO_SERVER,  /* WiFi arriba, backend inalcanzable */
    KUNDT_LED_RUNNING,    /* Todo funcionando */
    KUNDT_LED_SELFTEST,   /* Compilación de banco, NO de producción */
    KUNDT_LED_NO_DATA,    /* Conectado, pero el sensor no entrega nada */
    KUNDT_LED_FAULT,      /* Estado de fallo: el módulo se detuvo */
    KUNDT_LED_BUSY,       /* Ejecutando algo con movimiento: no interrumpir */
} kundt_led_state_t;

/**
 * @brief Arranca la tarea del LED. Se puede llamar antes que todo lo demás.
 * @param gpio Pin que maneja el LED; KUNDT_LED_DEFAULT_GPIO para el de la placa.
 */
esp_err_t kundt_led_init(int gpio);

/** @brief Registra el estado actual. No cambia el heartbeat del LED. */
void kundt_led_set_state(kundt_led_state_t state);

/** @brief Estado actual registrado. */
kundt_led_state_t kundt_led_get_state(void);

/**
 * @brief Marca la compilación como autoprueba de banco (sólo deja rastro en log).
 *
 * Una compilación de autoprueba mueve pines fuera de donde los pone el
 * esquemático (el servo de E2 pasa de GPIO21 a GPIO25); esto sirve para
 * distinguirla por log/serial al arrancar, no hay aviso visual por LED.
 */
void kundt_led_mark_selftest(void);

/** @brief Nombre legible de un estado, para el log. */
const char *kundt_led_state_name(kundt_led_state_t state);

#ifdef __cplusplus
}
#endif

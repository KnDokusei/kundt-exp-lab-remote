/*
 * kundt_ota.h - Actualización de firmware por aire.
 *
 * DIRECCIONAMIENTO
 *
 * Cada placa se suscribe a dos tópicos, uno propio y uno colectivo:
 *
 *   kundt-ota/<modulo>/<platform_id>   sólo esa placa
 *   kundt-ota/<modulo>/all             ese módulo en todos los kits
 *
 * donde <modulo> es "e1", "e2" o "e3". Así se puede actualizar el E1 del kit 3
 * sin tocar nada más, o el E1 de los cinco kits de una vez.
 *
 * El módulo SIEMPRE va explícito, y no es un descuido: cada uno necesita su
 * propio binario, así que un "todos los módulos de un kit" mandaría la imagen de
 * E1 a un E3. Actualizar un kit entero son tres publicaciones, y de eso se
 * encarga la herramienta.
 *
 * SEGURIDAD
 *
 * Recibir un aviso NO actualiza nada. Sólo deja una orden pendiente; la máquina
 * de estados del módulo decide cuándo atenderla, y lo hace únicamente desde un
 * estado en el que se sabe que no hay nada en marcha. E3 mueve un motor: una
 * actualización a mitad de un movimiento es justo lo que no debe pasar.
 *
 * Y el arranque de una imagen nueva queda a prueba: si no se llama a
 * kundt_ota_confirmar() el bootloader revierte a la anterior en el siguiente
 * reinicio. Conviene confirmar cuando haya WiFi y MQTT, no con sólo arrancar.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longitud máxima de la URL de la imagen. */
#define KUNDT_OTA_URL_MAX 160

/**
 * @brief Se suscribe a los tópicos de actualización de este módulo.
 *
 * @param modulo       "e1", "e2" o "e3". Forma parte del tópico.
 * @param broker_uri   el mismo que usa kundt_mqtt.
 * @param platform_id  identidad del kit.
 */
esp_err_t kundt_ota_start(const char *modulo, const char *broker_uri,
                          uint8_t platform_id);

/** @brief Verdadero si llegó una orden y todavía no se ha atendido. */
bool kundt_ota_pendiente(void);

/**
 * @brief Descarga y graba la imagen pendiente. Bloquea; al terminar reinicia.
 *
 * Llamar sólo desde un estado seguro del módulo. Si vuelve, es que falló: la
 * partición activa no se toca y la orden queda descartada.
 */
esp_err_t kundt_ota_aplicar(void);

/**
 * @brief Marca la imagen en marcha como buena y cancela el rollback.
 *
 * Sin esto, el siguiente reinicio vuelve a la imagen anterior. Llamar cuando el
 * módulo haya demostrado que funciona, no al arrancar.
 */
void kundt_ota_confirmar(void);

/** @brief Verdadero si esta imagen está a prueba y aún no se ha confirmado. */
bool kundt_ota_a_prueba(void);

/** @brief Registra partición activa, versión y estado de confirmación. */
void kundt_ota_log(void);

#ifdef __cplusplus
}
#endif

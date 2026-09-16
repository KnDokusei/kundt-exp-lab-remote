# PROYECTO_IOT_TUBO

Firmware **ESP-IDF** para el Laboratorio Remoto — Tubo de Kundt (USM).

Port del firmware original de Arduino IDE (Jose Borquez, ago. 2024;
Prof. Alfredo Navarro) a ESP-IDF v5.5.5.

## El experimento

Parlante en un extremo del tubo, émbolo motorizado en el otro, micrófono en la
entrada. Al mover el émbolo se atraviesan las resonancias (L = n·λ/2), y de la
separación entre ellas se despeja la velocidad del sonido.

## Módulos

Cada carpeta es un proyecto ESP-IDF independiente, con su placa, compilable y
flasheable por separado. Se coordinan sólo a través del servidor, por MQTT.

| Módulo | Qué hace | Estado |
|---|---|---|
| `E1-Mic` | Mide la amplitud del tono con Goertzel y publica escalares | En placa, validado |
| `E2-SineGen` | Genera el tono (AD9833) y el volumen (servo) | En placa, validado |
| `E3-StepMotor` | Mueve el émbolo (A4988) y reporta su posición | En placa, calibra |
| `EC-Cameras` | Tres ESP32-CAM sirviendo MJPEG | En placa, 3 funcionando |

Los tres primeros comparten `platform_id` y `controller_id` a propósito: son un
solo controlador lógico y se distinguen por los campos que publican.

## Compilar y flashear

```bash
source ~/esp/esp-idf/export.sh
cd E1-Mic                      # o el módulo que toque
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 -b 115200 flash monitor
```

**Siempre a 115200.** A 460800 estos CP2102 fallan con
`Unable to verify flash chip connection`.

`sdkconfig` está en `.gitignore`: nunca se versiona, porque contiene la
contraseña WiFi.

## Dar identidad a una placa

La configuración vive en NVS y **NVS gana sobre Kconfig**. Eso permite un solo
binario por módulo para los cinco kits:

```bash
tools/provision/kundt-provision.sh --kit 3 --port /dev/ttyUSB0
```

Graba una partición NVS en 0x9000 en 0,1 s, sin tocar la aplicación. La red y la
IP del servidor salen de `~/.kundt-provision.conf`, fuera del repositorio; la
contraseña, de `~/.kundt-wifi-pass` o por teclado, **nunca por argumento**.

```
sin provisionador   5 kits x 3 modulos = 15 binarios
con provisionador   4 binarios + 0,1 s por placa
```

Tras un `erase-flash` hay que reprovisionar: un `flash` normal no cambia la NVS.

## Actualización por aire (OTA)

```bash
tools/provision/kundt-ota.sh --modulo e1 --kit 1
```

Sirve la imagen desde un HTTP efímero en el PC y publica la URL. Los tópicos
(`kundt-ota/<modulo>/<kit>` y `.../all`) están fuera del contrato del servidor a
propósito. Una imagen nueva arranca a prueba: si no alcanza WiFi y broker, el
bootloader revierte sola.

## Tests

```bash
cd test/host && make            # E1, E2 y E3: 75 + 234 + 244 = 553
```

Compilan la misma unidad de DSP que el firmware, con gcc.

## Aporte al servidor

`server-kundt/` contiene el alta del experimento en **curiousBeagle**, el
servidor del laboratorio (repositorios privados de otro equipo).

`server-kundt/entrega/` es la entrega lista para revisar: serie de parches
aplicable con `git am`, con su README explicando qué toca, qué no toca y cómo
montarlo.

## Documentación

El análisis de la migración, los procedimientos de prueba y el checklist previo
a instalar se mantienen fuera de este repositorio.

## Autoría y licencia

Port a ESP-IDF del firmware original de **Jose Borquez Gaete** (agosto 2024)
para el Laboratorio Remoto — Tubo de Kundt, dirigido por el **Prof. Alfredo
Navarro**, Universidad Técnica Federico Santa María.

El diseño del experimento, los esquemáticos y la lógica de los cuatro módulos
son obra suya. Este trabajo migra ese firmware conservando su comportamiento, y
documenta las diferencias donde las hay.

> **Licencia pendiente.** No hay archivo `LICENSE` a propósito: al ser un trabajo
> derivado, la licencia corresponde definirla al autor original y al profesor a
> cargo. Hasta entonces todos los derechos quedan reservados y el código no debe
> redistribuirse sin su autorización.

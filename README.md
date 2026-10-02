# kundt-exp-lab-remote

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
| `E1-Mic` | Mide la amplitud del tono con Goertzel y publica escalares | En producción, kit 1 |
| `E2-SineGen` | Genera el tono (AD9833) y el volumen (servo) | En producción, kit 1 |
| `E3-StepMotor` | Mueve el émbolo (A4988) y reporta su posición | En producción, kit 1 |
| `EC-Cameras` | ESP32-CAM sirviendo MJPEG por HTTP | Respaldo |

Los tres primeros comparten `platform_id` y `controller_id` a propósito: son un
solo controlador lógico y se distinguen por los campos que publican.

En producción las cámaras corren el firmware RTSP de curiousBeagle, con los
arreglos de `camaras-rtsp/`.

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
tools/provision/kundt-ota.sh --modulo e1 --plataforma 8
```

Sirve la imagen desde un HTTP efímero en el PC y publica la URL. Los tópicos
(`kundt-ota/<modulo>/<platform_id>` y `.../all`) están fuera del contrato del servidor a
propósito. Una imagen nueva arranca a prueba: si no alcanza WiFi y broker, el
bootloader revierte sola.

**Se direcciona por plataforma, no por kit**: es el `platform_id` de curiousBeagle
y no coinciden (en producción el kit 1 es la plataforma 8). La orden se publica
en el broker que usan las placas, el de `~/.kundt-provision.conf`, y el PC tiene
que estar en la red del laboratorio para que puedan descargar la imagen.

## Tests

```bash
cd test/host && make            # E1, E2 y E3: 75 + 234 + 244 = 553
```

Compilan la misma unidad de DSP que el firmware, con gcc.

## Aporte al servidor

`server-kundt/` contiene el alta del experimento en **curiousBeagle**, el
servidor del laboratorio (repositorios privados de otro equipo). Cada entrega es
una serie de parches aplicable con `git am`, con su README.

| Entrega | Qué trae | Estado |
|---|---|---|
| `entrega-1/` | El tipo `kundt` en la API y la vista en el front | Tipo y vista fusionados en septiembre de 2026 (curiousBeagleAPI #38 y #40, curiousBeagleFront #77). La coalescencia (`0002`) y la siembra local (`0003`) no se incorporaron |
| `entrega-3/` | Caja de frecuencia, cámaras a 4:3 y tabla de mediciones con ajuste f vs 1/λ | Fusionada el 2026-10-02 (curiousBeagleFront #78) |

La entrega 2 quedó obsoleta antes de enviarse y no está en el repositorio.

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

> **Licencia pendiente.** No hay archivo `LICENSE` todavía: al ser un trabajo
> derivado, la licencia la definen el autor original y el profesor a cargo. El
> repositorio se publica con su visto bueno y, mientras no haya licencia, todos
> los derechos quedan reservados. Los parches de `camaras-rtsp/` son la
> excepción: modifican código GPL-3.0 y se rigen por esa licencia.

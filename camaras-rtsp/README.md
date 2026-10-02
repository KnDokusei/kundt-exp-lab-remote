# Cámaras con el firmware RTSP de curiousBeagle

En producción las cámaras del kit 1 corren `DopamineLabsLTDA/esp32CamFirmware`,
un fork de `rzeldent/esp32cam-rtsp` (GPL-3.0). Sirve `rtsp://<ip>:554/mjpeg/1`,
que es la URL que el servidor tiene registrada para cada cámara, así que no hace
falta tocar nada del lado del servidor. `EC-Cameras/` queda como respaldo por HTTP.

`parches/` son cinco arreglos sobre su commit `cf2075f`, todos en `src/main.cpp`:

| Parche | Qué arregla |
|---|---|
| `0001` | El servidor RTSP se crea una sola vez. IotWebConf llama a `on_connected()` en cada reconexión WiFi y el original abría otro servidor en el 554 con el anterior todavía escuchando: el RTSP quedaba muerto hasta reiniciar, aunque el ping y la web respondieran |
| `0002` | `WiFi.config()` antes de `WiFi.begin()`. En el orden original, si la primera asociación fallaba, el reintento del core volvía a DHCP y la cámara perdía su IP fija |
| `0003` | Reintento del sensor cada 30 s si no responde al encender, y la ruta `/restart`, que el botón de la página pedía y daba 404 |
| `0004` | Actualización del firmware por la web en `/firmware`, con el usuario `admin` y la contraseña del AP. La cámara se detiene antes de escribir la flash: con ella en marcha, el watchdog reiniciaba la placa al primer borrado |
| `0005` | `WiFi.setSleep(false)`. Con el ahorro de energía el AP retiene los ACK de TCP hasta el siguiente DTIM y el RTSP se atasca. El ping bajó de unos 100 ms a 13-34 ms |

## Aplicar

```bash
git clone https://github.com/DopamineLabsLTDA/esp32CamFirmware.git
cd esp32CamFirmware
git checkout -b kundt cf2075f
git am /ruta/a/camaras-rtsp/parches/*.patch
```

En `platformio.ini` conviene fijar además `platform = espressif32@7.1.3`, que es
la versión con la que se probó (el upstream no fija ninguna), y
`upload_speed = 115200`, porque los adaptadores USB-serie del laboratorio fallan
a 460800. Quedan fuera de los parches por ser ajustes de la máquina que compila.

## Cosas que conviene saber

- La configuración inicial va en `data/config.json`, se graba en LittleFS y el
  firmware la borra al primer arranque. Después se cambia en `http://<ip>/config`.
- Las casillas no se cargan desde el JSON: `BoolDataType::fromString` de
  IotWebConf compara con el byte `1`, no con el carácter. El volteo de una cámara
  montada al revés (`vm` y `hm`) se marca en la página de configuración.
- Su tabla de particiones (`min_spiffs`) no coincide con la de otros firmwares:
  si la placa traía otro, hacer `erase-flash` antes del primer flasheo.
- Cada arranque abre 30 s su punto de acceso antes de conectarse, así que el RTSP
  aparece unos 42 s después de encenderla.
- No abrir `/stream` con la cámara en producción: bloquea el bucle y congela el
  RTSP mientras dure. `/snapshot` sí se puede.

Los parches modifican código GPL-3.0 y se distribuyen bajo esa misma licencia.

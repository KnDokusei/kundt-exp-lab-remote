# Entrega: tipo de experimento `kundt` para curiousBeagle

Serie de parches lista para aplicar, pensada para que la revise y la monte quien
administra el servidor. Añade el tubo de Kundt siguiendo el mismo molde que
`Electron Diffraction` (commit `4a27491`, 8 archivos).

## Por qué parches y no un pull request

`DopamineLabsLTDA/curiousBeagleAPI` y `curiousBeagleFront` tienen
**`allow_forking: false`** y el autor de esta entrega sólo tiene permisos
`pull` y `triage`. No hay forma de publicar una rama, ni propia ni bifurcada.

Para pasar a pull request basta **una** de estas dos cosas, y entonces esta serie
se convierte en rama con un `git am`:

- activar el fork de repositorios privados en los ajustes de la organización, o
- conceder permiso de escritura para empujar una rama de función.

## Aplicar

```bash
# curiousBeagleAPI, sobre 972df80
git checkout -b kundt-tube main
git am /ruta/a/entrega-1/api/*.patch

# curiousBeagleFront, sobre 0aac322
git checkout -b kundt-tube main
git am /ruta/a/entrega-1/front/*.patch
```

El nombre de rama sigue el de `electron-diffraction`, y los mensajes de commit
siguen el estilo del repositorio: frase descriptiva, sin prefijos de conventional
commits.

Cada parche conserva autoría y mensaje. Si alguno no aplica, es que `main` avanzó:
`git am --3way` suele resolverlo.

## Qué contiene cada commit

### curiousBeagleAPI

| Commit | Qué hace |
|---|---|
| `0001` | El tipo `kundt`: los mismos 8 archivos que tocó `4a27491` |
| `0002` | Coalescencia de las publicaciones MQTT. Cambia comportamiento, va aparte a propósito |
| `0003` | Siembra idempotente para desarrollo local. **Prescindible**: si sobra, se descarta sin tocar los otros dos |

### curiousBeagleFront

| Commit | Qué hace |
|---|---|
| `0001` | La vista: tres constantes de ruta, cliente de API, tipos, cargador, página y una línea en el router |

## Por qué tres commits y no uno

`Electron Diffraction` entró en un solo commit. Aquí van tres porque el `0002`
**cambia comportamiento** (el ritmo de escritura a PostgreSQL) y el `0003` es
prescindible: separados se pueden revisar, aceptar o descartar por separado.
Si se prefiere un commit único, un squash al fusionar los junta.

## Lo que NO toca

Es la parte que más importa al revisar, así que está medida:

| | Líneas añadidas | Líneas borradas de código existente |
|---|---|---|
| curiousBeagleAPI | 557 | **2** |
| curiousBeagleFront | 974 | **0** |

Las dos líneas de la API son importaciones que se reescriben añadiendo
`KUNDT_TOPIC` a la lista:

```diff
-import { ELECTRON_DIFFRACTION_TOPIC, PENDULUM_TOPIC, TANK_TOPIC } from "src/mqtt/channels";
+import { ELECTRON_DIFFRACTION_TOPIC, KUNDT_TOPIC, PENDULUM_TOPIC, TANK_TOPIC } from "src/mqtt/channels";
```

La migración crea **una** tabla. Su único `ALTER TABLE` es sobre `Kundt` para
añadirle su propia clave foránea: no modifica `ExpBase`, `Tank`, `Pendulum` ni
`ElectronDiffraction`.

**Ningún componente compartido cambia.** El volteo vertical que necesita una de
las cámaras del montaje toca `CameraViewer` y `CamerasDisplay`, que usan también
ChargeMass, Tank y Pendulum, así que se dejó **fuera de esta entrega** y se
resolverá en el firmware de esa placa con `set_vflip()`.

## Dos avisos sobre el commit `0003` de siembra

- La siembra crea un usuario de tipo **`ADMIN`**. El script **exige**
  `KUNDT_TEST_PASS` y falla si no está: una contraseña por omisión escrita en un
  repositorio sería una cuenta de administrador con credencial conocida.
- `c_ctrl_ip` trae `192.168.0.60`, que era la red del montaje cuando se escribió.
  Es una dirección privada sin valor fuera de esa LAN, pero está desactualizada:
  conviene cambiarla o parametrizarla antes de usar la siembra en otro sitio.

## Decisiones del contrato

- **Un solo controlador lógico.** Los tres módulos ESP32 del tubo comparten
  `platform_id` y `controller_id` y se distinguen por los campos que publican.
  Partirlos en tres controladores partiría el estado del experimento en tres filas.
- **Cargas parciales.** Todos los campos del DTO son opcionales y el manejador
  fusiona en vez de reemplazar: escribir `undefined` en un `update` de Prisma deja
  la columna intacta. Sin esto, el módulo de micrófono borraría lo que el del
  émbolo acaba de reportar. Probado contra tres placas reales.
- **Las rutas usan `:id`, no `:platform_id`.** Ver la sección siguiente.
- **El micrófono manda escalares, no audio.** Publica amplitud a la frecuencia
  de excitación (Goertzel), RMS de banda ancha y pico, nunca forma de onda.

## Hallazgo no documentado: el payload MQTT es asimétrico

No aparece en ninguno de los tres repositorios y se descubrió ejecutando:

```
ctrl-channel:  {"pattern":"ctrl-channel/kundt/1/1","data":{"id":1,"actuators":{...}}}
dev-status:    {"device":{...},"sensors":{...}}          <- plano, sin envolver
```

NestJS **envuelve lo que publica** pero entiende el objeto plano que le llega. El
firmware tiene que leer `data.actuators`, no `actuators`. Vale para cualquier
experimento nuevo, no sólo para éste.

## Cosas encontradas en el código existente

Van aquí como contexto para quien revise. No las toca ningún parche de esta serie.

| Dónde | Qué pasa |
|---|---|
| `reservations.guard.ts` | `ValidReservationGuard` lee `request.params.id`, así que toda ruta que declare `:platform_id` responde 403 siempre. Tank usa `:id` y funciona; **Electron Diffraction usa `:platform_id`**. Kundt usa `:id` para no heredarlo |
| `mqtt-listener.service.ts` | `exp_entry!.id` con aserción de no-nulo: un `device.id` inexistente lanza `TypeError` y el mensaje se descarta en silencio. El manejador de `kundt` usa retorno temprano en su lugar |
| `site.service.ts:17` | Sin fila en `site."Site"`, `getReservationConfigs` devuelve `undefined`, el `JSON.parse` de `getBlocks` revienta y **todo endpoint protegido responde 500** con mensaje genérico. Ocurre en cualquier base recién creada |
| `postgresql/init-scripts/schema.sql` | Crea tablas kebab-case (`experiment-types`, `registered-users`) de un esquema anterior que hoy no consulta nadie |

## Verificado

Banco local con Mosquitto, PostgreSQL y la API nativa, contra **tres placas ESP32
reales** del montaje:

- Camino completo probado: HTTP → API → MQTT → dispositivo → MQTT → API →
  PostgreSQL → SSE → navegador.
- Fusión parcial con dos placas a la vez: ninguna borra lo que la otra reportó.
- Una sola publicación del servidor atendida por dos módulos distintos, cada uno
  tomando su campo.
- Cambios de frecuencia y volumen desde el navegador mueven el hardware.

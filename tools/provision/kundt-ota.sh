#!/usr/bin/env bash
#
# kundt-ota.sh - Manda una actualización de firmware por aire.
#
# DIRECCIONAMIENTO
#
#   --modulo e1 --plataforma 8      sólo el E1 de la plataforma 8
#   --modulo e1 --plataforma all    el E1 de todas las plataformas
#   --modulo all --plataforma 8     los tres módulos de la plataforma 8
#
# El tópico es kundt-ota/<modulo>/<platform_id>, y el platform_id es el que
# asigna curiousBeagle, NO el número de kit. En producción el kit 1 es la
# plataforma 8. Mientras coincidían daba igual; desde que no coinciden, una orden
# dirigida al kit llegaba a un tópico que ninguna placa escucha, sin ningún error.
# Por eso --kit ya no existe: si se usa, el script se niega en vez de adivinar.
#
# El módulo va siempre explícito porque cada uno necesita su propio binario:
# "--modulo all" no manda una imagen a todos, manda la de cada uno.
#
# A QUÉ BROKER PUBLICA
#
# Al mismo que usan las placas: KUNDT_SERVER de ~/.kundt-provision.conf, el
# mismo archivo del que sale su provisión. Antes publicaba en el broker local del
# PC, y en cuanto las placas pasaron al de producción las órdenes dejaron de
# llegarles. --broker lo sobreescribe.
#
# Los tópicos kundt-ota/... son nuestros y están fuera del contrato de
# curiousBeagle: publicarlos en su broker no toca nada de los demás experimentos.
#
# CÓMO LLEGA LA IMAGEN
#
# El script levanta un servidor HTTP efímero en este PC y publica su URL. Las
# placas descargan de aquí, así que el PC tiene que estar en la red del
# laboratorio: si no, reciben la orden pero no alcanzan la imagen.
#
# USO
#
#   ./kundt-ota.sh --modulo e2 --plataforma 8
#   ./kundt-ota.sh --modulo all --plataforma 8
#   ./kundt-ota.sh --modulo e1 --plataforma all --puerto 8123
#
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONF="${KUNDT_PROVISION_CONF:-$HOME/.kundt-provision.conf}"
MODULO=""; PLATAFORMA=""; PUERTO=8000; BROKER_HOST=""; SOLO_LISTAR=0; FORZAR=0
HERRAMIENTA="${KUNDT_MQTT_TOOL_CONTAINER:-cb-mqtt-broker}"

declare -A DIR=( [e1]="E1-Mic" [e2]="E2-SineGen" [e3]="E3-StepMotor" )
declare -A BIN=( [e1]="e1_mic.bin" [e2]="e2_sinegen.bin" [e3]="e3_stepmotor.bin" )

while [[ $# -gt 0 ]]; do
  case "$1" in
    --modulo)     MODULO="$2"; shift 2 ;;
    --plataforma) PLATAFORMA="$2"; shift 2 ;;
    --puerto)     PUERTO="$2"; shift 2 ;;
    --broker)     BROKER_HOST="$2"; shift 2 ;;
    --dry-run)    SOLO_LISTAR=1; shift ;;
    --force)      FORZAR=1; shift ;;
    --kit)
      echo "--kit ya no existe: el tópico usa el platform_id de curiousBeagle," >&2
      echo "que no coincide con el kit (el kit 1 es la plataforma 8)." >&2
      echo "Usa --plataforma. Ver ~/kundt-tools/inventario-placas.txt." >&2
      exit 1 ;;
    -h|--help) sed -n '2,42p' "$0"; exit 0 ;;
    *) echo "opción desconocida: $1" >&2; exit 1 ;;
  esac
done

[[ -z "$MODULO" ]] && { echo "falta --modulo (e1|e2|e3|all)" >&2; exit 1; }
[[ -z "$PLATAFORMA" ]] && { echo "falta --plataforma (su id en curiousBeagle, o all)" >&2; exit 1; }
[[ "$MODULO" =~ ^(e1|e2|e3|all)$ ]] || { echo "módulo inválido: $MODULO" >&2; exit 1; }
[[ "$PLATAFORMA" =~ ^([1-9][0-9]{0,2}|all)$ ]] || { echo "plataforma inválida: $PLATAFORMA" >&2; exit 1; }

MODULOS=()
if [[ "$MODULO" == "all" ]]; then MODULOS=(e1 e2 e3); else MODULOS=("$MODULO"); fi

# El broker de las placas: el mismo de su provisión.
if [[ -z "$BROKER_HOST" && -f "$CONF" ]]; then
    # shellcheck disable=SC1090
    BROKER_HOST="$(. "$CONF" && echo "${KUNDT_SERVER:-}")"
fi
[[ -z "$BROKER_HOST" ]] && { echo "no sé a qué broker publicar: falta KUNDT_SERVER en $CONF o --broker" >&2; exit 1; }

# La IP que las placas tienen que alcanzar para descargar, no 127.0.0.1.
IP="$(ip -4 route get 1.1.1.1 2>/dev/null | grep -oP 'src \K[\d.]+' | head -1)"
[[ -z "$IP" ]] && { echo "no se pudo determinar la IP de este PC" >&2; exit 1; }

# Las placas descargan de este PC: tiene que estar en la red del laboratorio.
RED_LAB="$( [[ -f "$CONF" ]] && . "$CONF" && echo "${KUNDT_LAB_NET:-}" )"
RED_LAB="${RED_LAB:-10.31.204}"
if [[ "$IP" != "$RED_LAB".* && "$FORZAR" != "1" ]]; then
    echo "este PC está en $IP, fuera de la red del laboratorio ($RED_LAB.0/24)." >&2
    echo "Las placas recibirían la orden pero no podrían descargar la imagen." >&2
    echo "Conecta el PC a esa red, o usa --force si sabes que la alcanzan." >&2
    exit 1
fi

# Publicar con el cliente del sistema si lo hay; si no, con el del contenedor.
publicar() {
    if command -v mosquitto_pub >/dev/null; then
        mosquitto_pub -h "$BROKER_HOST" -p 1883 -t "$1" -m "$2"
    else
        docker exec "$HERRAMIENTA" mosquitto_pub -h "$BROKER_HOST" -p 1883 -t "$1" -m "$2"
    fi
}

echo "  broker: $BROKER_HOST:1883"
nc -z -w5 "$BROKER_HOST" 1883 2>/dev/null \
    || { echo "el broker $BROKER_HOST:1883 no responde desde este PC" >&2; exit 1; }

# Se sirve un directorio temporal con sólo los binarios que toca publicar: así
# el servidor efímero no expone el árbol entero del proyecto.
SRV="$(mktemp -d /tmp/kundt-ota-XXXX)"
# La limpieza va en una función: con set -e, la primera orden que falla dentro
# de un trap aborta el resto, y en --dry-run (sin PID) eso se llevaba por
# delante el borrado del directorio temporal.
limpiar() {
    if [[ -n "${PID:-}" ]]; then
        kill "$PID" 2>/dev/null || true
        wait "$PID" 2>/dev/null || true
    fi
    rm -rf "$SRV" 2>/dev/null || true
}
trap limpiar EXIT

for m in "${MODULOS[@]}"; do
  ORIGEN="$RAIZ/${DIR[$m]}/build/${BIN[$m]}"
  [[ -f "$ORIGEN" ]] || { echo "falta $ORIGEN; compila ${DIR[$m]} primero" >&2; exit 1; }
  cp "$ORIGEN" "$SRV/"
  printf "  %-4s %s  (%s bytes)\n" "$m" "${BIN[$m]}" "$(stat -c%s "$ORIGEN")"
done
echo

if [[ "$SOLO_LISTAR" == "1" ]]; then
  for m in "${MODULOS[@]}"; do
    echo "  kundt-ota/$m/$PLATAFORMA  ->  http://$IP:$PUERTO/${BIN[$m]}"
  done
  echo "  --dry-run: no se publica nada"
  exit 0
fi

# Sin subshell: con "( ... ) &" el $! es el PID de la subshell, no el de python,
# y el kill del trap dejaba el servidor huérfano sirviendo un directorio ya
# borrado. El puerto quedaba ocupado y la siguiente actualización fallaba.
# El registro de accesos se conserva: saber si la placa llegó a pedir la imagen
# es lo que distingue "no recibió la orden" de "la recibió y falló al bajarla".
ACCESOS="$SRV/.accesos.log"
python3 -m http.server "$PUERTO" --bind 0.0.0.0 --directory "$SRV" > "$ACCESOS" 2>&1 &
PID=$!
sleep 1
kill -0 "$PID" 2>/dev/null || { echo "no se pudo abrir el puerto $PUERTO" >&2; exit 1; }
echo "  sirviendo en http://$IP:$PUERTO"
echo

for m in "${MODULOS[@]}"; do
  URL="http://$IP:$PUERTO/${BIN[$m]}"
  TOPICO="kundt-ota/$m/$PLATAFORMA"
  publicar "$TOPICO" "{\"url\":\"$URL\"}"
  echo "  publicado en $TOPICO"
done

echo
echo "  El servidor sigue abierto 120 s para que las placas descarguen."
sleep 120

echo
echo "  Peticiones recibidas:"
if grep -q "GET" "$ACCESOS" 2>/dev/null; then
    grep "GET" "$ACCESOS" | sed "s/^/    /"
else
    echo "    ninguna: las placas no llegaron a pedir la imagen"
fi

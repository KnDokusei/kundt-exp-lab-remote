#!/usr/bin/env bash
#
# kundt-ota.sh - Manda una actualización de firmware por aire.
#
# DIRECCIONAMIENTO
#
#   --modulo e1 --kit 3      sólo el E1 del kit 3
#   --modulo e1 --kit all    el E1 de todos los kits
#   --modulo all --kit 3     los tres módulos del kit 3 (tres publicaciones)
#
# El módulo va siempre explícito en el tópico porque cada uno necesita su propio
# binario: "--modulo all" no manda una imagen a todos, manda la de cada uno.
#
# CÓMO LLEGA LA IMAGEN
#
# El script levanta un servidor HTTP efímero sobre el directorio de compilación
# y publica su URL. No hace falta nada en el servidor de curiousBeagle: las
# placas descargan directamente del PC.
#
# USO
#
#   ./kundt-ota.sh --modulo e2 --kit 1
#   ./kundt-ota.sh --modulo all --kit 1
#   ./kundt-ota.sh --modulo e1 --kit all --puerto 8123
#
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MODULO=""; KIT=""; PUERTO=8000; BROKER="cb-mqtt-broker"; SOLO_LISTAR=0

declare -A DIR=( [e1]="E1-Mic" [e2]="E2-SineGen" [e3]="E3-StepMotor" )
declare -A BIN=( [e1]="e1_mic.bin" [e2]="e2_sinegen.bin" [e3]="e3_stepmotor.bin" )

while [[ $# -gt 0 ]]; do
  case "$1" in
    --modulo)  MODULO="$2"; shift 2 ;;
    --kit)     KIT="$2"; shift 2 ;;
    --puerto)  PUERTO="$2"; shift 2 ;;
    --broker)  BROKER="$2"; shift 2 ;;
    --dry-run) SOLO_LISTAR=1; shift ;;
    -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
    *) echo "opción desconocida: $1" >&2; exit 1 ;;
  esac
done

[[ -z "$MODULO" ]] && { echo "falta --modulo (e1|e2|e3|all)" >&2; exit 1; }
[[ -z "$KIT" ]] && { echo "falta --kit (1..5|all)" >&2; exit 1; }
[[ "$MODULO" =~ ^(e1|e2|e3|all)$ ]] || { echo "módulo inválido: $MODULO" >&2; exit 1; }
[[ "$KIT" =~ ^([1-5]|all)$ ]] || { echo "kit inválido: $KIT" >&2; exit 1; }

MODULOS=()
if [[ "$MODULO" == "all" ]]; then MODULOS=(e1 e2 e3); else MODULOS=("$MODULO"); fi

# La IP que las placas tienen que poder alcanzar, no 127.0.0.1.
IP="$(ip -4 route get 1.1.1.1 2>/dev/null | grep -oP 'src \K[\d.]+' | head -1)"
[[ -z "$IP" ]] && { echo "no se pudo determinar la IP de este PC" >&2; exit 1; }

# Se sirve un directorio temporal con sólo los binarios que toca publicar: así
# el servidor efímero no expone el árbol entero del proyecto.
SRV="$(mktemp -d /tmp/kundt-ota-XXXX)"
trap 'rm -rf "$SRV"; [[ -n "${PID:-}" ]] && kill "$PID" 2>/dev/null || true' EXIT

for m in "${MODULOS[@]}"; do
  ORIGEN="$RAIZ/${DIR[$m]}/build/${BIN[$m]}"
  [[ -f "$ORIGEN" ]] || { echo "falta $ORIGEN; compila ${DIR[$m]} primero" >&2; exit 1; }
  cp "$ORIGEN" "$SRV/"
  printf "  %-4s %s  (%s bytes)\n" "$m" "${BIN[$m]}" "$(stat -c%s "$ORIGEN")"
done
echo

if [[ "$SOLO_LISTAR" == "1" ]]; then
  for m in "${MODULOS[@]}"; do
    echo "  kundt-ota/$m/$KIT  ->  http://$IP:$PUERTO/${BIN[$m]}"
  done
  echo "  --dry-run: no se publica nada"
  exit 0
fi

( cd "$SRV" && python3 -m http.server "$PUERTO" --bind 0.0.0.0 >/dev/null 2>&1 ) &
PID=$!
sleep 1
kill -0 "$PID" 2>/dev/null || { echo "no se pudo abrir el puerto $PUERTO" >&2; exit 1; }
echo "  sirviendo en http://$IP:$PUERTO"
echo

for m in "${MODULOS[@]}"; do
  URL="http://$IP:$PUERTO/${BIN[$m]}"
  TOPICO="kundt-ota/$m/$KIT"
  docker exec "$BROKER" mosquitto_pub -h localhost -t "$TOPICO" \
         -m "{\"url\":\"$URL\"}"
  echo "  publicado en $TOPICO"
done

echo
echo "  El servidor sigue abierto 120 s para que las placas descarguen."
echo "  Míralas con: docker exec $BROKER mosquitto_sub -h localhost -t 'dev-status/kundt/#' -v"
sleep 120

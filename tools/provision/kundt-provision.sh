#!/usr/bin/env bash
#
# kundt-provision.sh - Da identidad a una placa sin recompilar el firmware.
#
# POR QUE EXISTE
#
# kundt_config lee su configuracion de NVS y solo recurre a los valores de
# Kconfig cuando la clave no esta. Es decir: NVS gana. Eso permite compilar UN
# binario por modulo y darle a cada placa su identidad al flashearla, que era
# justo lo que prometia la decision de "config multi-kit via NVS en runtime"
# y lo unico que le faltaba.
#
# Sin esto, cinco kits obligan a cinco compilaciones distintas de E1, E2 y E3
# solo porque cambia un numero.
#
# QUE ESCRIBE
#
# Genera una particion NVS de 24 KiB con el namespace "kundt" y la graba en
# 0x9000, sin tocar la aplicacion. Tarda menos de un segundo.
#
# POR QUE IMPORTA EL platform_id
#
# El topico MQTT es dev-status/kundt/<platform_id>/<controller_id>. Si los cinco
# kits comparten platform_id, una consigna de frecuencia al kit 1 cambia el tono
# de los cinco y los quince modulos escriben en la misma fila de PostgreSQL.
#
# USO
#
#   ./kundt-provision.sh --kit 2 --port /dev/ttyUSB0
#   ./kundt-provision.sh --kit 3 --port /dev/ttyUSB0 --ssid MiRed --server 10.0.0.5
#
# La red y el servidor NO se versionan aqui: salen de ~/.kundt-provision.conf,
# que queda fuera del repositorio. Ese archivo es un shell con, por ejemplo:
#
#   KUNDT_SSID=MiRed
#   KUNDT_SERVER=10.0.0.5
#
# Se pueden forzar con --ssid y --server, o por variables de entorno.
#
# Por omision platform_id = kit y controller_id = 1: un kit es una plataforma
# reservable en curiousBeagle, y sus tres modulos comparten controlador a
# proposito (ver "Un solo controlador logico" en CLAUDE.md).
#
set -euo pipefail

# Valores locales del laboratorio, fuera del repositorio.
CONF="$HOME/.kundt-provision.conf"
[[ -f "$CONF" ]] && . "$CONF"

SSID="${KUNDT_SSID:-}"
PASS="${KUNDT_PASS:-}"
SERVER="${KUNDT_SERVER:-}"
KIT=""
PLATFORM=""
CONTROLLER=1
PORT="/dev/ttyUSB0"
BAUD=115200
DRY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --kit)        KIT="$2"; shift 2 ;;
    --platform)   PLATFORM="$2"; shift 2 ;;
    --controller) CONTROLLER="$2"; shift 2 ;;
    --ssid)       SSID="$2"; shift 2 ;;
    --pass)       PASS="$2"; shift 2 ;;
    --server)     SERVER="$2"; shift 2 ;;
    --ip)         STATIC_IP="$2"; shift 2 ;;
    --mask)       NETMASK="$2"; shift 2 ;;
    --gw)         GATEWAY="$2"; shift 2 ;;
    --port)       PORT="$2"; shift 2 ;;
    --baud)       BAUD="$2"; shift 2 ;;
    --dry-run)    DRY=1; shift ;;
    -h|--help)    sed -n '2,40p' "$0"; exit 0 ;;
    *) echo "opcion desconocida: $1" >&2; exit 1 ;;
  esac
done

[[ -z "$KIT" ]] && { echo "falta --kit (1..5)" >&2; exit 1; }
[[ -z "$SSID" ]] && { echo "falta --ssid, o KUNDT_SSID en ~/.kundt-provision.conf" >&2; exit 1; }
[[ -z "$SERVER" ]] && { echo "falta --server, o KUNDT_SERVER en ~/.kundt-provision.conf" >&2; exit 1; }
[[ "$KIT" =~ ^[1-5]$ ]] || { echo "el kit debe estar entre 1 y 5" >&2; exit 1; }
PLATFORM="${PLATFORM:-$KIT}"

# La contrasena no se pasa por la linea de ordenes si se puede evitar: quedaria
# en el historial del shell y en la lista de procesos.
if [[ -z "$PASS" ]]; then
  if [[ -f "$HOME/.kundt-wifi-pass" ]]; then
    PASS="$(cat "$HOME/.kundt-wifi-pass")"
  else
    read -rsp "contrasena de \"$SSID\": " PASS; echo
  fi
fi

[[ -n "${IDF_PATH:-}" ]] || { echo "primero: source ~/esp/esp-idf/export.sh" >&2; exit 1; }

CSV="$(mktemp /tmp/kundt-nvs-XXXX.csv)"
BIN="$(mktemp /tmp/kundt-nvs-XXXX.bin)"
trap 'rm -f "$CSV" "$BIN"' EXIT

cat > "$CSV" <<EOF
key,type,encoding,value
kundt,namespace,,
ssid,data,string,$SSID
pass,data,string,$PASS
srv_ip,data,string,$SERVER
kit,data,u8,$KIT
plat,data,u8,$PLATFORM
ctrl,data,u8,$CONTROLLER
EOF

# Direccion fija: solo se escriben las claves si se pidio --ip. Sin ellas el
# firmware usa DHCP, que es lo que hacen todas las placas ya desplegadas.
if [[ -n "${STATIC_IP:-}" ]]; then
    cat >> "$CSV" <<EOF
ip,data,string,$STATIC_IP
mask,data,string,${NETMASK:-255.255.255.0}
gw,data,string,${GATEWAY:-10.31.204.1}
EOF
    echo "  IP fija: $STATIC_IP  mascara ${NETMASK:-255.255.255.0}  puerta ${GATEWAY:-10.31.204.1}"
fi

echo "  kit=$KIT  platform_id=$PLATFORM  controller_id=$CONTROLLER"
echo "  ssid=$SSID  servidor=$SERVER"
echo "  topico MQTT: dev-status/kundt/$PLATFORM/$CONTROLLER"
echo

python "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" \
       generate "$CSV" "$BIN" 0x6000 > /dev/null
echo "  particion NVS generada ($(stat -c%s "$BIN") bytes)"

if [[ "$DRY" == "1" ]]; then
  echo "  --dry-run: no se graba nada"
  exit 0
fi

# 0x9000 es el offset de la particion nvs en la tabla por defecto del proyecto.
python -m esptool --port "$PORT" --baud "$BAUD" write_flash 0x9000 "$BIN"
echo
echo "  listo. Reinicia la placa y comprueba en el log:"
echo "    kundt_config: curiousBeagle: platform=$PLATFORM controller=$CONTROLLER"

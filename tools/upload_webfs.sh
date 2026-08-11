#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  tools/upload_webfs.sh [serial-port]

Examples:
  tools/upload_webfs.sh
  tools/upload_webfs.sh /dev/cu.usbserial-0001
EOF
}

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root_dir"

if [[ $# -gt 1 ]]; then
  usage
  exit 2
fi

port="${1:-}"
image="${WEBFS_IMAGE:-.build/webfs/spiffs.bin}"
offset_file="$(dirname "$image")/spiffs.offset"
esptool="${ESPTOOL:-.arduino15/packages/esp32/tools/esptool_py/3.0.0/esptool}"
baud="${BAUD:-921600}"

if [[ -z "$port" ]]; then
  shopt -s nullglob
  candidates=(
    /dev/cu.usbserial*
    /dev/cu.SLAB_USBtoUART*
    /dev/cu.wchusbserial*
    /dev/cu.usbmodem*
    /dev/cu.usb*
  )
  shopt -u nullglob

  if [[ ${#candidates[@]} -eq 1 ]]; then
    port="${candidates[0]}"
  else
    echo "Could not auto-select an ESP32 serial port." >&2
    echo "Pass the port explicitly, for example:" >&2
    echo "  tools/upload_webfs.sh /dev/cu.usbserial-0001" >&2
    exit 2
  fi
fi

tools/build_webfs.sh "$image"

if [[ ! -x "$esptool" ]]; then
  echo "Missing esptool: $esptool" >&2
  exit 1
fi

if [[ ! -f "$offset_file" ]]; then
  echo "Missing SPIFFS offset metadata: $offset_file" >&2
  exit 1
fi

offset="$(cat "$offset_file")"

echo "Uploading web filesystem to $port at $offset"
"$esptool" --chip esp32 --port "$port" --baud "$baud" --before default_reset --after hard_reset write_flash -z "$offset" "$image"

echo "Done. Refresh http://192.168.4.1 after the ESP32 resets."

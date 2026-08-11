#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  tools/monitor_node.sh [serial-port]

Example:
  tools/monitor_node.sh /dev/cu.usbserial-0001
EOF
}

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root_dir"

if [[ $# -gt 1 ]]; then
  usage
  exit 2
fi

cli="$root_dir/.tools/arduino-cli"
config="$root_dir/arduino-cli.yaml"
port="${1:-}"

if [[ ! -x "$cli" ]]; then
  echo "Missing local Arduino CLI at .tools/arduino-cli" >&2
  exit 1
fi

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
    echo "Visible Arduino ports:" >&2
    "$cli" --config-file "$config" board list >&2 || true
    echo >&2
    echo "Pass the port explicitly, for example:" >&2
    echo "  tools/monitor_node.sh /dev/cu.usbserial-0001" >&2
    exit 2
  fi
fi

"$cli" --config-file "$config" monitor -p "$port" --config baudrate=115200

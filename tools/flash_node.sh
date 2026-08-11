#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  tools/flash_node.sh <1|2|3|A|B|C> [serial-port]

Examples:
  tools/flash_node.sh A
  tools/flash_node.sh B /dev/cu.usbserial-0001
  RADIO_BACKEND=SPI_LORA tools/flash_node.sh C /dev/cu.wchusbserial1230
  FQBN=esp32:esp32:ttgo-lora32-v1 tools/flash_node.sh C /dev/cu.wchusbserial1230
EOF
}

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root_dir"

if [[ $# -lt 1 || $# -gt 2 ]]; then
  usage
  exit 2
fi

case "$1" in
  1|A|a) preset=1; label=A ;;
  2|B|b) preset=2; label=B ;;
  3|C|c) preset=3; label=C ;;
  *) usage; exit 2 ;;
esac

cli="$root_dir/.tools/arduino-cli"
config="$root_dir/arduino-cli.yaml"
sketch="ESP32LoRaChat"
fqbn="${FQBN:-esp32:esp32:esp32}"
build_path=".build/ESP32LoRaChat-preset${preset}"
port="${2:-}"
backend="${RADIO_BACKEND:-AS32_UART}"

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
    echo "  tools/flash_node.sh ${label} /dev/cu.usbserial-0001" >&2
    exit 2
  fi
fi

compile_args=(
  --config-file "$config"
  compile
  --fqbn "$fqbn"
  --build-path "$build_path"
)

case "$backend" in
  AS32|AS32_UART|as32|as32_uart)
    backend_flag="-DRADIO_BACKEND=RADIO_BACKEND_AS32_UART"
    backend_label="AS32 UART"
    ;;
  SPI|SPI_LORA|spi|spi_lora)
    backend_flag="-DRADIO_BACKEND=RADIO_BACKEND_SPI_LORA"
    backend_label="SPI LoRa"
    ;;
  *)
    echo "Unknown RADIO_BACKEND: $backend" >&2
    echo "Use AS32_UART or SPI_LORA." >&2
    exit 2
    ;;
esac

compile_args+=(--build-property "compiler.cpp.extra_flags=-DNODE_PRESET=${preset} ${backend_flag}")

echo "Compiling node ${label} with ${fqbn} (${backend_label})"
"$cli" "${compile_args[@]}" "$sketch"

echo "Uploading node ${label} to ${port}"
"$cli" --config-file "$config" upload -p "$port" --fqbn "$fqbn" --build-path "$build_path" "$sketch"

echo "Done. Node ${label} AP should come up after reset."
echo "If this board has no web UI yet, run: tools/upload_webfs.sh ${port}"

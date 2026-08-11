#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root_dir"

data_dir="${DATA_DIR:-ESP32LoRaChat/data}"
partition_csv="${PARTITIONS_CSV:-.arduino15/packages/esp32/hardware/esp32/1.0.6/tools/partitions/default.csv}"
out_file="${1:-.build/webfs/spiffs.bin}"
mkspiffs="${MKSPIFFS:-.arduino15/packages/esp32/tools/mkspiffs/0.2.3/mkspiffs}"

if [[ ! -d "$data_dir" ]]; then
  echo "Missing data directory: $data_dir" >&2
  exit 1
fi

if [[ ! -f "$partition_csv" ]]; then
  echo "Missing partition CSV: $partition_csv" >&2
  exit 1
fi

if [[ ! -x "$mkspiffs" ]]; then
  echo "Missing mkspiffs tool: $mkspiffs" >&2
  exit 1
fi

read -r spiffs_offset spiffs_size < <(
  awk -F, '
    $1 !~ /^[[:space:]]*#/ {
      name=$1; subtype=$3; offset=$4; size=$5;
      gsub(/[[:space:]]/, "", name);
      gsub(/[[:space:]]/, "", subtype);
      gsub(/[[:space:]]/, "", offset);
      gsub(/[[:space:]]/, "", size);
      if (tolower(name) == "spiffs" || tolower(subtype) == "spiffs") {
        print offset, size;
        exit;
      }
    }
  ' "$partition_csv"
)

if [[ -z "${spiffs_offset:-}" || -z "${spiffs_size:-}" ]]; then
  echo "No SPIFFS partition found in $partition_csv" >&2
  exit 1
fi

mkdir -p "$(dirname "$out_file")"
size_dec=$((spiffs_size))

"$mkspiffs" -c "$data_dir" -p 256 -b 4096 -s "$size_dec" "$out_file"

printf '%s\n' "$spiffs_offset" > "$(dirname "$out_file")/spiffs.offset"
printf '%s\n' "$spiffs_size" > "$(dirname "$out_file")/spiffs.size"

echo "Built $out_file"
echo "SPIFFS offset: $spiffs_offset"
echo "SPIFFS size:   $spiffs_size"

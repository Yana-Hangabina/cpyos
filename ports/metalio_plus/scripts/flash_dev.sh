#!/usr/bin/env bash
# Flash the development Plus image the complete way: app to both OTA slots and
# the data partition (language pack, emoji, reading core). An app-only flash
# leaves the old data layout behind, so the device shows missing-glyph prompts
# and an English-only UI until the data partition is rewritten.
#
#   bash ports/metalio_plus/scripts/flash_dev.sh [--port /dev/cu.usbmodemXXXX]
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
repo="$(cd "$here/../.." && pwd)"
image="$here/build_shell/cpyos_metalio_plus.bin"
if [ ! -f "$image" ]; then
  echo "error: $image is missing; build it first" >&2
  exit 1
fi
port=""
if [ "${1:-}" = "--port" ]; then port="${2:?}"; fi
python3 "$repo/scripts/generate_data_manifest.py" \
  --partitions ports/metalio_plus/partitions_plus_ota.csv \
  --out build/data_manifest_plus.txt
cmd=(python3 "$here/scripts/flash_metalio_plus.py" --layout port-ota --slot both --allow-versioned
     --firmware "$image")
if [ -n "$port" ]; then cmd+=(--port "$port"); fi
# One 16 MiB backup is the restore point for the whole chip; reuse it on later
# flashes so daily iteration does not pay a full read every time.
backup="${CPYOS_PLUS_BACKUP:-$(cd "$repo/.." && pwd)/device/plus_full_backup.bin}"
mkdir -p "$(dirname "$backup")"
if [ -f "$backup" ]; then cmd+=(--reuse-backup "$backup"); else cmd+=(--backup "$backup"); fi
"${cmd[@]}"

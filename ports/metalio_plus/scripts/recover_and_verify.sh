#!/bin/sh
# Recover a Metalio E-INK4 Plus whose USB-Serial/JTAG stopped answering and
# flash the current tree, then report what still needs a hardware pass.
#
# Why a power cycle is required: opening the console resets the chip through
# USB-Serial/JTAG (rst:0x17). A reset that lands while the SD card is mounted can
# leave the card wedged (INVALID_CRC at every profile) and the USB endpoint
# unresponsive; neither sdremount nor a reflash clears it. Only a real PMU
# off/on does.
#
# Usage:
#   bash ports/metalio_plus/scripts/recover_and_verify.sh [--no-flash]
#
# Before running: hold POWER ~10 s to force the PMU off, wait 2 s, press POWER
# once. The script waits up to 3 minutes for the USB-Serial/JTAG port and
# esptool sync.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PORT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
WORKSPACE=$(CDPATH= cd -- "$PORT_DIR/../../../.." && pwd)
FW="$PORT_DIR/build/cpyos_metalio_plus.bin"
BACKUP=${PLUS_BACKUP:-$WORKSPACE/artifacts/plus_backup_20261114.bin}
PY=${IDF_PYTHON:-$HOME/.espressif/python_env/idf6.1_py3.12_env/bin/python}
NO_FLASH=0
[ "${1:-}" = "--no-flash" ] && NO_FLASH=1

say() { printf '%s\n' "$*"; }

[ -x "$PY" ] || { say "missing IDF python: $PY (set IDF_PYTHON)"; exit 1; }

say "== waiting for the USB-Serial/JTAG port =="
say "If the unit is wedged: hold POWER ~10 s (PMU off), wait 2 s, press POWER."
deadline=$(( $(date +%s) + 180 ))
port=""
while [ "$(date +%s)" -lt "$deadline" ]; do
  port=$(ls /dev/cu.usbmodem* 2>/dev/null | tail -1 || true)
  if [ -n "$port" ]; then
    if "$PY" -m esptool --chip esp32s31 --port "$port" --connect-attempts 1 chip-id >/dev/null 2>&1; then
      break
    fi
  fi
  port=""
  sleep 4
done
[ -n "$port" ] || { say "no synced port after 180 s; power-cycle the unit and retry"; exit 2; }
say "== port $port answers esptool =="

if [ "$NO_FLASH" -eq 0 ]; then
  [ -f "$FW" ] || { say "missing $FW (build first)"; exit 1; }
  [ -f "$BACKUP" ] || { say "missing backup $BACKUP (set PLUS_BACKUP)"; exit 1; }
  say "== full install: table + app0/app1 + data packs =="
  "$PY" "$SCRIPT_DIR/flash_metalio_plus.py" --layout port-ota --full-install \
    --allow-versioned --reuse-backup "$BACKUP" --firmware "$FW"
  say "== flashed; waiting for the app =="
  sleep 12
fi

say ""
say "Next hardware checks (run one at a time; each opens the console once):"
say "  $PY $WORKSPACE/exp/plus_hw_check.py"
say "  $PY $WORKSPACE/exp/plus_formats_recheck.py"
say "  bash  $WORKSPACE/exp/plus_verify_all.sh"
say ""
say "Console self-checks before the hardware passes:"
say "  sdpark    open a file, park every handle, restore, verify the stream continues"
say "  audio     mode 1 state; audio tone 400; audio mic 400 (state=2, rms>0)"
say "  haptic 50 motor polarity/feel at 35/50/70 ms levels"
say ""
say "Still unverified on this unit:"
say "  - CFF page render (embed an .otf EPUB or select an .otf SD family)"
say "  - camera still capture + first FULL after the LCD_CAM hand-off"
say "  - BT audio pairing with a headset, voice memo mic/level"
say "  - AI voice with a real xiaozhi URL/token (/.crosspoint/voice.json)"
say "  - light sleep from standby over Home AND over an open book (power key + timer wake)"
say "  - 4G (needs a modem), including a BT reset while 4G is up (cell-recover re-attach)"
say "  - BLE pager, tilt/pinch, USB MSC eject"
say "  - battery-only shutdown / 3.1 V cutoff, VCOM measurement"

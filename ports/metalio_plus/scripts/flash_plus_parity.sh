#!/usr/bin/env bash
# Bring a Plus up to Read-Pico parity in one pass:
#   app0 + app1, the six data-partition blobs, the PDF engine, and the
#   managed-engine install (so the update flow has a target to replace).
#
# Prerequisites: exported ESP-IDF 6.1 (`export.sh`), a built app
# (build_shell/cpyos_metalio_plus.bin), build/pdfmod-rv.bin paired with that
# exact app (re-run scripts/build_pdf_artifacts.sh after every app rebuild) and
# build/data_manifest_plus.txt (scripts/generate_data_manifest.py
# --partitions ports/metalio_plus/partitions_plus_ota.csv).
#
#   bash ports/metalio_plus/scripts/flash_plus_parity.sh [--skip-data] [--port P]
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
repo="$(cd "$here/../.." && pwd)"
port=""
skip_data=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --port) port="$2"; shift 2 ;;
    --skip-data) skip_data=1; shift ;;
    -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
if [[ -z "${IDF_PATH:-}" ]]; then
  echo "IDF_PATH is not set; export ESP-IDF 6.1 first" >&2
  exit 2
fi
py="${IDF_PYTHON_ENV_PATH:-$HOME/.espressif/python_env/idf6.1_py3.12_env}/bin/python"
app="$here/build_shell/cpyos_metalio_plus.bin"
engine="$repo/build/pdfmod-rv.bin"
manifest="$repo/build/data_manifest_plus.txt"
for f in "$app" "$engine" "$manifest"; do
  [[ -f "$f" ]] || { echo "missing $f" >&2; exit 2; }
done
size=$(stat -f%z "$app" 2>/dev/null || stat -c%s "$app")
if (( size > 0x660000 )); then
  echo "app is $size B; the slot is 0x660000 (6692864) B" >&2
  exit 2
fi
[[ -n "$port" ]] || port=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
[[ -n "$port" ]] || { echo "no USB serial port found" >&2; exit 2; }

echo "== app0 + app1 ($size B) on $port"
"$py" -m esptool --chip esp32s31 -p "$port" -b 921600 \
  write_flash 0x10000 "$app" 0x670000 "$app"

if (( skip_data == 0 )); then
  echo "== data partition"
  "$py" "$here/scripts/flash_plus_data.py" --port "$port"
else
  echo "== data partition skipped"
fi

echo "== waiting for the shell"
sleep 10
echo "== uploading the engine to /pdfmod.bin"
python3 "$here/scripts/putfile_fast.py" --local "$engine" --remote /pdfmod.bin

echo "== installing the managed engine (update-flow target)"
"$py" - "$port" <<'PY'
import sys, time, serial
port = sys.argv[1]
d = serial.Serial()
d.port = port
d.baudrate = 115200
d.timeout = 0.3
d.open()  # do not touch DTR/RTS: that resets the SoC
time.sleep(0.6)
d.write(b"pdfengineinstall /pdfmod.bin\n")
deadline = time.time() + 25
buf = b""
while time.time() < deadline:
    chunk = d.read(4096)
    if chunk:
        buf += chunk
        if b"pdfengineinstall:" in buf:
            break
d.close()
text = buf.decode("utf-8", "replace")
for line in text.splitlines():
    if "pdfengineinstall:" in line or "PDFM" in line:
        print(line)
status = [l for l in text.splitlines() if "pdfengineinstall:" in l]
if not status or not status[-1].rstrip().endswith("0"):
    raise SystemExit("engine install did not report 0; device left as-is")
PY

echo "== done. Expect: [CORE] carrier present, [SDFS] CPYOSCJKCore, [PDFM] engine installed"

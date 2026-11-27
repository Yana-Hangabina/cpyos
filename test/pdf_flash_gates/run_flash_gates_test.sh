#!/usr/bin/env bash
# Flash-gate host regression, with the mutation step this repo requires:
# weakening a gate must turn the suite red, otherwise the gate is decoration.
set -euo pipefail
cd "$(dirname "$0")/../.."

PYTHON="${PYTHON:-$HOME/.platformio/penv/bin/python}"
[ -x "$PYTHON" ] || PYTHON=python3

echo "== pdf flash gates: normal =="
"$PYTHON" test/pdf_flash_gates/classify_boot_test.py
"$PYTHON" test/pdf_flash_gates/partition_table_test.py

echo
echo "== mutation (reset-loop threshold raised out of reach) =="
MUT="$(mktemp -d)"; trap 'rm -rf "$MUT"' EXIT
mkdir -p "$MUT/scripts" "$MUT/test/pdf_flash_gates"
cp scripts/flash_pdf_xip.py "$MUT/scripts/"
cp test/pdf_flash_gates/classify_boot_test.py "$MUT/test/pdf_flash_gates/"
if ! sed -i.bak 's/>= 3:/>= 99:/' "$MUT/scripts/flash_pdf_xip.py"; then
  echo "SKIP: sed -i unavailable on this platform"
  exit 0
fi
# The copy must really differ, or a silent sed makes the "detection" meaningless.
cmp -s "$MUT/scripts/flash_pdf_xip.py" scripts/flash_pdf_xip.py && {
  echo "mutation did not apply" >&2; exit 1; }
if "$PYTHON" "$MUT/test/pdf_flash_gates/classify_boot_test.py" >/dev/null 2>&1; then
  echo "MUTATION SURVIVED: the loop gate is not actually tested" >&2
  exit 1
fi
echo "mutation detected"

echo
echo
echo "== mutation (layout-shift gate neutered: always says 'no shift') =="
MUT2="$(mktemp -d)"
mkdir -p "$MUT2/scripts" "$MUT2/test/pdf_flash_gates" "$MUT2/.pio/build/readpico_pdf"
cp scripts/flash_pdf_xip.py "$MUT2/scripts/"
cp partitions_readpico_xip.csv "$MUT2/"
cp partitions_readpico.csv "$MUT2/" 2>/dev/null || true
cp .pio/build/readpico_pdf/partitions.bin "$MUT2/.pio/build/readpico_pdf/" 2>/dev/null || true
cp test/pdf_flash_gates/partition_table_test.py "$MUT2/test/pdf_flash_gates/"
if ! sed -i.bak 's/^    here, there = device.get("spiffs"), target.get("spiffs")$/    here, there = device.get("spiffs"), device.get("spiffs")/' \
    "$MUT2/scripts/flash_pdf_xip.py"; then
  echo "SKIP: sed -i unavailable on this platform"
else
  if cmp -s "$MUT2/scripts/flash_pdf_xip.py" scripts/flash_pdf_xip.py; then
    echo "FAIL: mutation did not apply (layout gate)" >&2
    exit 1
  fi
  if (cd "$MUT2" && "$PYTHON" test/pdf_flash_gates/partition_table_test.py >/dev/null 2>&1); then
    echo "FAIL: mutation was not detected -- a moved data partition would pass unnoticed" >&2
    exit 1
  fi
  echo "mutation detected"
fi
rm -rf "$MUT2"

echo "flash gate host regression: PASS"

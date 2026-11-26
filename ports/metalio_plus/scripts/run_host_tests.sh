#!/usr/bin/env bash
# Run every host-side regression for the Metalio E-INK4 Plus port.
#
#   bash ports/metalio_plus/scripts/run_host_tests.sh
#
# Optional: KIIKOREAD_RC110_DIR=/path/to/read-pico-reader also runs the rc110
# inventory (the license test uses it automatically when it exists).
set -euo pipefail

root="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$root"

echo "== license scope / rc110 copy checks"
python3 scripts/tests/test_license_scope.py

echo "== packaging and flash planner"
python3 -m unittest ports.metalio_plus.test.test_package_metalio_plus \
                   ports.metalio_plus.test.test_flash_metalio_plus \
                   ports.metalio_plus.test.test_verify_bundle \
                   ports.metalio_plus.test.test_verify_features \
                   ports.metalio_plus.test.test_console_docs \
                   ports.metalio_plus.test.test_vendor_sleep_docs \
                   ports.metalio_plus.test.test_vendor_app_inventory \
                   ports.metalio_plus.test.test_port_jjwxc_wiring \
                   ports.metalio_plus.test.test_transfer_page \
                   ports.metalio_plus.test.test_display_modes \
                   ports.metalio_plus.test.test_camera_table \
                   ports.metalio_plus.test.test_standby_power \
                   ports.metalio_plus.test.test_touch_input_wiring \
                   ports.metalio_plus.test.test_web_pages \
                   ports.metalio_plus.test.test_web_fonts \
                   ports.metalio_plus.test.test_docs_artifacts \
                   ports.metalio_plus.test.test_analyze_power_log \
                   ports.metalio_plus.test.test_reference_inventory \
                   ports.metalio_plus.test.test_pdf_port \
                   ports.metalio_plus.test.test_flash_plus_data \
                   ports.metalio_plus.test.test_data_layout

bundle="$root/../../artifacts/cpyos-metalio-plus-0.0.0-untested.zip"
# Only verify a bundle produced from this tree. An older bundle (the 2026-11-09
# bring-up zip) describes the single-slot layout and would fail the current
# port-OTA notice checks for a reason that is not a regression.
if [[ -f "$bundle" ]]; then
  # The NOTICE inside a bundle records the data-partition offsets it was built
  # for.  The 2026-11-09 bring-up zip predates the port-OTA layout, so its
  # notice names the single-slot spiffs base; verifying it against today's
  # checks would report a layout mismatch that is not a regression.
  if unzip -p "$bundle" 'UNTESTED-ON-HARDWARE.txt' 2>/dev/null | grep -q '0xCD0000'; then
    python3 ports/metalio_plus/scripts/verify_metalio_bundle.py --bundle "$bundle"
  else
    echo "note: skipping legacy bundle (pre-port-OTA data offsets)"
  fi
fi

if [[ -f ports/metalio_plus/build/cpyos_metalio_plus.elf \
   || -d "$root/../../artifacts" ]]; then
  echo "== feature presence in the built images"
  ALLOW_PRIVATE=""
  if [ -f ports/metalio_plus/build/cpyos_metalio_plus.elf ] && \
     grep -aq "JjwxcActivity" ports/metalio_plus/build/cpyos_metalio_plus.elf; then
    echo "note: dev shell links the private component; public builds must not (CI will fail without --allow-private)"
    ALLOW_PRIVATE="--allow-private"
  fi
  # Check the tree's current build, not the historical 2026-11-09 artifacts.
  FEATURE_ARTIFACTS=$(mktemp -d "${TMPDIR:-/tmp}/plus-features.XXXXXX")
  python3 ports/metalio_plus/scripts/verify_port_features.py --artifacts "$FEATURE_ARTIFACTS" \
    --bin ports/metalio_plus/build/cpyos_metalio_plus.bin \
    --elf ports/metalio_plus/build/cpyos_metalio_plus.elf $ALLOW_PRIVATE
  rm -rf "$FEATURE_ARTIFACTS"
fi

echo "== voice-memo WAV header (ASan/UBSan)"
bash ports/metalio_plus/test/run_wav_header_test.sh

echo "== byte-mode QR version table (ASan/UBSan)"
bash ports/metalio_plus/test/run_qr_version_test.sh

echo "== xiaozhi voice framing (ASan/UBSan)"
bash ports/metalio_plus/test/run_voice_frames_test.sh

echo "== BH1098 inquiry parser (ASan/UBSan)"
bash ports/metalio_plus/test/run_bt_audio_parse_test.sh

echo "== AI/BOOT key page routing (ASan/UBSan)"
bash ports/metalio_plus/test/run_ai_key_routing_test.sh

echo "== AI/BOOT key short/long sampler (ASan/UBSan)"
bash ports/metalio_plus/test/run_ai_key_sampler_test.sh

echo "== BLE HID report decoding (ASan/UBSan)"
bash ports/metalio_plus/test/run_ble_hid_decode_test.sh

echo "== HTTP transfer path safety (ASan/UBSan)"
bash ports/metalio_plus/test/run_transfer_paths_test.sh

echo "== WebSocket upload START parser (ASan/UBSan)"
bash ports/metalio_plus/test/run_ws_protocol_test.sh

echo "== multipart /upload parser (ASan/UBSan)"
bash ports/metalio_plus/test/run_multipart_test.sh

echo "== browser plugin job table (ASan/UBSan)"
bash ports/metalio_plus/test/run_plugin_jobs_test.sh

echo "== form/JSON helpers for the file-management API (ASan/UBSan)"
bash ports/metalio_plus/test/run_form_fields_test.sh

echo "== 1-bit framebuffer BMP encoder (ASan/UBSan)"
bash ports/metalio_plus/test/run_bmp_export_test.sh

echo "== WebDAV formatting helpers (ASan/UBSan)"
bash ports/metalio_plus/test/run_webdav_format_test.sh

echo "== portrait touch mapping (ASan/UBSan)"
bash ports/metalio_plus/test/run_touch_map_test.sh

echo "== font name validation (ASan/UBSan)"
bash ports/metalio_plus/test/run_font_store_test.sh

echo "== CloudZao installed-resource record (ASan/UBSan)"
bash ports/metalio_plus/test/run_plus_resources_record_test.sh

echo "== two-finger pinch classification (ASan/UBSan)"
bash ports/metalio_plus/test/run_touch_pair_test.sh

if [[ -n "${KIIKOREAD_RC110_DIR:-}" ]]; then
  echo "== rc110 inventory"
  python3 ports/metalio_plus/scripts/inventory_kiikoread.py \
      --tree "$KIIKOREAD_RC110_DIR" --json >/dev/null
  echo "inventory ok"
fi

echo "all host tests passed"

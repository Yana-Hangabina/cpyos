#!/usr/bin/env bash
# Build all four Metalio E-INK4 Plus variants and copy them out with the
# UNTESTED marker. Requires an ESP-IDF 6.1 export (IDF_PATH + --preview).
#
#   source $IDF_PATH/export.sh
#   bash ports/metalio_plus/scripts/build_all.sh [output-dir]
#
# Every image is stamped `version 0.0.0-untested` in esp_app_desc. Nothing here
# has run on hardware; do not publish these as a stable release.
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
out="${1:-$here/../../../../artifacts}"
stamp="$(date +%Y%m%d)"
variants=(
  "bsp:"
  "bringup:CPYOS_UI_PROBE=1"
  "reader:CPYOS_READER_DEMO=1"
  "shell:CPYOS_SHELL_DEMO=1 CPYOS_ACTIVITY_PROBE=1 CPYOS_UI_PROBE=1"
)

mkdir -p "$out"
cd "$here"
for entry in "${variants[@]}"; do
  name="${entry%%:*}"
  envs="${entry#*:}"
  echo "== $name ($envs)"
  # shellcheck disable=SC2086
  env $envs idf.py --preview -DPROJECT_VER=0.0.0-untested reconfigure build >"/tmp/metalio_${name}.log" 2>&1 || {
    echo "FAILED: see /tmp/metalio_${name}.log" >&2
    exit 1
  }
  dest="$out/cpyos_metalio_plus_${name}_${stamp}_untested.bin"
  cp build/cpyos_metalio_plus.bin "$dest"
  python3 - "$dest" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
off = 0x20
if len(data) < off + 0x80 or struct.unpack_from('<I', data, off)[0] != 0xABCD5432:
    sys.exit("no esp_app_desc in " + sys.argv[1])
ver = data[off+0x10:off+0x30].split(b'\0')[0].decode()
if 'untested' not in ver:
    sys.exit("version is %r, expected an -untested label" % ver)
print("   %d bytes, version=%s" % (len(data), ver))
PY
  shasum -a 256 "$dest"
done


# Offline input/hyphenation packs. The bundler reads them from the repo build/;
# copy them next to the images so a bring-up session can flash them by hand.
for extra in pinyin hyphen; do
  src="$here/../../build/${extra}.dat"
  if [[ -f "$src" ]]; then
    dest="$out/cpyos_metalio_plus_${extra}_${stamp}_untested.dat"
    cp "$src" "$dest"
    echo "== ${extra}"
    ls -l "$dest"
    shasum -a 256 "$dest"
  else
    echo "WARNING: $src missing; run scripts/generate_${extra}_db.py" >&2
  fi
done
# Language pack for the 33 non-embedded languages (spiffs partition).
pack="$here/build/i18n_pack.dat"
if [[ -f "$pack" ]]; then
  dest="$out/cpyos_metalio_plus_i18n_pack_${stamp}_untested.dat"
  cp "$pack" "$dest"
  echo "== i18n pack"
  ls -l "$dest"
  shasum -a 256 "$dest"
else
  echo "WARNING: build/i18n_pack.dat missing" >&2
fi

# Bundled emoji face and the compressed reading core (spiffs).
root="$(cd "$here/../.." && pwd)"
python3 "$root/scripts/generate_emoji_pack.py" >/dev/null
python3 "$root/scripts/build_ui_font_subset.py" --core-only >/dev/null
core="$root/build/cjk_core.dat"
dest="$out/cpyos_metalio_plus_core_${stamp}_untested.dat"
cp "$core" "$dest"
echo "== reading core"
ls -l "$dest"
shasum -a 256 "$dest"
dest="$out/cpyos_metalio_plus_emoji_pack_${stamp}_untested.dat"
cp "$root/build/emoji.dat" "$dest"
echo "== emoji pack"
ls -l "$dest"
shasum -a 256 "$dest"

#!/usr/bin/env bash
# Build the Plus PDF engine (RISC-V MuPDF module) and its update manifest.
#
# Prerequisites: an exported ESP-IDF 6.1, and an app built with CPYOS_PDF=1 so
# that build_shell/cpyos_metalio_plus.elf, compile_commands.json and
# specs/picolibc.specs exist.  MuPDF 1.26.3 source with thirdparty is downloaded
# on first use (proxy-aware: run through `gg` if the direct fetch fails).
#
#   bash ports/metalio_plus/scripts/build_pdf_artifacts.sh \
#       [--build build_shell] [--mupdf-src DIR] [--manifest-url URL] \
#       [--with-assets] [--jobs N]
#
# Outputs:
#   build/pdfmod-rv.bin            engine module (appTag = sha256(app ELF))
#   build/pdf-engine-manifest.txt  only when --manifest-url is given
#   build/pdf_full_assets.bin      only with --with-assets (font pack + CMaps)
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
repo="$(cd "$here/../.." && pwd)"
build="build_shell"
mupdf_src=""
manifest_url=""
with_assets=0
jobs="${JOBS:-8}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build) build="$2"; shift 2 ;;
    --mupdf-src) mupdf_src="$2"; shift 2 ;;
    --manifest-url) manifest_url="$2"; shift 2 ;;
    --with-assets) with_assets=1; shift ;;
    --jobs) jobs="$2"; shift 2 ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "IDF_PATH is not set; export ESP-IDF 6.1 first" >&2
  exit 2
fi
elf="$here/$build/cpyos_metalio_plus.elf"
compile_db="$here/$build/compile_commands.json"
specs="$here/$build/specs/picolibc.specs"
for f in "$elf" "$compile_db" "$specs"; do
  [[ -f "$f" ]] || { echo "missing $f (build the app with CPYOS_PDF=1 first)" >&2; exit 2; }
done

tc="${PDF_MODULE_TOOLCHAIN:-$HOME/.espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin}"
arch="${PDF_MODULE_ARCH:--mabi=ilp32f -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv -mtune=esp-base}"

if [[ -z "$mupdf_src" ]]; then
  mupdf_src="$repo/.scratch/mupdf-rv/mupdf-1.26.3-source"
fi
if [[ ! -d "$mupdf_src" ]]; then
  echo "MuPDF source not found at $mupdf_src"
  echo "download https://mupdf.com/downloads/archive/mupdf-1.26.3-source.tar.gz and"
  echo "extract it there, then apply patches/mupdf/source overrides"
  exit 2
fi
# The two project overrides are required (external CMaps + image changes).
for f in source/pdf/pdf-cmap-load.c source/fitz/image.c; do
  if ! cmp -s "$repo/patches/mupdf/$f" "$mupdf_src/$f"; then
    echo "applying patches/mupdf/$f over $mupdf_src/$f"
    cp "$repo/patches/mupdf/$f" "$mupdf_src/$f"
  fi
done

libs="$mupdf_src/build/rv32_nocjk"
if [[ ! -f "$libs/libmupdf.a" || ! -f "$libs/libmupdf-third.a" ]]; then
  echo "building MuPDF libs ($jobs jobs) -> $libs"
  make -C "$mupdf_src" -j"$jobs" OUT="$libs" libs \
    CC="$tc/riscv32-esp-elf-gcc" CXX="$tc/riscv32-esp-elf-g++" \
    AR="$tc/riscv32-esp-elf-ar" RANLIB="$tc/riscv32-esp-elf-ranlib" \
    build=release html=no xps=no mujs=no brotli=no extract=no barcode=no tesseract=no \
    XCFLAGS="-Os $arch -msmall-data-limit=0 -specs=$specs -D__PICOLIBC_ERRNO_FUNCTION=__errno -ffunction-sections -fdata-sections -D_GNU_SOURCE -D_DEFAULT_SOURCE -DNO_CJK -DFZ_ENABLE_HYPHEN=0 -DTOFU -DTOFU_CJK -DTOFU_CJK_EXT -DTOFU_BASE14 -Wno-error=incompatible-pointer-types -Wno-incompatible-pointer-types -include $repo/patches/mupdf/timegm_compat.h"
fi

echo "linking module against $elf"
PDF_MODULE_TOOLCHAIN="$tc" PDF_MODULE_TOOL_PREFIX=riscv32-esp-elf- \
PDF_MODULE_STUB_CFLAGS="$arch" PDF_MODULE_LINK_FLAGS="$arch" \
PDF_MODULE_LINK_SPECS="$specs" \
  python3 "$repo/scripts/build_pdf_module.py" \
    --firmware-elf "$elf" --compile-db "$compile_db" --layout sd \
    --mupdf-libs "$libs" --out "$repo/build/pdfmod-rv.bin"

if [[ "$with_assets" == 1 ]]; then
  echo "building the PDF asset bundle (font pack + CMaps)"
  g++ -std=c++20 -O2 -I"$repo/lib/EpdFont" "$repo/tools/generate_font_pack.cpp" \
    -o "$repo/build/generate_font_pack"
  "$repo/build/generate_font_pack" "$repo/build/font_pack.dat"
  rm -rf "$repo/build/cmaps"
  mkdir -p "$repo/build/cmaps"
  cp "$mupdf_src/resources/cmaps/"* "$repo/build/cmaps/"
  python3 "$repo/tools/gen_pdf_assets.py"
fi

if [[ -n "$manifest_url" ]]; then
  python3 "$repo/scripts/build_pdf_engine_manifest.py" \
    --firmware-elf "$elf" --pdfmod "$repo/build/pdfmod-rv.bin" \
    --url "$manifest_url" --out "$repo/build/pdf-engine-manifest.txt"
fi

echo "engine: $repo/build/pdfmod-rv.bin ($(wc -c < "$repo/build/pdfmod-rv.bin") bytes)"
echo "stage it with scripts/stage_pdf_sd.py"

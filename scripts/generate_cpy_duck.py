#!/usr/bin/env python3
"""Rasterize the CPY (Crispy Duck) mascot line art into 1-bit icon bitmaps.

The duck's SVG geometry is the frozen design (docs/plans/2026-10-15-cpyos-ui-
theme-design.md): stroke-3 line art on a 64x64 viewBox, painter's order
(tail, body, wing, beak, head, eye) so the white fills merge the shapes.

Output matches the SDK icon pipeline exactly (rsvg-convert + Pillow, the same
rasterize()/pack() the Lucide chrome icons use), so the generated
freeink::Icon entries drop into renderer.drawIcon()/drawIconInverted().

Usage:
    python3 scripts/generate_cpy_duck.py                # -> src/components/themes/cpy/cpy_duck_assets.h
    python3 scripts/generate_cpy_duck.py --out X.h      # custom output
    python3 scripts/generate_cpy_duck.py --preview      # ASCII-preview every raster
"""

import argparse
import importlib.util
import io
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = ROOT / 'freeink-sdk/libs/assets/Icons/tools/gen_icons.py'
DEFAULT_OUT = ROOT / 'src/components/themes/cpy/cpy_duck_assets.h'

STROKE = "fill='#fff' stroke='#000' stroke-width='3' stroke-linecap='round' stroke-linejoin='round'"

# --- Frozen duck geometry (matches design/preview-1.html <symbol id="d1-*">) ---

STAND = f"""<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>
  <g {STROKE}>
    <path d='M52 36 q7 -2 9 -8 q1 8 -6 11'/>
    <ellipse cx='34' cy='42' rx='20' ry='13'/>
    <path d='M28 54 q0 5 -3 6 M40 54 q0 5 3 6' fill='none'/>
    <path d='M30 42 q8 -4 14 1 q-6 5 -14 3 z'/>
    <ellipse cx='10' cy='20' rx='8' ry='4.5'/>
    <ellipse cx='11' cy='26' rx='6' ry='3'/>
    <circle cx='26' cy='20' r='14'/>
    <path d='M26 6 q1 -4 5 -4' fill='none'/>
  </g>
  <circle cx='22' cy='16' r='2.2' fill='#000'/>
  <path d='M32 27 q3 2 6 1' fill='none' stroke='#000' stroke-width='2' stroke-linecap='round'/>
</svg>"""

SWIM = f"""<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>
  <g {STROKE}>
    <path d='M56 40 q5 -2 6 -7' fill='none'/>
    <path d='M26 46 C28 32 36 28 44 30 C52 32 56 38 58 46 Z'/>
    <path d='M34 40 q6 -3 10 1 q-5 4 -10 2 z'/>
    <ellipse cx='10' cy='22' rx='8' ry='4.5'/>
    <ellipse cx='11' cy='28' rx='6' ry='3'/>
    <circle cx='26' cy='22' r='14'/>
    <path d='M26 8 q1 -4 5 -4' fill='none'/>
  </g>
  <circle cx='22' cy='18' r='2.2' fill='#000'/>
  <path d='M2 46 q6 -4 12 0 t12 0 t12 0 t12 0 t12 0' fill='none' stroke='#000' stroke-width='3' stroke-linecap='round'/>
</svg>"""

SLEEP = SWIM.replace(
    "<circle cx='22' cy='18' r='2.2' fill='#000'/>",
    "<rect x='18' y='14' width='8' height='8' fill='#fff' stroke='none'/>"
    "<path d='M18 18 q4 4 8 0' fill='none' stroke='#000' stroke-width='3' stroke-linecap='round'/>"
    "<path d='M46 10 h7 l-7 7 h7 M55 2 h5 l-5 5 h5' fill='none' stroke='#000' stroke-width='2.5' "
    "stroke-linecap='round' stroke-linejoin='round'/>",
)

# name -> list of (svg, sizes) — the frozen usage set from the design doc.
# Small sizes get simplified geometry: at <=48px the double-lobe beak, the
# closed wing and the wave-to-body gap all fuse into blobs, so they use the
# single-lobe/open-wing variants; 200px+ keeps the full pose.

STAND = f"""<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>
  <g {STROKE}>
    <path d='M52 36 q7 -2 9 -8 q1 8 -6 11'/>
    <ellipse cx='34' cy='42' rx='20' ry='13'/>
    <path d='M28 54 q0 5 -3 6 M40 54 q0 5 3 6' fill='none'/>
    <path d='M30 42 q8 -4 14 1 q-6 5 -14 3 z'/>
    <ellipse cx='10' cy='20' rx='8' ry='4.5'/>
    <ellipse cx='11' cy='26' rx='6' ry='3'/>
    <circle cx='26' cy='20' r='14'/>
    <path d='M26 6 q1 -4 5 -4' fill='none'/>
  </g>
  <circle cx='22' cy='16' r='2.2' fill='#000'/>
  <path d='M32 27 q3 2 6 1' fill='none' stroke='#000' stroke-width='2' stroke-linecap='round'/>
</svg>"""

STAND_SMALL = f"""<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>
  <g {STROKE}>
    <path d='M51 34 q8 -2 10 -8 q1 8 -7 12'/>
    <ellipse cx='36' cy='42' rx='18' ry='13'/>
    <path d='M30 54 q0 4 -4 5 M42 54 q0 4 4 5' fill='none'/>
    <path d='M33 41 q8 -3 13 1' fill='none'/>
    <circle cx='28' cy='20' r='13.5'/>
    <path d='M28 7 q1 -4 5 -4' fill='none'/>
  </g>
  <path d='M15 14.5 Q4 16.5 4 20.5 Q4 24.5 15 26.5 Z' fill='#000' stroke='none'/>
  <circle cx='24' cy='16' r='2.2' fill='#000'/>
</svg>"""

SWIM = f"""<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>
  <g {STROKE}>
    <path d='M56 40 q5 -2 6 -7' fill='none'/>
    <path d='M26 46 C28 32 36 28 44 30 C52 32 56 38 58 46 Z'/>
    <path d='M34 40 q6 -3 10 1 q-5 4 -10 2 z'/>
    <ellipse cx='10' cy='22' rx='8' ry='4.5'/>
    <ellipse cx='11' cy='28' rx='6' ry='3'/>
    <circle cx='26' cy='22' r='14'/>
    <path d='M26 8 q1 -4 5 -4' fill='none'/>
  </g>
  <circle cx='22' cy='18' r='2.2' fill='#000'/>
  <path d='M2 46 q6 -4 12 0 t12 0 t12 0 t12 0 t12 0' fill='none' stroke='#000' stroke-width='3' stroke-linecap='round'/>
</svg>"""

SWIM_SMALL = f"""<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 64 64'>
  <g {STROKE}>
    <path d='M55 34 q6 -2 7 -8 q2 9 -6 13' fill='none'/>
    <path d='M27 42 C29 30 37 26 45 28 C53 30 57 36 59 42 Z'/>
    <path d='M35 38 q8 -3 13 1' fill='none'/>
    <ellipse cx='12' cy='22' rx='9' ry='5.5'/>
    <circle cx='27' cy='20' r='14'/>
    <path d='M27 6 q1 -4 5 -4' fill='none'/>
  </g>
  <circle cx='23' cy='16' r='2.2' fill='#000'/>
  <path d='M2 50 q6 -4 12 0 t12 0 t12 0 t12 0 t12 0' fill='none' stroke='#000' stroke-width='3' stroke-linecap='round'/>
</svg>"""

SLEEP = SWIM.replace(
    "<circle cx='22' cy='18' r='2.2' fill='#000'/>",
    "<rect x='18' y='14' width='8' height='8' fill='#fff' stroke='none'/>"
    "<path d='M18 18 q4 4 8 0' fill='none' stroke='#000' stroke-width='3' stroke-linecap='round'/>"
    "<path d='M46 10 h7 l-7 7 h7 M55 2 h5 l-5 5 h5' fill='none' stroke='#000' stroke-width='2.5' "
    "stroke-linecap='round' stroke-linejoin='round'/>",
)

VARIANTS = {
    'stand': ((STAND_SMALL, (36,)), (STAND, ())),
    'swim': ((SWIM_SMALL, (40, 48)), (SWIM, (260,))),
    'sleep': ((SWIM_SMALL.replace(
        "<circle cx='23' cy='16' r='2.2' fill='#000'/>",
        "<rect x='19' y='11' width='9' height='9' fill='#fff' stroke='none'/>"
        "<path d='M19 16 q4 4 9 0' fill='none' stroke='#000' stroke-width='3' stroke-linecap='round'/>"
        "<path d='M46 10 h7 l-7 7 h7 M55 2 h5 l-5 5 h5' fill='none' stroke='#000' stroke-width='2.5' "
        "stroke-linecap='round' stroke-linejoin='round'/>", ), ()), (SLEEP, (200,))),
}


def ascii_preview(image):
    px = image.convert('L')
    lines = []
    for y in range(px.height):
        row = ''.join('#' if px.getpixel((x, y)) < 128 else '.' for x in range(px.width))
        lines.append(row)
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', default=str(DEFAULT_OUT))
    parser.add_argument('--preview', action='store_true', help='ASCII-preview each raster')
    parser.add_argument('--png-dir', default=None, help='dump 8x nearest-neighbour PNGs here')
    args = parser.parse_args()

    spec = importlib.util.spec_from_file_location('sdk_icon_generator', GENERATOR)
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)

    png_dir = Path(args.png_dir) if args.png_dir else None
    if png_dir:
        png_dir.mkdir(parents=True, exist_ok=True)

    text = ['#pragma once', '', '#include "Icon.h"', '',
            '// Generated by scripts/generate_cpy_duck.py from the frozen CPY mascot',
            '// geometry (docs/plans/2026-10-15-cpyos-ui-theme-design.md). Do not edit.',
            '']
    with tempfile.TemporaryDirectory(prefix='cpy-duck-') as tmp:
        for name, groups in VARIANTS.items():
            for group_index, (svg, sizes) in enumerate(groups):
                if not sizes:
                    continue
                svg_path = Path(tmp) / f'{name}_{group_index}.svg'
                svg_path.write_text(svg, encoding='utf-8')
                for size in sizes:
                    image = generator.rasterize(str(svg_path), size)
                    if args.preview:
                        print(f'--- {name} {size}px ---')
                        print(ascii_preview(image))
                    if png_dir:
                        image.resize((size * 8, size * 8), Image.NEAREST).save(png_dir / f'{name}_{size}.png')
                    data, center = generator.pack(image, size)
                    bits = f'cpy_duck_{name}_{size}_bits'
                    text.append(f'// duck {name}; {size}px')
                    # inline (not static) so the header included from several
                    # translation units keeps exactly one copy in the image.
                    text.append(f'inline const uint8_t {bits}[] = {{')
                    text.append('    ' + ', '.join(f'0x{byte:02X}' for byte in data) + '};')
                    text.append(f'inline const freeink::Icon cpy_duck_{name}_{size} = '
                                f'{{{size}, {size}, {center}, {bits}}};')
                    text.append('')

    if not args.preview:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text('\n'.join(text), encoding='utf-8')
        print(f'wrote {out}')


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""Panel-corner geometry (`lib/GfxRenderer/PanelInsets.h`): the pure rules the
safe-area clamps rest on.

What is pinned here:
  * `cornerCut(0, ·) == 0` and `sideClearance(inset, 0, ·) == inset` for every
    straight-edge inset the shipped profiles declare. That identity is the
    whole byte-identity argument for the corner clamps: every consumer writes
    `max(existing, cornerCut(...))`, so at radius 0 the expression is provably
    the value it replaced.
  * the corner-cut table from the design (absorb-safearea §1.2), in integer
    pixels;
  * the two guards (`depth <= 0` is the whole radius, `depth >= radius` is a
    straight edge) and monotonicity in radius (up) and depth (down);
  * `isqrt` exactness on squares and the radius**2 boundary.

A mutation step then deletes the `depth >= radius` guard and requires the same
checks to fail: if they still pass, they are not watching the guard.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HEADER_DIR = ROOT / 'lib' / 'GfxRenderer'

PROGRAM = r'''
#include <cassert>
#include <cstdio>

#include "PanelInsets.h"

using namespace panel_insets;

int main() {
  // --- radius 0 is inert ---------------------------------------------------
  // The shipped profiles' straight-edge insets, as their TRBL sets: READ_PICO
  // {5,5,8,5}, X3/X4 {9,7,3,7}, the EEGO A4's {24,8,16,8}, the struct defaults
  // {9,3,3,3}, and a bare full-bleed panel.
  const int trbl[][4] = {{5, 5, 8, 5}, {9, 7, 3, 7}, {24, 8, 16, 8}, {9, 3, 3, 3}, {0, 0, 0, 0}};
  for (const auto& set : trbl) {
    for (const int inset : set) {
      for (int depth = 0; depth <= 64; ++depth) {
        assert(cornerCut(0, depth) == 0);
        assert(sideClearance(inset, 0, depth) == inset);
      }
    }
  }

  // --- the cut table (design §1.2) ----------------------------------------
  // Exact values are 9.6 / 6.0 / 1.7 at radius 30; integer isqrt floors the
  // root, so the cut rounds *up* — never less clearance than the true arc.
  assert(cornerCut(30, 8) == 10);
  assert(cornerCut(30, 12) == 6);
  assert(cornerCut(30, 20) == 2);
  assert(cornerCut(30, 30) == 0);
  assert(cornerCut(40, 8) == 16);
  assert(cornerCut(8, 8) == 0);
  assert(cornerCut(8, 12) == 0);
  assert(cornerCut(0, 0) == 0);

  // --- sideClearance is max(inset, cut) ------------------------------------
  assert(sideClearance(5, 30, 8) == 10);
  assert(sideClearance(24, 30, 8) == 24);
  assert(sideClearance(5, 30, 20) == 5);

  // --- guards and monotonicity --------------------------------------------
  for (int r = 1; r <= 255; ++r) {
    // Ink on (or past) the edge loses the whole arc.
    assert(cornerCut(r, 0) == r);
    assert(cornerCut(r, -5) == r);
    for (int d = r; d <= 300; ++d) assert(cornerCut(r, d) == 0);
    for (int d = 0; d <= 320; ++d) {
      assert(cornerCut(r, d) >= 0);
      assert(cornerCut(r, d) <= r);
      // Monotone up in radius, down in depth: a bigger corner never asks for
      // less clearance on the same row, and a deeper row never asks for more.
      assert(cornerCut(r, d) <= cornerCut(r + 1, d));
      assert(cornerCut(r, d + 1) <= cornerCut(r, d));
    }
  }

  // --- isqrt exactness ----------------------------------------------------
  for (int n = 0; n <= 65025; ++n) {
    const int s = isqrt(n);
    assert(static_cast<long long>(s) * s <= n);
    assert(static_cast<long long>(s + 1) * (s + 1) > n);
  }
  for (int s = 0; s <= 255; ++s) {
    assert(isqrt(s * s) == s);
    if (s > 0) assert(isqrt(s * s - 1) == s - 1);
  }

  std::puts("PanelInsets: identity at radius 0, cut table, guards, monotonicity and isqrt pass");
}
'''


def build(work: Path, name: str, include_dir: Path) -> Path:
    src = work / name / 'check.cpp'
    src.parent.mkdir(parents=True, exist_ok=True)
    src.write_text(PROGRAM)
    binary = work / name / 'check'
    subprocess.run(
        ['c++', '-std=c++20', '-O1', '-Wall', '-Wextra', '-I', str(include_dir), str(src), '-o', str(binary)],
        check=True,
    )
    return binary


def main() -> None:
    with tempfile.TemporaryDirectory(prefix='panel-insets-') as tmp:
        work = Path(tmp)
        subprocess.run([str(build(work, 'real', HEADER_DIR))], check=True)

        # Mutation: drop the `depth >= radius` guard. Past the arc the radicand
        # goes negative, the integer root clamps at 0 and the cut becomes the
        # full radius — which the `d >= r` assertions must catch.
        mutant_dir = work / 'mutant'
        mutant_dir.mkdir()
        source = (HEADER_DIR / 'PanelInsets.h').read_text()
        needle = '  if (depth >= radius) return 0;\n'
        if needle not in source:
            raise SystemExit('mutation needle missing: the depth guard changed shape')
        (mutant_dir / 'PanelInsets.h').write_text(source.replace(needle, ''))

        binary = build(work, 'mutant', mutant_dir)
        result = subprocess.run([str(binary)], capture_output=True)
        if result.returncode == 0:
            raise SystemExit('mutation not detected: dropping the depth guard left every assertion green')
        print('PASS: depth-guard mutation detected')


if __name__ == '__main__':
    main()

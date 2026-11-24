#!/usr/bin/env bash
# Build every native game package for both targets.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
games=(2048 sokoban gomoku minesweeper sudoku chinese-chess)
for game in "${games[@]}"; do
  dir="$here/$game"
  src="$dir/main/plugin.c"
  [[ -f "$src" ]] || { echo "missing $src" >&2; exit 1; }
  bash "$repo/plugins/sdk/build_e4p.sh" "$src" "$dir/dist/plugin.elf"
  cp "$dir/manifest.json" "$dir/dist/manifest.json"
  bash "$repo/plugins/sdk/build_xtensa.sh" "$src" "$dir/dist-xtensa/plugin.elf"
  cp "$dir/manifest-xtensa.json" "$dir/dist-xtensa/manifest.json"
done
printf 'built %d games for both targets\n' "${#games[@]}"

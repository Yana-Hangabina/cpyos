#!/usr/bin/env bash
# Host regression for the blocking font-download gate (src/LanguageFontGate.h).
#
# Includes the mutation step the plan requires: a gate rule that no build can break is not
# a gate. Each mutation is reverted by the trap, and the run fails if a mutation stays green.
set -euo pipefail

cd "$(dirname "$0")/../.."
SRC=src/LanguageFontGate.h
OUT=$(mktemp -d)
restore() {  # the header is untracked, so git checkout cannot undo a half-applied mutation
  for backup in "$OUT"/*.orig; do
    [ -e "$backup" ] || continue
    cp "$backup" "$SRC"
  done
  rm -rf "$OUT"
}
trap restore EXIT

build_run() {
  g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -Isrc \
      -o "$OUT/gate" test/language_font_gate/GateTest.cpp
  "$OUT/gate"
}

echo "== baseline"
build_run

mutate() {  # <name> <python-slice>
  local name="$1"; shift
  cp "$SRC" "$OUT/$name.orig"
  python3 - "$SRC" "$@" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1]); src = path.read_text()
old, new = sys.argv[2], sys.argv[3]
assert old in src, f'mutation anchor missing for {old!r}'
path.write_text(src.replace(old, new, 1))
PY
  if (cd "$(pwd)" && build_run >"$OUT/$name.log" 2>&1); then
    echo "MUTATION SURVIVED (bad): $name"
    exit 1
  fi
  grep -q '^FAIL: ' "$OUT/$name.log" && echo "mutation '$name' -> red as expected"
  cp "$OUT/$name.orig" "$SRC"
}

# 1. Ignore whether packs are missing: the gate would let anyone into a tofu screen.
mutate ignore-missing-packs \
  'if (!packsMissing(request.requiredMask, request.installedMask)) {' \
  'if (true) {'
# 2. Treat every script as fixable: Arabic would get a download page that cannot help it.
mutate never-refuse \
  'constexpr bool unsupportedScript(uint8_t language) { return language_font::mustRefuse(language); }' \
  'constexpr bool unsupportedScript(uint8_t) { return false; }'
# 3. Let Back escape while a language is pending.
mutate exit-always-allowed \
  'return request.wantsExit ? Decision::AllowExit : Decision::AllowSelection;' \
  'return Decision::AllowExit;'

echo "== re-verified after mutations"
build_run
echo "ALL PASS"

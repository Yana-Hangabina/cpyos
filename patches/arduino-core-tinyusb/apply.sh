#!/bin/sh
# Inject the Arduino TinyUSB Kconfig symbols into the pioarduino precompiled
# core, or take them back out. See README.md for why the core needs this.
#
#   ./apply.sh            inject (idempotent)
#   ./apply.sh --revert   remove the injected block
#   ./apply.sh --status   report which state the core file is in
#
# The injected text is wrapped in marker comments, so both directions are exact
# and neither depends on a backup file existing.
set -eu

CORE="${ARDUINO_CORE_DIR:-$HOME/.platformio/packages/framework-arduinoespressif32}"
TARGET="$CORE/Kconfig.projbuild"
HERE="$(cd "$(dirname "$0")" && pwd)"
FRAGMENT="$HERE/tinyusb.kconfig"
BEGIN="# --- BEGIN crossmux arduino-core-tinyusb ---"
END="# --- END crossmux arduino-core-tinyusb ---"

die() {
    echo "apply.sh: $*" >&2
    exit 1
}

[ -f "$TARGET" ] || die "no such file: $TARGET
  Set ARDUINO_CORE_DIR if the pioarduino core lives somewhere else."
[ -f "$FRAGMENT" ] || die "no such file: $FRAGMENT"

is_applied() { grep -qF "$BEGIN" "$TARGET"; }

case "${1:-apply}" in
--status)
    if is_applied; then
        echo "applied: $TARGET defines $(grep -c '^config TINYUSB_' "$TARGET") TINYUSB symbols"
    elif [ "$(grep -c 'TINYUSB' "$TARGET")" -ne 0 ]; then
        # apply refuses in this state, so say so here too rather than reporting
        # "not applied" over a file that already defines the symbols its own way.
        echo "BLOCKED: $TARGET mentions TINYUSB $(grep -c 'TINYUSB' "$TARGET") times without our markers."
        echo "  The core grew its own definitions, or a previous injection lost its markers."
        echo "  Inspect it; do not apply over it."
        exit 1
    else
        echo "not applied: $TARGET defines no TINYUSB symbols"
    fi
    ;;

--revert)
    if ! is_applied; then
        echo "nothing to revert: no marker block in $TARGET"
        exit 0
    fi
    awk -v begin="$BEGIN" -v endm="$END" '
        $0 == begin { skip = 1; next }
        $0 == endm  { skip = 0; next }
        !skip       { print }
    ' "$TARGET" > "$TARGET.new"
    mv "$TARGET.new" "$TARGET"
    # The core file must be back to defining nothing, or the next build silently
    # keeps whatever the leftover symbols were.
    [ "$(grep -c 'TINYUSB' "$TARGET")" -eq 0 ] ||
        die "revert left TINYUSB text in $TARGET"
    echo "reverted: $TARGET"
    echo "now delete the generated configs so the next build re-resolves them:"
    echo "  rm -f sdkconfig.readpico_usbmsc && rm -rf .pio/build/readpico_usbmsc"
    ;;

apply)
    if is_applied; then
        echo "already applied: $(grep -c '^config TINYUSB_' "$TARGET") TINYUSB symbols in $TARGET"
        exit 0
    fi
    [ "$(grep -c 'TINYUSB' "$TARGET")" -eq 0 ] ||
        die "$TARGET mentions TINYUSB without our markers; refusing to guess. Inspect it first."

    # One-time safety copy next to the target, never clobbered by a re-run.
    [ -f "$TARGET.crossmux-bak" ] || cp -p "$TARGET" "$TARGET.crossmux-bak"

    # Insert before the closing `endmenu` of the file's single top-level menu so
    # the symbols sit inside "Arduino Configuration" like every other symbol.
    awk -v begin="$BEGIN" -v endm="$END" -v frag="$FRAGMENT" '
        { line[NR] = $0 }
        END {
            at = 0
            for (i = NR; i >= 1; i--) if (line[i] == "endmenu") { at = i; break }
            if (at == 0) at = NR + 1
            for (i = 1; i < at; i++) print line[i]
            print begin
            while ((getline l < frag) > 0) print l
            close(frag)
            print endm
            for (i = at; i <= NR; i++) print line[i]
        }
    ' "$TARGET" > "$TARGET.new"
    mv "$TARGET.new" "$TARGET"

    got=$(grep -c '^config TINYUSB_' "$TARGET")
    want=$(grep -c '^config TINYUSB_' "$FRAGMENT")
    if [ "$got" -ne "$want" ]; then
        cp -p "$TARGET.crossmux-bak" "$TARGET"
        die "injected $got symbols but the fragment has $want; restored $TARGET.crossmux-bak"
    fi
    echo "applied: $got TINYUSB symbols injected into $TARGET"
    echo "now delete the generated configs so the next build re-resolves them:"
    echo "  rm -f sdkconfig.readpico_usbmsc && rm -rf .pio/build/readpico_usbmsc"
    ;;

*)
    die "unknown argument '$1' (expected: apply | --revert | --status)"
    ;;
esac

#!/bin/sh
# Re-apply the local freeink-sdk modifications on top of the pinned upstream commit.
#
# freeink-sdk is a gitlink this repository cannot commit into (see README.md next to
# this script), so the local work travels as a patch instead. Run this after
# `git submodule update --init` on a fresh clone, or to restore the work after anything
# checks the pinned commit out over the tree again.
#
# Base: upstream d9a1304c (2026-11-03). The CFF/OTF sibling patch is gone: upstream now
# ships cff + psaux + pshinter + psnames behind the same FREEINK_FONT_ENABLE_CFF.
set -e
cd "$(dirname "$0")/../.."
if [ ! -d freeink-sdk/libs ]; then
  echo "freeink-sdk working tree missing; run: git submodule update --init freeink-sdk" >&2
  exit 1
fi
# Marker: the water-ripple engine directory only this patch creates. Its presence means
# the work is already in place (idempotent on repeated runs).
if [ -f freeink-sdk/libs/display/EpdiyLcd/src/e0470_page_turn/e0470_page_turn.c ]; then
  echo "freeink-sdk local patch already applied" >&2
  exit 0
fi
# A tree still pinned at 96de1be (or carrying the older patch) cannot be patched in place:
# the facade and the epdiy driver adapter were rebased onto d9a1304c's driver contract.
if [ -f freeink-sdk/libs/display/FreeInkDisplay/src/driver/EpdiyLcdDriver.cpp ] &&
   ! grep -q "supportsDeferredGrayscaleBase" freeink-sdk/libs/display/FreeInkDisplay/src/driver/PanelDriver.h; then
  echo "freeink-sdk is not at d9a1304c; move the gitlink first (see README.md, 'Upgrading an older tree')" >&2
  exit 1
fi
patch -p1 -d freeink-sdk < patches/freeink-sdk/d9a1304c-local.patch
echo "freeink-sdk local patch applied"

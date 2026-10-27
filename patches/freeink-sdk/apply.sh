#!/bin/sh
# Re-apply the local freeink-sdk modifications on top of the pinned upstream commit.
#
# freeink-sdk is a gitlink this repository cannot commit into (see README.md next to
# this script), so the local work travels as a patch instead. Run this after
# `git submodule update --init` on a fresh clone, or to restore the work after anything
# checks the pinned commit out over the tree again.
set -e
cd "$(dirname "$0")/../.."
if [ ! -d freeink-sdk/libs ]; then
  echo "freeink-sdk working tree missing; run: git submodule update --init freeink-sdk" >&2
  exit 1
fi
if grep -q epd_draw_base_start freeink-sdk/libs/display/EpdiyLcd/src/epdiy/src/render.c; then
  echo "freeink-sdk local patch already applied" >&2
  exit 0
fi
# The deferred-scan round extended this patch and moved the marker. A tree that
# only carries the earlier version cannot be patched in place -- restore it to
# 96de1be first (see README, "Upgrading an older tree").
if grep -q e0470_textturn_soft_settle freeink-sdk/libs/display/EpdiyLcd/src/e0470/e0470_epaper_waveform.c; then
  echo "an OLDER version of 96de1be-local.patch is already applied;" >&2
  echo "restore freeink-sdk to upstream 96de1be before running this script (README.md)" >&2
  exit 1
fi
patch -p1 -d freeink-sdk < patches/freeink-sdk/96de1be-local.patch
echo "freeink-sdk local patch applied"

#!/bin/sh
# Apply the CFF/OTF FreeType additions to the freeink-sdk working tree.
#
# freeink-sdk is a gitlink this repository cannot commit into (see
# ../freeink-sdk/README.md), so these files travel as a patch. Unlike the
# 96de1be-local.patch (display work), this patch only ADDS files plus one
# hunk in ftmodule.h, so it applies independently of that patch in any order.
# Run after `git submodule update --init` on a fresh clone.
set -e
cd "$(dirname "$0")/../.."
if [ ! -d freeink-sdk/libs ]; then
  echo "freeink-sdk working tree missing; run: git submodule update --init freeink-sdk" >&2
  exit 1
fi
if [ -f freeink-sdk/libs/font/FreeInkFont/src/freetype/ft_cff.c ]; then
  echo "freeink-sdk CFF patch already applied" >&2
  exit 0
fi
patch -p1 -d freeink-sdk < patches/freeink-sdk-cff/cff-otf.patch
echo "freeink-sdk CFF patch applied"

#!/bin/sh
# Host regression for src/pdf/PdfModuleHeaderCheck.h -- the gate that runs BEFORE the
# pdfmod partition is erased.
#
# The failure it prevents is specific and unrecoverable by ordinary means: an image whose
# 80-byte header is intact but whose payload is short used to be accepted, erase the only
# working engine, and then be refused by the loader.  This board has no BOOT/RESET button,
# so that state means a full web flash.  Hence: geometry is validated up front, the real
# build/pdfmod.bin must pass, and the same file truncated by one byte must not.
#
#   bash test/pdf_header_check/run_pdf_header_check_test.sh [path-to-pdfmod.bin]
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
HEADER_REL="pdf/PdfModuleHeaderCheck.h"
CXX=${CXX:-g++}
IMAGE=${1:-$REPO_ROOT/build/pdfmod.bin}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pdf-header-check-test.XXXXXX")
PRISTINE_SHA=$(shasum -a 256 "$REPO_ROOT/src/$HEADER_REL" | awk '{print $1}')
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

run_suite() {
  out_dir=$1
  include_dir=$2
  mkdir -p "$out_dir"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$include_dir" -o "$out_dir/hdr_test" "$SCRIPT_DIR/PdfModuleHeaderCheckTest.cpp"
  "$out_dir/hdr_test" "$IMAGE"
}

echo "== pdf header check: normal =="
run_suite "$BUILD_DIR/normal" "$REPO_ROOT/src/pdf"

# mutate <label> <marker> <perl expression>
mutate() {
  label=$1
  marker=$2
  expr=$3
  dir="$BUILD_DIR/m$(printf '%s' "$label" | tr -cd '[:alnum:]')"
  mkdir -p "$dir/pdf"
  cp "$REPO_ROOT/src/$HEADER_REL" "$dir/pdf/PdfModuleHeaderCheck.h"
  perl -0pi -e "$expr" "$dir/pdf/PdfModuleHeaderCheck.h"
  if cmp -s "$dir/pdf/PdfModuleHeaderCheck.h" "$REPO_ROOT/src/$HEADER_REL"; then
    echo "FAIL: '$label' did not modify the header; the run below would pass vacuously" >&2
    exit 1
  fi
  if ! grep -qF "$marker" "$dir/pdf/PdfModuleHeaderCheck.h"; then
    echo "FAIL: '$label' did not land where intended (missing '$marker')" >&2
    exit 1
  fi
  if run_suite "$dir-run" "$dir/pdf" >/dev/null 2>&1; then
    echo "FAIL: mutation not detected ($label)" >&2
    exit 1
  fi
  echo "mutation detected ($label)"
}

# 1. The containment rule -- the one that stops a truncated image reaching the erase.
mutate "the file-size containment check is dropped" '/* HC-NOFILE */' \
  's~if \(total > fileBytes\) return Status::FileTooSmall;~/* HC-NOFILE */~'

# 2. The partition bound: an oversized image would be written past the partition end.
mutate "the partition-size check is dropped" '/* HC-NOPART */' \
  's~if \(partitionBytes != 0 && total > partitionBytes\) return Status::PartitionTooSmall;~/* HC-NOPART */~'

# 3. The ABI comparison, i.e. the guard this feature nearly shipped without.
mutate "the ABI comparison is dropped" '/* HC-NOABI */' \
  's~if \(h\.abi != kPdfModuleAbiVersion\) return Status::AbiMismatch;~/* HC-NOABI */~'

# 3b. Host API: the module calls services through this table, so a different
#     major (or a newer minor) must be a compatibility failure, not a crash.
mutate "the host API comparison is dropped" '/* HC-NOHOST */' \
  's~if \(\(h\.hostApi >> 16\) != static_cast<uint32_t>\(kPdfModuleHostApiMajor\) \|\|\n      \(h\.hostApi & 0xFFFFu\) > static_cast<uint32_t>\(kPdfModuleHostApiMinor\)\) \{\n    return Status::HostApiMismatch;\n  \}~/* HC-NOHOST */~'

# 3c. Memory layout: the loader maps fixed virtual addresses, so a different key
#     would execute the image at the wrong addresses.
mutate "the layout-key comparison is dropped" '/* HC-NOLAYOUT */' \
  's~if \(h\.layoutKey != kPdfModuleLayoutKey\) return Status::LayoutMismatch;~/* HC-NOLAYOUT */~'

# 4. Section ordering: overlapping sections are the shape of a spliced or half-written file.
mutate "section overlap is no longer refused" '/* HC-OVERLAP */' \
  's~const uint64_t rodataEnd = static_cast<uint64_t>\(h\.rodataOffset\) \+ h\.rodataSize;~const uint64_t rodataEnd = h.rodataOffset; /* HC-OVERLAP */~'

# 5. An entry offset outside the code section is an immediate crash after a successful
#    erase, which is precisely the class of failure this gate exists to keep out.
mutate "the entry point is not bounds-checked" '/* HC-NOENTRY */' \
  's~if \(h\.entryOffset >= h\.codeSize\) return Status::EntryOutsideCode;~/* HC-NOENTRY */~'

NOW_SHA=$(shasum -a 256 "$REPO_ROOT/src/$HEADER_REL" | awk '{print $1}')
if [ "$NOW_SHA" != "$PRISTINE_SHA" ]; then
  echo "FAIL: a mutation modified the pristine header under src/ -- the tree is dirty" >&2
  git -C "$REPO_ROOT" diff --stat -- "$HEADER_REL" >&2 || true
  exit 1
fi
echo "pdf header check host regression: PASS"

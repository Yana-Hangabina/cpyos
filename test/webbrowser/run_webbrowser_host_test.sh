#!/bin/sh
# Host regression for the browser's reading pipeline. Offline: URL resolution,
# tolerant HTML tokenizing, reader-mode extraction, pagination and charset
# sniffing all run on fixtures, with ASan/UBSan.
#
# Two mutation runs prove the checks are load-bearing: breaking the CJK
# no-break-before rule or the foreign-scheme guard must turn the suite red.
#
#   bash test/webbrowser/run_webbrowser_host_test.sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
LIB_SRC="$REPO_ROOT/lib/WebBrowser/src"
TXT_SRC="$REPO_ROOT/lib/Txt"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/webbrowser-host-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

run_suite() {
  out_dir=$1
  src_dir=$2
  mkdir -p "$out_dir"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined -DENABLE_CHINESE_VERSION=1 \
    -I"$src_dir" -I"$TXT_SRC" -o "$out_dir/webbrowser_test" \
    "$SCRIPT_DIR/WebBrowserHostTest.cpp" "$src_dir"/*.cpp "$TXT_SRC/TxtEncoding.cpp"
  "$out_dir/webbrowser_test"
}

echo "== webbrowser: normal =="
run_suite "$BUILD_DIR/normal" "$LIB_SRC"

echo "== webbrowser: mutation (allow break before closing punctuation) =="
mkdir -p "$BUILD_DIR/mut1"
cp "$LIB_SRC"/* "$BUILD_DIR/mut1/"
perl -0pi -e 's/if \(noBreakBefore\(cp\) \|\| noBreakAfter\(previous\)\) return false;/if (false) return false;/' \
  "$BUILD_DIR/mut1/TextPager.cpp"
if run_suite "$BUILD_DIR/mut1run" "$BUILD_DIR/mut1" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (pagination punctuation rule)" >&2
  exit 1
fi
echo "mutation detected"

echo "== webbrowser: mutation (accept foreign URL schemes) =="
mkdir -p "$BUILD_DIR/mut2"
cp "$LIB_SRC"/* "$BUILD_DIR/mut2/"
perl -0pi -e 's/if \(!portLike\) return false;/if (false) return false;/' "$BUILD_DIR/mut2/WebUrl.cpp"
if run_suite "$BUILD_DIR/mut2run" "$BUILD_DIR/mut2" >/dev/null 2>&1; then
  echo "FAIL: mutation was not detected (foreign URL scheme guard)" >&2
  exit 1
fi
echo "mutation detected"

echo "webbrowser host regression: PASS"

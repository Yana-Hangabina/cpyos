#!/bin/sh
# Host regression for the cache-clear keep rule and its sweep:
#   lib/BookCache/BookCachePrune.h    -- which directory entries a clear may delete
#   lib/BookCache/BookCachePrune.cpp  -- the sweep, run against an in-memory filesystem
#
# This is a data-loss test, not a tidiness test.  Before the rule existed, every reader's
# clearCache() ran Storage.removeDir() on the per-book cache directory, which is also where
# annotations.bin / bookmarks.bin / progress.bin live -- so 设置 → 清除阅读缓存 (which sweeps
# EVERY book cache at once) and the per-book 清除分页缓存 destroyed the reader's own work in
# all six formats, and PDF lost it silently whenever the file's size or mtime changed.
#
# Each mutation removes one decision that protects that work (or one that makes the clear
# actually clear) and must turn the suite red.  Two vacuous-green failure modes are refused
# outright, because both happened while this file was being written:
#   * a patch that does not modify the file at all ("did not apply") -- caught by cmp;
#   * a patch that modifies the file but lands in the WRONG place -- caught by requiring a
#     marker comment that each patch stamps at its own target.  Without the marker, an
#     `if (!name` anchor hit isUserDataName() instead of shouldDelete() and the mutation
#     "passed" while testing a different rule entirely.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
HEADER_REL="lib/BookCache/BookCachePrune.h"
CPP_REL="lib/BookCache/BookCachePrune.cpp"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/book-cache-prune-test.XXXXXX")
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

# A copy of the tree carrying only the file under test, so the shipped one is never touched.
prepare_copy() {
  dir=$1
  mkdir -p "$dir/$(dirname "$HEADER_REL")"
  cp "$REPO_ROOT/$HEADER_REL" "$dir/$HEADER_REL"
  cp "$REPO_ROOT/$CPP_REL" "$dir/$CPP_REL"
}

run_suite() {
  out_dir=$1
  root=$2
  mkdir -p "$out_dir"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$root/lib/BookCache" -o "$out_dir/prune_test" "$SCRIPT_DIR/BookCachePruneTest.cpp"
  "$out_dir/prune_test"
}

# The sweep needs the in-memory HalStorage/Logging in stubs/ to shadow the firmware ones.
run_exec_suite() {
  out_dir=$1
  root=$2
  mkdir -p "$out_dir"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$SCRIPT_DIR/stubs" -I"$root/lib/BookCache" \
    -o "$out_dir/prune_exec_test" "$SCRIPT_DIR/BookCachePruneExecTest.cpp" \
    "$root/lib/BookCache/BookCachePrune.cpp"
  "$out_dir/prune_exec_test"
}

# run_mutation <label> <marker> <suite-kind> <patch> <copy-dir> <target-rel>
guard_and_run() {
  label=$1
  marker=$2
  kind=$3
  dir=$4
  target=$5
  if cmp -s "$dir/$target" "$REPO_ROOT/$target"; then
    echo "FAIL: '$label' did not modify $target; the run below would pass vacuously" >&2
    exit 1
  fi
  if ! grep -qF "$marker" "$dir/$target"; then
    echo "FAIL: '$label' did not land where intended (missing '$marker' in $target)" >&2
    exit 1
  fi
  if [ "$kind" = header ]; then
    if run_suite "$dir-run" "$dir" >/dev/null 2>&1; then
      echo "FAIL: mutation not detected ($label)" >&2
      exit 1
    fi
  else
    if run_exec_suite "$dir-run" "$dir" >/dev/null 2>&1; then
      echo "FAIL: mutation not detected ($label)" >&2
      exit 1
    fi
  fi
  echo "mutation detected ($label)"
}

mutate_header() {
  label=$1
  marker=$2
  patch=$3
  dir="$BUILD_DIR/h_$(printf '%s' "$label" | tr -cd '[:alnum:]')"
  prepare_copy "$dir"
  "$patch" "$dir/$HEADER_REL"
  guard_and_run "$label" "$marker" header "$dir" "$HEADER_REL"
}

mutate_exec() {
  label=$1
  marker=$2
  patch=$3
  dir="$BUILD_DIR/x_$(printf '%s' "$label" | tr -cd '[:alnum:]')"
  prepare_copy "$dir"
  "$patch" "$dir/$CPP_REL"
  guard_and_run "$label" "$marker" exec "$dir" "$CPP_REL"
}

# --- the rule ---------------------------------------------------------------------------

# 1. The whole point: the reader's files must be recognised.
drop_keep_set() {
  perl -0pi -e 's/if \(std::strcmp\(name, kept\) == 0\) return true;/if (false) return true; \/* XM-KEEPSET *\//' "$1"
}
# 2. A "clear cache" that clears nothing is its own bug: it frees no space and leaves the
#    corrupt pagination in place, so the menu entry silently stops doing its job.
keep_everything() {
  perl -0pi -e 's/inline bool shouldDelete\(const char\* name\) \{/inline bool shouldDelete(const char* name) { if (name) return false; \/* XM-KEEPEVERY *\//' "$1"
}
# 3. An interrupted atomic replace leaves the newest content in <name>.tmp, so deleting it
#    with the rest of the cache can destroy the only copy of a reader's notes.
drop_temp_sibling_rule() {
  perl -0pi -e 's/if \(endsWith\(name, suffix\)\) \{/if (false \&\& endsWith(name, suffix)) { \/* XM-TEMPRULE *\//' "$1"
}
# 4. The base name is copied into a fixed buffer; without the bound a long directory entry
#    writes off the end of the stack (ASan catches it).
drop_base_buffer_bound() {
  perl -0pi -e 's/if \(cut < sizeof\(base\)\) \{/if (true) { \/* XM-NOBOUND *\//' "$1"
}
# 5. Substring matching would also protect "progress.binx" and "xannotations.bin" -- a loose
#    rule quietly stops the clear from clearing.
looseen_to_substring() {
  perl -0pi -e 's/if \(std::strcmp\(name, kept\) == 0\) return true;/if (std::strstr(name, kept) != nullptr) return true; \/* XM-SUBSTR *\//' "$1"
}
# 6. An entry whose name did not fit the caller's buffer reads as empty, and deleting ""
#    means deleting the directory itself -- the rmtree this rule replaced.  The anchor is
#    this guard's own comment, which exists only inside shouldDelete().
delete_unnamed_entries() {
  perl -0pi -e 's{return false;  \/\/ never act on an unnamed entry}{return true;  \/* XM-UNNAMED *\/}' "$1"
}

# --- the sweep --------------------------------------------------------------------------

# Deletes the reader's files as well: the original bug through the back door.
exec_deletes_user_data() {
  perl -0pi -e 's/if \(!shouldDelete\(name\)\) \{/if (false) { \/* XX-ALL *\//' "$1"
}
# Treats every entry as a plain file, so sections/ and html/ lose their directory node but
# keep their children: the "cleared" cache still holds the stale pagination.
exec_no_recursion() {
  perl -0pi -e 's/const bool isDir = file\.isDirectory\(\);/const bool isDir = false;  \/* XX-NORECUR *\//' "$1"
}
# One pass only.  A cursor that shifts children into the hole it just made then skips half
# the cache -- the failure this sweep actually had while these tests were written.
exec_single_pass() {
  perl -0pi -e 's/while \(last\.removed > 0 && rounds < kMaxPasses\);/while (false);  \/* XX-ONEPASS *\//' "$1"
}
# A stuck entry becomes invisible, so the settings page reports "Cache cleared" while the
# blob the user was trying to get rid of is still on the card.
exec_swallow_failures() {
  perl -0pi -e 's/      result\.failed = true;/      ;  \/* XX-SWALLOW *\//' "$1"
}
# A cache that was never built becomes an error: a brand-new card shows the failure state,
# and the FB2 branch ANDs per-format results so one absent format fails the whole action.
exec_missing_is_error() {
  perl -0pi -e 's/if \(!Storage\.exists\(cachePath\.c_str\(\)\)\) return true;/if (!Storage.exists(cachePath.c_str())) return false;  \/* XX-MISSING *\//' "$1"
}
# Removes the directory at the end, which takes the preserved files with it.
exec_removes_the_directory() {
  perl -0pi -e 's/  LOG_DBG\("CACHE", "pruned /  Storage.removeDir(cachePath.c_str());  \/* XX-RMDIR *\/\n  LOG_DBG("CACHE", "pruned /' "$1"
}

echo "== book cache prune: rule =="
run_suite "$BUILD_DIR/normal" "$REPO_ROOT"

mutate_header "keep set disabled" '/* XM-KEEPSET */' drop_keep_set
mutate_header "nothing is ever deleted" '/* XM-KEEPEVERY */' keep_everything
mutate_header "temp siblings of user data deleted" '/* XM-TEMPRULE */' drop_temp_sibling_rule
mutate_header "base-name buffer bound removed" '/* XM-NOBOUND */' drop_base_buffer_bound
mutate_header "keep match loosened to substring" '/* XM-SUBSTR */' looseen_to_substring
mutate_header "unnamed entry marked deletable" '/* XM-UNNAMED */' delete_unnamed_entries

echo "== book cache prune: sweep =="
run_exec_suite "$BUILD_DIR/exec" "$REPO_ROOT"

mutate_exec "sweep deletes user data too" '/* XX-ALL */' exec_deletes_user_data
mutate_exec "sweep does not recurse into directories" '/* XX-NORECUR */' exec_no_recursion
mutate_exec "sweep runs a single pass" '/* XX-ONEPASS */' exec_single_pass
mutate_exec "sweep swallows a failed removal" '/* XX-SWALLOW */' exec_swallow_failures
mutate_exec "sweep treats a missing cache as failure" '/* XX-MISSING */' exec_missing_is_error
mutate_exec "sweep removes the directory itself" '/* XX-RMDIR */' exec_removes_the_directory

echo "book cache prune host regression: PASS"

# On hardware the equivalent is one serial command, which runs the product path against
# SdFat's real directory cursor:
#   CMD:PRUNE /JJWXC/some-book.epub
#   [ERR] CACHE: serial CMD:PRUNE ok=1 annotations 1->1 progress 1->1 meta 1->0 book 1->0
# The two directions in that line are the entire rule: derived blobs 1->0, reader data 1->1.

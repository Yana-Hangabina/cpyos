#!/bin/sh
# Host regression for the Wi-Fi attempt attribution rules
#   src/activities/network/WifiAttempt.h
#
# Both rules exist because of one released symptom: 首页「检查晋江更新」偶尔显示
# 「连接失败 / <SSID> / 错误:通用故障」 while the join had not been refused by
# anything.  WL_CONNECT_FAILED is a stored field in the Arduino core
# (libraries/WiFi/src/STA.cpp:251) and WiFi.begin() returns it both for "the
# driver refused this join" and for "a previous attempt failed", so the sheet
# can end a healthy association with the generic error.  The disconnect-event
# clock is what separates them, and these checks pin that separation.
#
#   bash test/wifi_attempt/run_wifi_attempt_test.sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
HEADER_REL="activities/network/WifiAttempt.h"
CXX=${CXX:-g++}
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/wifi-attempt-test.XXXXXX")
# A mutation harness must never be able to edit the tree it is testing (a real
# accident in test/selection_hold: a perl -0pi whose file argument was lost
# rewrote the pristine header).  Digest taken now, re-checked before PASS.
PRISTINE_SHA=$(shasum -a 256 "$REPO_ROOT/src/$HEADER_REL" | awk '{print $1}')
trap 'rm -rf "$BUILD_DIR"' EXIT HUP INT TERM

run_suite() {
  out_dir=$1
  include_dir=$2
  mkdir -p "$out_dir"
  "$CXX" -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I"$include_dir" -o "$out_dir/attempt_test" "$SCRIPT_DIR/WifiAttemptTest.cpp"
  "$out_dir/attempt_test"
}

# mutate <label> <marker> <perl expression>
mutate() {
  label=$1
  marker=$2
  expr=$3
  dir="$BUILD_DIR/m$(printf '%s' "$label" | tr -cd '[:alnum:]')"
  mkdir -p "$dir/network"
  cp "$REPO_ROOT/src/$HEADER_REL" "$dir/network/WifiAttempt.h"
  perl -0pi -e "$expr" "$dir/network/WifiAttempt.h"
  if cmp -s "$dir/network/WifiAttempt.h" "$REPO_ROOT/src/$HEADER_REL"; then
    echo "FAIL: '$label' did not modify the header; the run below would pass vacuously" >&2
    exit 1
  fi
  if ! grep -qF "$marker" "$dir/network/WifiAttempt.h"; then
    echo "FAIL: '$label' did not land where intended (missing '$marker')" >&2
    exit 1
  fi
  if run_suite "$dir-run" "$dir/network" >/dev/null 2>&1; then
    echo "FAIL: mutation not detected ($label)" >&2
    exit 1
  fi
  echo "ok: mutation detected ($label)"
}

echo "== wifi_attempt suite =="
run_suite "$BUILD_DIR/base" "$REPO_ROOT/src/activities/network"

# The three pinned numbers are wl_status_t's real values.  Reading them out of
# the installed core header keeps the test honest; without a PlatformIO install
# this step is announced rather than quietly skipped.
CORE_HEADER="$HOME/.platformio/packages/framework-arduinoespressif32/libraries/WiFi/src/WiFiType.h"
if [ -f "$CORE_HEADER" ]; then
  for pair in "WL_NO_SSID_AVAIL 1" "WL_CONNECT_FAILED 4" "WL_DISCONNECTED 6" "WL_CONNECTED 3"; do
    name=$(printf '%s' "$pair" | cut -d' ' -f1)
    want=$(printf '%s' "$pair" | cut -d' ' -f2)
    got=$(grep -E "^[[:space:]]*$name[[:space:]]*=[[:space:]]*[0-9]+" "$CORE_HEADER" |
          sed -E 's/.*=[[:space:]]*([0-9]+).*/\1/')
    if [ "$got" != "$want" ]; then
      echo "FAIL: core header says $name=$got, the pinned constant says $want" >&2
      exit 1
    fi
  done
  echo "ok: pinned wl_status_t values match $CORE_HEADER"
else
  echo "SKIP: core header pinning (no PlatformIO install at $CORE_HEADER)"
fi

mutate 'stale CONNECT_FAILED treated as a refusal' 'XM-STALE' \
  's/after\.status == kStatusConnectFailed && before\.status != kStatusConnectFailed/after.status == kStatusConnectFailed \&\& true \/* XM-STALE *\//'

mutate 'begin() ignores the disconnect-event clock' 'XM-EVENT' \
  's/if \(after\.disconnectEvents != before\.disconnectEvents\) return Verdict::RejectedByAp;/if (false) return Verdict::RejectedByAp; \/* XM-EVENT *\//'

mutate 'begin() never reports a driver refusal' 'XM-NEVER' \
  's/return Verdict::DriverRefused;/return Verdict::Issued; \/* XM-NEVER *\//'

mutate 'NO_SSID_AVAIL stops being a failure' 'XM-NOSSID' \
  's/return status == kStatusConnectFailed \|\| status == kStatusNoSsidAvail;/return status == kStatusConnectFailed; \/* XM-NOSSID *\//'

mutate 'polled failures ignore the event clock' 'XM-ALWAYS' \
  's/return !eventsTracked \|\| now\.disconnectEvents != attemptStart\.disconnectEvents;/return true; \/* XM-ALWAYS *\//'

mutate 'untracked builds start failing early' 'XM-UNTRACKED' \
  's/if \(!eventsTracked\) return Verdict::Issued;/if (false) return Verdict::Issued; \/* XM-UNTRACKED *\//'

AFTER_SHA=$(shasum -a 256 "$REPO_ROOT/src/$HEADER_REL" | awk '{print $1}')
if [ "$AFTER_SHA" != "$PRISTINE_SHA" ]; then
  echo "FAIL: the pristine header changed during the run" >&2
  exit 1
fi

echo "wifi_attempt: ALL PASS (pristine header untouched)"

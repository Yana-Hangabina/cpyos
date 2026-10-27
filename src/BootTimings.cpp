#include "BootTimings.h"

#include <Arduino.h>
#include <Logging.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace boottimings {
namespace {

constexpr uint32_t RECORD_MAGIC = 0x424F4F54u;  // "BOOT"
constexpr uint16_t NOT_REACHED = 0xFFFF;
constexpr size_t kStageCount = static_cast<size_t>(Stage::Count);

struct Record {
  uint32_t magic;
  uint16_t stageMs[kStageCount];
  uint8_t finished;
};

// RTC memory survives a panic or watchdog reset and deep sleep, but is
// uninitialized after a power cut, so the magic is the only proof a slot was
// written (same discipline as lib/Logging and lib/hal/HalSystem).
RTC_NOINIT_ATTR Record currentRecord;
RTC_NOINIT_ATTR Record previousRecord;

const char* const kStageNames[kStageCount] = {"boot", "board", "sd", "stores", "panel", "splash", "frame"};

bool hasMagic(const Record& record) { return record.magic == RECORD_MAGIC; }

void reset(Record& record) {
  memset(&record, 0, sizeof(record));
  record.magic = RECORD_MAGIC;
  for (size_t i = 0; i < kStageCount; i++) {
    record.stageMs[i] = NOT_REACHED;
  }
}

// Highest stage with a timestamp. Stages can be skipped (a splashless wake never
// marks Splash), so this must not stop at the first gap.
size_t lastReached(const Record& record) {
  size_t last = 0;
  for (size_t i = 0; i < kStageCount; i++) {
    if (record.stageMs[i] != NOT_REACHED) last = i;
  }
  return last;
}

void append(char* out, const size_t cap, size_t& used, const char* format, ...) {
  if (used + 1 >= cap) return;
  va_list args;
  va_start(args, format);
  const int written = vsnprintf(out + used, cap - used, format, args);
  va_end(args);
  if (written < 0) return;
  used += static_cast<size_t>(written);
  if (used >= cap) used = cap - 1;  // keep room for the terminator
}

size_t formatTrail(const Record& record, char* out, const size_t cap) {
  if (cap == 0) return 0;
  out[0] = '\0';
  if (!hasMagic(record)) return 0;
  size_t used = 0;
  const size_t last = lastReached(record);
  append(out, cap, used, "boot %ums:", static_cast<unsigned>(record.stageMs[last]));
  for (size_t i = 0; i <= last; i++) {
    if (record.stageMs[i] == NOT_REACHED) continue;
    append(out, cap, used, "%s%s %u", used ? " " : "", kStageNames[i], static_cast<unsigned>(record.stageMs[i]));
  }
  return used;
}

}  // namespace

void begin() {
  if (hasMagic(currentRecord) && !currentRecord.finished) {
    previousRecord = currentRecord;  // a boot that never painted: keep its trail
  } else if (hasMagic(previousRecord)) {
    memset(&previousRecord, 0, sizeof(previousRecord));  // the trail is one boot old
  }
  reset(currentRecord);
  mark(Stage::BootStart);
}

void mark(const Stage stage) {
  const size_t index = static_cast<size_t>(stage);
  if (index >= kStageCount) return;
  if (!hasMagic(currentRecord) || currentRecord.finished) return;
  if (currentRecord.stageMs[index] != NOT_REACHED) return;  // first arrival wins
  const unsigned long elapsed = millis();
  currentRecord.stageMs[index] = elapsed >= NOT_REACHED ? NOT_REACHED - 1 : static_cast<uint16_t>(elapsed);
}

void markFirstPaint() {
  if (currentRecord.finished) return;
  mark(Stage::FirstPaint);
  currentRecord.finished = 1;

  char trail[128];
  if (formatTrail(currentRecord, trail, sizeof(trail)) > 0) {
    LOG_INF("BOOT", "%s", trail);
  }
}

bool hasIncompletePrevious() { return hasMagic(previousRecord) && !previousRecord.finished; }

size_t incompletePrevious(char* out, const size_t cap) {
  if (!hasIncompletePrevious()) {
    if (cap > 0) out[0] = '\0';
    return 0;
  }
  const size_t last = lastReached(previousRecord);
  size_t used = 0;
  out[0] = '\0';
  append(out, cap, used, "%s %ums", kStageNames[last], static_cast<unsigned>(previousRecord.stageMs[last]));
  return used;
}

}  // namespace boottimings

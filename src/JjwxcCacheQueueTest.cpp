#include "activities/apps/jjwxc/JjwxcCacheQueue.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace JjwxcCacheQueue;

static Item makeItem(const char* id, const char* title, uint32_t from, uint32_t to, uint32_t anchor, bool valid) {
  Item it;
  snprintf(it.novelId, sizeof(it.novelId), "%s", id);
  snprintf(it.title, sizeof(it.title), "%s", title);
  it.fromOrdinal = from;
  it.toOrdinal = to;
  it.anchorOrdinal = anchor;
  it.setAnchorValid(valid);
  it.state = static_cast<uint8_t>(State::Queued);
  it.totalChapters = to - from + 1;
  return it;
}

int main() {
  // Empty image.
  uint8_t image[kMaxImageSize] = {};
  assert(encode(nullptr, 0, image, sizeof(image)) == kHeaderSize + kTrailerSize);
  {
    Item out[kMaxItems];
    size_t count = 99;
    assert(decode(image, kHeaderSize + kTrailerSize, out, kMaxItems, count));
    assert(count == 0);
  }

  // Round trip, long title and error, anchor flag.
  Item items[kMaxItems];
  items[0] = makeItem("4465668", "a very long novel title that must be truncated safely in the fixed record", 0, 186, 126, true);
  items[0].doneChapters = 12;
  items[0].doneParagraphs = 34;
  items[0].totalParagraphs = 56;
  items[0].requests = 57;
  snprintf(items[0].lastError, sizeof(items[0].lastError), "wifi");
  items[0].state = static_cast<uint8_t>(State::NeedsWifi);
  for (size_t i = 1; i < kMaxItems; ++i) items[i] = makeItem("1", "book", 10, 19, 15, false);
  const size_t bytes = encode(items, kMaxItems, image, sizeof(image));
  assert(bytes == kHeaderSize + kMaxItems * kRecordSize + kTrailerSize);
  {
    Item out[kMaxItems];
    size_t count = 0;
    assert(decode(image, bytes, out, kMaxItems, count));
    assert(count == kMaxItems);
    assert(strcmp(out[0].novelId, "4465668") == 0);
    assert(out[0].fromOrdinal == 0 && out[0].toOrdinal == 186 && out[0].anchorOrdinal == 126);
    assert(out[0].anchorValid());
    assert(out[0].state == static_cast<uint8_t>(State::NeedsWifi));
    assert(out[0].doneChapters == 12 && out[0].doneParagraphs == 34 && out[0].requests == 57);
    assert(strcmp(out[0].lastError, "wifi") == 0);
    assert(!out[1].anchorValid());
  }
  // Buffer smaller than the file: report full count, copy only what fits.
  {
    Item out[2];
    size_t count = 0;
    assert(decode(image, bytes, out, 2, count));
    assert(count == kMaxItems);
  }
  // Corruptions are rejected.
  {
    Item out[kMaxItems];
    size_t count = 0;
    std::vector<uint8_t> bad(image, image + bytes);
    bad[0] ^= 0xFF;
    assert(!decode(bad.data(), bad.size(), out, kMaxItems, count));
    bad.assign(image, image + bytes);
    bad[4] = 9;  // version
    assert(!decode(bad.data(), bad.size(), out, kMaxItems, count));
    bad.assign(image, image + bytes);
    bad[kHeaderSize + kRecordSize * 2 + 3] ^= 0x40;  // payload CRC
    assert(!decode(bad.data(), bad.size(), out, kMaxItems, count));
    bad.assign(image, image + bytes - 1);  // truncated
    assert(!decode(bad.data(), bad.size(), out, kMaxItems, count));
  }
  // Ordering: after the reading anchor, wrap, then the current chapter last.
  {
    uint32_t expect[] = {6, 7, 8, 9, 0, 1, 2, 3, 4, 5};
    for (uint32_t i = 0; i < 10; ++i) assert(ordinalForStep(0, 9, 5, true, i) == expect[i]);
    for (uint32_t i = 0; i < 10; ++i) assert(ordinalForStep(0, 9, 0, false, i) == i);
    expect[0] = 16; expect[1] = 17; expect[2] = 18; expect[3] = 19;
    expect[4] = 10; expect[5] = 11; expect[6] = 12; expect[7] = 13; expect[8] = 14; expect[9] = 15;
    for (uint32_t i = 0; i < 10; ++i) assert(ordinalForStep(10, 19, 15, true, i) == expect[i]);
    for (uint32_t i = 0; i < 10; ++i) assert(ordinalForStep(10, 19, 3, true, i) == 10 + i);
  }
  puts("JjwxcCacheQueueTest OK");
  return 0;
}

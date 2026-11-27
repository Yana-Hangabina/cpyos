#include "InvokeCallback.h"

#include <FaultCodes.h>
#include <FaultLog.h>

#include <Logging.h>

namespace callbacks {
namespace {

// A fixed table rather than a set: the tags are compile-time literals at known
// call sites, and this runs on the render task where an allocation would be the
// very thing that aborts the device.
const char* gTags[kKnownTags] = {};
volatile size_t gCount = 0;

// Index of `tag` in the table, inserting it when unseen. `first` says whether this
// call is the site's debut, which is what gates the log line. Pointer identity,
// not strcmp: the same literal at the same site is the same address, and no
// comparison may touch memory that could be freed under us.
//
// A site beyond the table capacity gets kUnknownSite and still counts as "first":
// losing the log line for an unnumbered site would hide a real defect, and the
// repeated line is bounded by the number of distinct tags in the tree (sixteen
// covers every site the gate knows about).
size_t claim(const char* tag, bool& first) {
  const size_t seen = gCount;
  for (size_t i = 0; i < seen; ++i) {
    if (gTags[i] == tag) {
      first = false;
      return i;
    }
  }
  first = true;
  if (seen < kKnownTags) {
    gTags[seen] = tag;
    gCount = seen + 1;
    return seen;
  }
  return kUnknownSite;
}

}  // namespace

bool noteEmptyOnce(const char* tag) {
  bool first = false;
  (void)claim(tag, first);
  // Two tasks racing on the same new tag may both report once. That is a
  // duplicate log line, never a missed one, which is the right direction.
  return first;
}

void noteEmpty(const char* tag) {
  bool first = false;
  const size_t slot = claim(tag, first);
  // Recorded on every skip, not once per boot: the ring counts occurrences, so a
  // callback that is missing on every frame reads `CB-01x400` -- which says both
  // that it is happening and how much of the UI it is affecting -- instead of the
  // single serial line a screenshot cannot contain. `detail` is the site index, so
  // crash_report.txt can name the sites that were skipped.
  fault::raise(fault::Code::CallbackEmpty, static_cast<uint32_t>(slot));
  if (slot == kUnknownSite) fault::raise(fault::Code::CallbackTableFull);
  if (first) reportEmpty(tag);
}

const char* siteTagAt(const size_t index) { return index < gCount ? gTags[index] : ""; }

void reportEmpty(const char* tag) { LOG_ERR("CB", "empty callback skipped: %s", tag); }

}  // namespace callbacks

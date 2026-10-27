// WAP discovery pages: channel / ranking / free / completed / paged category
// lists, and the advanced-search result table.
//
// These pages are all server-rendered HTML with one row per book, but they come
// in two markup families and the differences are easy to get wrong:
//
//   - Channel, ranking, free and category pages use `<li>` rows. Some rows carry
//     TWO anchors to the same novel: a bracketed category label first (the gold
//     ranking pages do this) or a `<span class="novelname">` inside an activity
//     sentence ("<book> received a rocket"). Taking the first anchor would name
//     the book "[幻想现言]" or the whole sentence.
//   - Advanced-search results (/assort?...) use a `<tr>` table instead. A page
//     can contain both: the navigation chrome is `<li>`-based even on the table
//     page. Choosing rows by "does this block contain a /book2/ anchor" and
//     preferring `<li>` only when one of them does keeps both families working
//     without a per-page rule.
//
// The input must already be UTF-8 (the app layer transcodes GB18030) and may be
// truncated at `len`; nothing here assumes NUL termination.
//
// No heap, no STL, no Arduino/ESP headers: fixed stack arrays only, so this
// builds and runs on the host and costs the firmware no internal RAM.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace jjwxc {

struct DiscoverEntry {
  char novelId[16] = {};
  char novelName[96] = {};
  // Empty when the row carries no /wapauthor/ link (channel lists often do not).
  char authorName[48] = {};
};

using DiscoverSink = bool (*)(void* ctx, const DiscoverEntry& entry, size_t index);

// What the page's pager said. `nextUrl` is the exact href of the link that goes
// to `current + 1` -- not a URL this file built. Queries differ between pages
// (`?type=dp`, `&isfinish=...`), so copying the link the server rendered is the
// only way not to drop a parameter. It may be absolute or start with '/'.
struct DiscoverPage {
  uint32_t current = 1;
  uint32_t max = 0;
  char nextUrl[192] = {};
};

// Parses one list document. Returns the number of entries handed to `sink`.
//
// `requestedPage` is the 1-based page the caller asked the server for; it seeds
// the pager. `pageOut` may be null. Returning 0 can legitimately mean "this page
// has no books", so callers must not treat it as an error on its own.
size_t parseDiscoverList(const char* html, size_t len, DiscoverSink sink, void* ctx, DiscoverPage* pageOut = nullptr,
                         uint32_t requestedPage = 1);

// One entry of /assort's tag picker. The page's form posts numeric tag ids
// (`bq=39,124`), so a UI that lets the user type "甜文" has to resolve the name
// through this map first. 279 tags are on the page today; the caller's sink is
// expected to stop early when its buffer is full.
struct AssortTag {
  char id[12] = {};
  char name[32] = {};
};

using AssortTagSink = bool (*)(void* ctx, const AssortTag& tag, size_t index);

size_t parseAssortTags(const char* html, size_t len, AssortTagSink sink, void* ctx);

}  // namespace jjwxc

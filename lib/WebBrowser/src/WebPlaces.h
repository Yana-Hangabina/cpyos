#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "WebTypes.h"

namespace webbrowser {

// One saved place: a bookmark or a history entry. Stored as text so a card can
// be inspected and repaired by hand; fields are tab-separated and sanitised on
// write (titles from HTML can contain tabs and newlines).
struct Place {
  WebString url;
  WebString title;
  uint32_t visitedAt = 0;  // epoch seconds; 0 when the clock was never set
  uint16_t page = 0;       // bookmark page index; history uses 0
};

constexpr size_t kMaxBookmarks = 50;
constexpr size_t kMaxHistory = 50;
constexpr size_t kMaxPlaceUrlBytes = 512;
constexpr size_t kMaxPlaceTitleBytes = 256;

// "WPL1\t1" header, one entry per line: url \t title \t visitedAt \t page.
WebString serializePlaces(const WebVector<Place>& places);
bool deserializePlaces(std::string_view text, WebVector<Place>& out);

// Bookmarks are unique by URL; adding an existing URL refreshes its title and
// page and moves it to the front. The list is capped at kMaxBookmarks.
void addBookmark(WebVector<Place>& bookmarks, const Place& place);
bool removeBookmark(WebVector<Place>& bookmarks, std::string_view url);
int findBookmark(const WebVector<Place>& bookmarks, std::string_view url);
void toggleBookmark(WebVector<Place>& bookmarks, const Place& place);

// History is MRU-ordered, unique by URL, capped at kMaxHistory.
void recordHistory(WebVector<Place>& history, const Place& place);

void clearPlaces(WebVector<Place>& places);

}  // namespace webbrowser

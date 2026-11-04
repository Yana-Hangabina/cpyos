#include "WebPlaces.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace webbrowser {
namespace {

constexpr char kHeader[] = "WPL1\t1";
constexpr size_t kMaxSerializedBytes = 64 * 1024;

WebString sanitize(std::string_view value, const size_t cap) {
  size_t length = std::min(value.size(), cap);
  // Do not split a UTF-8 sequence at the cap.
  while (length > 0 && (static_cast<unsigned char>(value[length]) & 0xC0) == 0x80) --length;
  WebString out;
  out.reserve(length);
  for (size_t i = 0; i < length; ++i) {
    const char c = value[i];
    out.push_back((c == '\t' || c == '\r' || c == '\n') ? ' ' : c);
  }
  return out;
}

std::string_view viewOf(const WebString& text) { return std::string_view(text.data(), text.size()); }

}  // namespace

WebString serializePlaces(const WebVector<Place>& places) {
  WebString out;
  out.append(kHeader);
  out.push_back('\n');
  for (const auto& place : places) {
    if (place.url.empty()) continue;
    // Sanitise on write as well: a title coming from HTML may contain tabs or
    // newlines, which would corrupt the line format on the next parse.
    out.append(sanitize(viewOf(place.url), kMaxPlaceUrlBytes));
    out.push_back('\t');
    out.append(sanitize(viewOf(place.title), kMaxPlaceTitleBytes));
    out.push_back('\t');
    char number[24];
    snprintf(number, sizeof(number), "%lu\t%u", static_cast<unsigned long>(place.visitedAt),
             static_cast<unsigned>(place.page));
    out.append(number);
    out.push_back('\n');
    if (out.size() >= kMaxSerializedBytes) break;
  }
  return out;
}

bool deserializePlaces(std::string_view text, WebVector<Place>& out) {
  out.clear();
  if (text.empty()) return true;
  size_t pos = text.find('\n');
  if (pos == std::string_view::npos) return false;
  const std::string_view header = text.substr(0, pos);
  if (header != kHeader) return false;
  ++pos;

  while (pos < text.size() && out.size() < kMaxHistory) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    const std::string_view line = text.substr(pos, end - pos);
    pos = end + 1;
    if (line.empty()) continue;

    const size_t tab1 = line.find('\t');
    const size_t tab2 = tab1 == std::string_view::npos ? tab1 : line.find('\t', tab1 + 1);
    const size_t tab3 = tab2 == std::string_view::npos ? tab2 : line.find('\t', tab2 + 1);
    if (tab3 == std::string_view::npos) continue;

    Place place;
    place.url = sanitize(line.substr(0, tab1), kMaxPlaceUrlBytes);
    place.title = sanitize(line.substr(tab1 + 1, tab2 - tab1 - 1), kMaxPlaceTitleBytes);
    place.visitedAt = static_cast<uint32_t>(strtoul(std::string(line.substr(tab2 + 1, tab3 - tab2 - 1)).c_str(),
                                                    nullptr, 10));
    const unsigned page = static_cast<unsigned>(strtoul(std::string(line.substr(tab3 + 1)).c_str(), nullptr, 10));
    place.page = static_cast<uint16_t>(std::min(page, 0xFFFFu));
    if (place.url.empty()) continue;
    out.push_back(std::move(place));
  }
  return true;
}

int findBookmark(const WebVector<Place>& bookmarks, const std::string_view url) {
  for (size_t i = 0; i < bookmarks.size(); ++i) {
    if (viewOf(bookmarks[i].url) == url) return static_cast<int>(i);
  }
  return -1;
}

void addBookmark(WebVector<Place>& bookmarks, const Place& place) {
  if (place.url.empty()) return;
  const int existing = findBookmark(bookmarks, viewOf(place.url));
  if (existing >= 0) bookmarks.erase(bookmarks.begin() + existing);
  bookmarks.insert(bookmarks.begin(), place);
  if (bookmarks.size() > kMaxBookmarks) bookmarks.resize(kMaxBookmarks);
}

bool removeBookmark(WebVector<Place>& bookmarks, const std::string_view url) {
  const int existing = findBookmark(bookmarks, url);
  if (existing < 0) return false;
  bookmarks.erase(bookmarks.begin() + existing);
  return true;
}

void toggleBookmark(WebVector<Place>& bookmarks, const Place& place) {
  if (findBookmark(bookmarks, viewOf(place.url)) >= 0) {
    removeBookmark(bookmarks, viewOf(place.url));
  } else {
    addBookmark(bookmarks, place);
  }
}

void recordHistory(WebVector<Place>& history, const Place& place) {
  if (place.url.empty()) return;
  const int existing = findBookmark(history, viewOf(place.url));
  if (existing >= 0) history.erase(history.begin() + existing);
  history.insert(history.begin(), place);
  if (history.size() > kMaxHistory) history.resize(kMaxHistory);
}

void clearPlaces(WebVector<Place>& places) { places.clear(); }

}  // namespace webbrowser

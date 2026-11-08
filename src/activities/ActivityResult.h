#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

#include "booksource/BookSource.h"

struct WifiResult {
  bool connected = false;
  std::string ssid;
  std::string ip;
};

struct KeyboardResult {
  std::string text;
};

struct MenuResult {
  int action = -1;
  uint8_t orientation = 0;
  uint8_t pageTurnRate = 0;
};

struct ChapterResult {
  int spineIndex = 0;
  std::string anchor;
};

struct PercentResult {
  int percent = 0;
};

struct IntervalResult {
  int32_t value = 0;
};

struct ChapterRangeResult {
  uint32_t first = 0;
  uint32_t last = 0;
};

struct PageResult {
  uint32_t page = 0;
};

struct TxtOffsetResult {
  uint32_t sourceOffset = 0;
};

struct ProgressChangeResult {
  int spineIndex = 0;
  int page = 0;
  int totalPages = 0;
  std::string xpath;
  float percentage = 0.0f;
  bool hasSavedProgress = false;
  // Exact visible-codepoint offset within spineIndex, when the source (a bookmark) has one.
  // Preferred over xpath/percentage on resolution: it is immune to re-pagination.
  bool hasVisibleTextOffset = false;
  uint32_t visibleTextOffset = 0;
};

enum class NetworkMode;

struct NetworkModeResult {
  NetworkMode mode;
};

struct FootnoteResult {
  std::string href;
};

struct FilePathResult {
  std::string path;
};

// Word picked inside a definition by DictionaryWordSelectActivity's
// select-only mode. The dictionary viewer looks it up itself, so the nested
// lookup stays inside one activity and the back stack owns its history.
struct DictionarySelectionResult {
  std::string word;
};

// A phrase lookup ran out of candidates (upstream c201af43). The reader reopens
// the word picker at the same page so the user can choose a word by hand; the
// phrase screen has no page of its own to show a picker over.
struct DictionaryPickerRequest {};

// A book source's app returns booksource::AppResult when it finished a
// maintenance action on its own (see booksource/BookSource.h).

using ResultVariant = std::variant<std::monostate, WifiResult, KeyboardResult, MenuResult, ChapterResult, PercentResult,
                                   IntervalResult, ChapterRangeResult, PageResult, TxtOffsetResult,
                                   ProgressChangeResult, NetworkModeResult, FootnoteResult, FilePathResult,
                                   DictionarySelectionResult, DictionaryPickerRequest, booksource::AppResult>;

struct ActivityResult {
  bool isCancelled = false;
  ResultVariant data;

  explicit ActivityResult() = default;

  template <typename ResultType>
    requires std::is_constructible_v<ResultVariant, ResultType&&>
  // cppcheck-suppress noExplicitConstructor
  ActivityResult(ResultType&& result) : data{std::forward<ResultType>(result)} {}
};

using ActivityResultHandler = std::function<void(const ActivityResult&)>;

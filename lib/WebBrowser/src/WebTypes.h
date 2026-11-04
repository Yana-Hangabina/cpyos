#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

// Container types for the browser's document model.
//
// A fetched page is decoded into one text buffer plus a block/link index; a
// 200 KB article must not land in internal RAM. On Read Pico every container
// here therefore allocates from PSRAM, and a failed allocation aborts rather
// than silently falling back to internal RAM -- the JjwxcPsramAllocator header
// documents what four rounds of misdiagnosis that silent fallback cost.
#if defined(BOARD_HAS_PSRAM) && defined(__has_include)
#if __has_include(<esp_heap_caps.h>)
#include <esp_heap_caps.h>
#define WEBBROWSER_ALLOCATOR_HAS_HEAP_CAPS 1
#endif
#endif

namespace webbrowser {

#if defined(BOARD_HAS_PSRAM) && !defined(WEBBROWSER_ALLOCATOR_HAS_HEAP_CAPS)
// Same rule as JjwxcPsramAllocator: a PSRAM board that cannot reach
// esp_heap_caps.h would silently put the whole document in internal RAM.
#error "BOARD_HAS_PSRAM is set but <esp_heap_caps.h> was not found; WebAllocator would use internal RAM"
#endif

#ifdef WEBBROWSER_ALLOCATOR_HAS_HEAP_CAPS
template <typename T>
struct WebAllocator {
  using value_type = T;
  using propagate_on_container_move_assignment = std::true_type;
  using is_always_equal = std::true_type;

  WebAllocator() noexcept = default;
  template <typename U>
  WebAllocator(const WebAllocator<U>&) noexcept {}

  T* allocate(const std::size_t n) {
    void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
      // -fno-exceptions: there is no way to report this and keep going. The
      // caller's own OOM checks (page size caps) are the real guard.
      abort();
    }
    return static_cast<T*>(p);
  }
  void deallocate(T* p, std::size_t) noexcept { heap_caps_free(p); }

  template <typename U>
  bool operator==(const WebAllocator<U>&) const noexcept {
    return true;
  }
  template <typename U>
  bool operator!=(const WebAllocator<U>&) const noexcept {
    return false;
  }
};
#else
template <typename T>
using WebAllocator = std::allocator<T>;
#endif

using WebString = std::basic_string<char, std::char_traits<char>, WebAllocator<char>>;
template <typename T>
using WebVector = std::vector<T, WebAllocator<T>>;

enum class BlockKind : uint8_t {
  Paragraph,
  Heading,
  ListItem,
  Quote,
  Pre,
};

// A run of text in Document::text. Blocks carry their own kind so the pager can
// apply heading spacing and weight without re-parsing the source.
struct Block {
  BlockKind kind = BlockKind::Paragraph;
  uint8_t level = 0;  // Heading: 1..6
  uint32_t textStart = 0;
  uint32_t textLength = 0;
};

// An anchor's visible text (range in Document::text) and its resolved absolute
// URL (range in Document::hrefs). Links are recorded only for http(s) targets;
// javascript:/mailto:/in-page anchors are dropped.
struct Link {
  uint32_t textStart = 0;
  uint32_t textLength = 0;
  uint32_t hrefStart = 0;
  uint32_t hrefLength = 0;
};

struct Document {
  WebString text;
  WebString hrefs;
  WebString title;
  WebString url;
  WebVector<Block> blocks;
  WebVector<Link> links;
  bool truncated = false;

  void clear() {
    text.clear();
    hrefs.clear();
    title.clear();
    url.clear();
    blocks.clear();
    links.clear();
    truncated = false;
  }
};

}  // namespace webbrowser

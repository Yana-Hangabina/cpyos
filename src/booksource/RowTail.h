#pragma once

// Pure mapping from a source's persisted chapter state to the reader's
// Contents row-tail chrome. Kept out of the activity so it can be host-tested
// and so a change in the state numbering cannot silently change the icons.

#include <cstdint>

#include "booksource/BookSource.h"

namespace booksource {

inline RowTail rowTailFor(const uint8_t state, const bool isVip) {
  switch (static_cast<TocState>(state)) {
    case TocState::Cached:
      return {isVip ? RowBadge::Vip : RowBadge::Free, RowAction::Refresh};
    case TocState::Missing:
      // Ownership is unknown here: the source's price page decides between
      // caching an owned chapter and asking for a purchase.
      return {isVip ? RowBadge::Vip : RowBadge::Free, RowAction::Download};
    case TocState::Unpurchased:
      return {RowBadge::VipYen, RowAction::None};
    case TocState::Locked:
    case TocState::AppOnly:
      return {RowBadge::Lock, RowAction::None};
  }
  return {};
}

}  // namespace booksource

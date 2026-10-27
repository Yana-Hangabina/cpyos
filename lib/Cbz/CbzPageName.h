#pragma once

#include <cctype>
#include <cstring>

// Natural ordering for archive page names: page2 sorts before page10, and
// page1 before page01. Pure and header-only so the host test exercises the
// same comparator the device uses.
namespace cbz_page {

namespace detail {
inline bool isDigit(const char c) { return c >= '0' && c <= '9'; }

inline int compareNumericRun(const char*& left, const char*& right) {
  const char* leftStart = left;
  const char* rightStart = right;
  while (isDigit(*left)) ++left;
  while (isDigit(*right)) ++right;
  const size_t leftLen = static_cast<size_t>(left - leftStart);
  const size_t rightLen = static_cast<size_t>(right - rightStart);

  const char* leftSignificant = leftStart;
  const char* rightSignificant = rightStart;
  while (leftSignificant < left && *leftSignificant == '0') ++leftSignificant;
  while (rightSignificant < right && *rightSignificant == '0') ++rightSignificant;
  const size_t leftDigits = static_cast<size_t>(left - leftSignificant);
  const size_t rightDigits = static_cast<size_t>(right - rightSignificant);
  if (leftDigits != rightDigits) return leftDigits < rightDigits ? -1 : 1;
  const int cmp = std::memcmp(leftSignificant, rightSignificant, leftDigits);
  if (cmp != 0) return cmp < 0 ? -1 : 1;
  if (leftLen != rightLen) return leftLen < rightLen ? -1 : 1;
  return 0;
}
}  // namespace detail

inline int compareNatural(const char* left, const char* right) {
  while (*left != '\0' && *right != '\0') {
    if (detail::isDigit(*left) && detail::isDigit(*right)) {
      const int cmp = detail::compareNumericRun(left, right);
      if (cmp != 0) return cmp;
      continue;
    }
    const unsigned char l = static_cast<unsigned char>(*left);
    const unsigned char r = static_cast<unsigned char>(*right);
    const int cmp = std::tolower(l) - std::tolower(r);
    if (cmp != 0) return cmp < 0 ? -1 : 1;
    ++left;
    ++right;
  }
  if (*left == *right) return 0;
  return *left == '\0' ? -1 : 1;
}

}  // namespace cbz_page

#pragma once

#include <string>

// Convert an HTML fragment to plain text. Styling is ignored; block elements
// become line breaks and HTML entities are decoded. A heading becomes a label
// line of its own: `headingMarker` is written at the start of that line (the
// dictionary view draws a marked line in bold), and a '\0' marker leaves the
// line plain so non-dictionary callers see no extra byte.
std::string htmlToPlainText(const std::string& html, char headingMarker = '\0');

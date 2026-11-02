#pragma once
#include <string>

/**
 * Type of OPDS entry.
 */
enum class OpdsEntryType {
  NAVIGATION,  // Link to another catalog
  BOOK         // Downloadable book
};

/**
 * One row of an OPDS catalog, format-agnostic: both the Atom (1.x) parser and
 * the JSON (2.0) parser fill this in, and the browser activity maps it onto its
 * list rows and its download path.
 *
 * `summary` is only filled by the JSON parser (Atom feeds carry no summary in
 * this model). It is bounded by the parser and is used as the row subtitle when
 * the entry has no author, so a 2.0 catalog row still tells the reader
 * something.
 */
struct OpdsEntry {
  OpdsEntryType type = OpdsEntryType::NAVIGATION;
  std::string title;
  std::string author;  // Only for books
  std::string href;    // Navigation URL or epub download URL
  std::string id;
  std::string summary;  // Only for books (OPDS 2.0 metadata.description)

  // OPDS 2.0 marks DRM-wrapped distributions with an indirect acquisition
  // property on a direct acquisition link. Kept for logging / future use; the
  // download path currently fetches `href` as-is.
  bool indirectAcquisition = false;
};

// Legacy alias for backward compatibility
using OpdsBook = OpdsEntry;

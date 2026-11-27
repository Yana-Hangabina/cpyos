#pragma once

#include <cstdint>

// The decision half of publishing a downloaded file over an existing one.
//
// Three different replace strategies had grown in this tree -- Storage's
// remove-then-rename, WeRead's rename-via-.bak, and the PDF engine installer's
// in-place overwrite -- and the difference between them only shows up on the one
// card where FAT refuses the rename. These rules name every outcome the
// executor can reach, so the caller reports *what actually happened* instead of
// collapsing "cannot delete", "cannot rename" and "cannot write" into one
// storage error. That collapse is why the 0.8.2 font-pack failure was
// undiagnosable from the message alone.
//
// The executor lives in src/util/FilePublish.h; keep this header free of HAL and
// ESP includes so a host test can enumerate the table.
namespace file_publish {

enum class Outcome : uint8_t {
  // The destination was absent and the rename moved the file into place.
  Renamed,
  // The destination was removed first, then the rename moved the file in.
  Replaced,
  // No rename was possible (the destination would not go away, or the rename was
  // refused), so the destination was overwritten byte for byte. This is a
  // success, not a degraded one: the file the caller verified is now the file on
  // the card.
  CopiedOver,
  // Nothing to publish: the source is gone. Never reported as success.
  SourceMissing,
  // A destination exists that can neither be deleted nor overwritten. The card
  // entry is stuck (a damaged cluster chain behaves exactly like this); the
  // answer is a manual delete or a reformat, not a retry.
  DestinationStuck,
  // Neither rename nor copy could put the bytes in place, with no stuck
  // destination involved.
  CopyFailed,
};

// What the executor observed, in the order it acted.
struct Attempts {
  bool sourcePresent = false;
  bool destPresent = false;
  // Only meaningful when destPresent.
  bool removeOk = false;
  // Only meaningful when there was something to rename.
  bool renameOk = false;
  // Only meaningful once the rename route failed.
  bool copyOk = false;
};

constexpr Outcome classify(const Attempts& a) {
  if (!a.sourcePresent) return Outcome::SourceMissing;
  if (a.renameOk) return a.destPresent ? Outcome::Replaced : Outcome::Renamed;
  if (a.copyOk) return Outcome::CopiedOver;
  return a.destPresent ? Outcome::DestinationStuck : Outcome::CopyFailed;
}

constexpr bool succeeded(const Outcome outcome) {
  return outcome == Outcome::Renamed || outcome == Outcome::Replaced || outcome == Outcome::CopiedOver;
}

// Stable, log-safe label. Never a user-facing string: those go through tr().
// Inline and here rather than in the executor so a host test can assert the
// labels without linking the storage layer.
inline const char* name(const Outcome outcome) {
  switch (outcome) {
    case Outcome::Renamed:
      return "renamed";
    case Outcome::Replaced:
      return "replaced";
    case Outcome::CopiedOver:
      return "copied-over";
    case Outcome::SourceMissing:
      return "source-missing";
    case Outcome::DestinationStuck:
      return "destination-stuck";
    case Outcome::CopyFailed:
      return "copy-failed";
  }
  return "unknown";
}

}  // namespace file_publish

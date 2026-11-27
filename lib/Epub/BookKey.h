#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Per-book content keys, stored next to the book as "<book>.key" and wrapped
// (AES-256-GCM) with a key derived from this device: a copied card does not open
// on another reader, and the authenticated header keeps the expiry from being
// edited. The plugin that fulfils a book derives its key (whatever the scheme)
// and hands it over through POST /api/book-key; the reader then needs no scheme
// knowledge.
//
// WHAT the identity is decides whether a full erase destroys the library:
//   * v2 (written now) derives the wrap key from the hardware id -- the eFuse MAC,
//     which no flashing tool can change. Re-flashing with "erase device", which
//     this project does routinely when partitions change, therefore keeps every
//     book readable.
//   * v1 (read only, migrated on first success) derives it from the NVS-stored
//     device secret. That secret is replaced by a full erase, which is exactly how
//     a library of v1 books became a row of "DRM-protected file" dialogs: the wrap
//     key no longer existed, so the content key could not be unwrapped.
// The tradeoff is stated where it matters: the MAC is not a secret (it is on the
// device and readable over USB), so the wrap keeps a *removed card* from opening
// elsewhere -- the same level as the cookie obfuscation -- rather than resisting
// someone holding the device.
namespace bookkey {

constexpr size_t KEY_LEN = 16;  // AES-128

// expiresAt: epoch seconds, 0 = no expiry.
bool write(const std::string& bookPath, const uint8_t (&key)[KEY_LEN], int64_t expiresAt);

enum class ReadStatus : uint8_t {
  Ok,
  Missing,        // no ".key" beside the book
  Unreadable,     // short file, bad magic, version this build does not know
  ForeignDevice,  // well-formed, but the wrap key of this device does not open it
};

// The reason matters to the reader: ForeignDevice is what an erase-install of a
// previous key scheme looks like, and only that case can be fixed by re-caching
// the book. Older callers keep using read().
ReadStatus readStatus(const std::string& bookPath, uint8_t (&key)[KEY_LEN], int64_t* expiresAt);

// False when the file is missing, not wrapped by this device, or tampered.
bool read(const std::string& bookPath, uint8_t (&key)[KEY_LEN], int64_t* expiresAt);

}  // namespace bookkey

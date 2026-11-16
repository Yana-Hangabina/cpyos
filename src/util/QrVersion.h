// Byte-mode QR version selection (ECC level L).
//
// QrUtils used to pick the version with four rough thresholds copied from a
// different capacity table ("version 4 holds ~114 bytes"). Version 4 at ECC-L
// holds 78 BYTES in byte mode; a 91-byte JJWXC login QR was silently encoded
// into a version-4 symbol that no scanner could read. Every payload in the gaps
// (79-114, 115-395, ...) produced an invalid code, and the library reports
// success rather than an error.
//
// This table is the standard byte-mode capacity for ECC-L, version 1..40.
// Pure and header-only so it can be pinned by a host test.
#pragma once

#include <stddef.h>

namespace QrUtils {

// Smallest version whose byte-mode capacity holds `len` at ECC-L. Payloads
// longer than version 40's 2953 bytes report 40: the caller truncates first,
// exactly as before.
constexpr int byteModeVersionFor(const size_t len) {
  constexpr int kCapacity[41] = {
      0,                                                       // unused
      17,   32,   53,   78,   106,  134,  154,  192,  230,   // v1..v9
      271,  321,  367,  425,  458,  520,  586,  644,  718,   // v10..v18
      792,  858,  929,  1003, 1091, 1171, 1273, 1367, 1465,  // v19..v27
      1528, 1628, 1732, 1840, 1952, 2068, 2188, 2303, 2431,  // v28..v36
      2563, 2699, 2809, 2953,                                 // v37..v40
  };
  for (int v = 1; v <= 40; ++v) {
    if (len <= static_cast<size_t>(kCapacity[v])) return v;
  }
  return 40;
}

}  // namespace QrUtils

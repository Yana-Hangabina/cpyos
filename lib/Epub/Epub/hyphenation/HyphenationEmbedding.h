#pragma once

// Decides where the Liang hyphenation tries come from: embedded in the image, or
// loaded from external storage at runtime.
//
// The eleven tries total 351,458 bytes of .rodata. On the Read Pico build the app
// partition was at 98.5% (about 93 KB free) before this, so carrying them is the
// single largest reclaimable block in the image. They cannot simply be dropped:
// hyphenation quality is asserted by test/hyphenation_eval against per-language F1
// thresholds, and the C3 build has no PSRAM to load a 206 KB German trie into.
//
// So the rule is about memory, not about taste:
//
//   - BOARD_HAS_PSRAM (Read Pico)  -> CROSSPOINT_HYPHENATION_FROM_STORAGE. The tries
//     ride in /hyphen.dat or in the data partition, and are read into PSRAM one
//     language at a time. Absent or corrupt data leaves the language with no
//     hyphenator, which is the nullptr path callers already handle.
//   - no PSRAM (C3) and SIMULATOR  -> CROSSPOINT_EMBED_HYPHENATION. The generated
//     headers are compiled in exactly as before, so nothing regresses on a board
//     that has nowhere to put an external copy.
//
// Either macro may also be defined by the build to force a mode. Host tests use
// that: test/hyphenation_eval defines CROSSPOINT_EMBED_HYPHENATION to keep its
// thresholds meaningful, and the data-format test in cpyos/src defines it too so it
// can compare the packed file against the embedded arrays byte for byte.
//
// Exactly one of the two is defined after this header. Code that needs to know
// should test CROSSPOINT_EMBED_HYPHENATION and treat the else branch as external.

#if !defined(CROSSPOINT_EMBED_HYPHENATION) && !defined(CROSSPOINT_HYPHENATION_FROM_STORAGE)
#if defined(BOARD_HAS_PSRAM)
#define CROSSPOINT_HYPHENATION_FROM_STORAGE 1
#else
#define CROSSPOINT_EMBED_HYPHENATION 1
#endif
#endif

#if defined(CROSSPOINT_EMBED_HYPHENATION) && defined(CROSSPOINT_HYPHENATION_FROM_STORAGE)
#error "define at most one of CROSSPOINT_EMBED_HYPHENATION / CROSSPOINT_HYPHENATION_FROM_STORAGE"
#endif

# Private book-source component

The JJWXC client is no longer part of the public source tree. The reader, the
library and the app shell talk to it through a small, source-agnostic interface
in `src/booksource/BookSource.h`; the implementation itself lives in a private
tree at `src/jjwxc/` that is **not** distributed with this repository.

This document is the contract between the public tree and that private tree, and
the checklist for keeping `repos/crossmux` (development) and `repos/cpyos-public`
(release) in sync.

## What is public

| Path | Role |
|---|---|
| `src/booksource/BookSource.h` | Generic value types (`TocState`, `UpdateGap`, `AppResult`, `RowTail`, …) and the `ISource` / `ReaderSupport` / `ReaderHost` interfaces |
| `src/booksource/BookSource.cpp` | The registry: resolves the component symbol when `CROSSPOINT_BOOKSOURCE_JJWXC` is defined, otherwise returns `nullptr` and every helper degrades |
| `src/booksource/RowTail.h` | Pure state → Contents row-tail mapping (host-tested) |
| `src/booksource/PsramAllocator.h` | PSRAM STL allocator used by the reader's source tables |
| `src/activities/reader/EpubReaderActivity.*` | Keeps the *declarations* of its source hooks and the generic page/Contents plumbing; the definitions live in the private tree |
| `lib/Epub/Epub/JjwxcCommentSidecar.h` | On-card comment-count format (POD + codec), shared by the reader's page cache |
| i18n keys (`STR_JJWXC_*`), icons, app ids | Kept public on purpose: only the implementation is private |

The reader does **not** include any private header. With no component linked its
hooks compile to inert stubs (`#if defined(BOARD_HAS_PSRAM) && !defined(CROSSPOINT_BOOKSOURCE_JJWXC)`
at the end of `EpubReaderActivity.cpp`), so the public firmware behaves exactly
like a build without the feature.

`src/main.cpp` still names a few private types, but only inside
`#if defined(CROSSPOINT_BOOKSOURCE_JJWXC)` blocks that implement serial debug
commands (`CMD:DETAIL`, `CMD:JJSEARCH`, `CMD:TLSPROBE`, `CMD:JJCACHE*`,
`CMD:PURCHASEDEMO`). Those are declarations and debug entry points, not
implementation, and they are compiled out of every public build.

## What is private

```
src/jjwxc/
  app/       JjwxcActivity + screens, store, HTTP transport, EPUB packager,
             cache service, reader support (JjwxcReaderMethods.cpp),
             TlsProbe, ... and JjwxcSource.cpp (the ISource adapter)
  protocol/  URL builders, streaming JSON, three-layer crypto, WAP decoding,
             font-watermark map, ...
  test/      JjwxcCacheQueueTest.cpp
```

`src/jjwxc/JjwxcSource.cpp` is the only symbol the public tree resolves:

```cpp
namespace booksource {
ISource* privateSource();   // defined there, referenced from BookSource.cpp
}
```

## Building

**Public target (this repository, CI):**

```bash
pio run -e readpico
```

`src/jjwxc` is gitignored and excluded by `build_src_filter`, and
`CROSSPOINT_BOOKSOURCE_JJWXC` is not defined. The result has no JJWXC app,
no JJWXC symbols and no network code for it.

**Full firmware (private tree present):**

1. Put the private tree at `src/jjwxc/` (copy, or a submodule of the private
   repository).
2. Add the component flags to the board env you build, e.g.

   ```ini
   build_flags =
     ${s3_ble_psram.build_flags}
     -DCROSSPOINT_BOOKSOURCE_JJWXC=1
     -Isrc/jjwxc/app
     -Isrc/jjwxc/protocol/src
   ```

   (the development tree `repos/crossmux` already has them in `[s3_ble_psram]`).

Without the flags the component compiles but is dead code; without the
directory the flags produce a link error — that is the intended signal.

## Verification

```bash
# Public/no-source target (must stay green in CI)
pio run -e readpico
# Contract of the boundary itself (no hardware needed)
g++ -std=c++20 -O1 -Wall -Wextra -fsanitize=address,undefined \
    -I repos/crossmux/src -o /tmp/t_booksource \
    cpyos/src/BookSourceContractTest.cpp \
    repos/crossmux/src/booksource/BookSource.cpp && /tmp/t_booksource
```

Expected: `37 checks, 0 failures`, and `nm firmware.elf | grep -c JjwxcActivity`
returns `0` for the public image.

## Keeping development and release in sync

- **`repos/crossmux` (`feat/jjwxc`)** is the development tree and the source of
  truth for both the public files and `src/jjwxc`.
- **`repos/cpyos-public` (`cpyos`)** commits only the public side. `src/jjwxc`
  is in `.gitignore` there; never `git add` it, never commit the private tree.
- When a change touches both sides, split it in two commits (or two change
  sets): the public interface/core change, and the private implementation
  change. The public side must build and pass `BookSourceContractTest` on its
  own.
- Porting dev → public: copy `src/booksource/*` and the core hunks; do not copy
  `src/jjwxc`. Porting public → dev is the same in reverse, then re-apply any
  private-side adaptation.
- The private repository should carry its own CI with a `readpico`-equivalent
  environment (the flags above) plus the JJWXC host tests. Public CI stays
  green either way.

### Invariants (do not regress)

1. No `#include` of a `src/jjwxc` header outside the guarded `main.cpp` debug
   blocks.
2. No JJWXC protocol/storage code in `src/booksource`; it holds types and the
   registry only.
3. `pio run -e readpico` must succeed with `src/jjwxc` absent.
4. `BookSourceContractTest` must stay green — it pins the persisted state
   numbering, the icon enum numbering and the inert no-component behaviour.
5. Any new reader hook goes through `booksource::ReaderHost` (or a declaration
   + stub pair) — never by including a private header.

## Concrete wiring (2026-11-04)

- Private repository: **https://github.com/Yana-Hangabina/cpyos-jjwxc** (private,
  branch `main`). Its tree is exactly the content of this repository's
  `src/jjwxc/`.
- The release workflow clones it with a **read-only deploy key** stored as
  `secrets.JJWXC_DEPLOY_KEY`, then builds `pio run -e readpico_jjwxc`. A missing
  secret or a failed clone fails the release on purpose.
- `pio run -e readpico` is the public/no-source target (the CI default), so a
  plain clone builds with no private checkout.
- Public history was rewritten on 2026-11-04 to remove `lib/JjwxcApi`,
  `src/activities/apps/jjwxc`, `src/TlsProbe.*` and
  `src/JjwxcCacheQueueTest.cpp` from every commit; the 0.1.0–0.2.3 tags and
  their releases were deleted. Unreachable objects can remain in GitHub's
  object storage and in the fork network until GitHub GC runs; only GitHub
  Support can force that purge.

## Release pipeline note

The public release workflow builds the public target, so its artifacts have no
JJWXC app. A release that ships the app must be built where `src/jjwxc` is
present (private CI or a locally prepared tree) with the flags above. Keep the
two artifact sets labelled so users are not surprised by the difference.

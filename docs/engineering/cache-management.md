# Cache Management & Invalidation

> Deep reference for [AGENTS.md](../../AGENTS.md). The SD-card cache trades flash
> for RAM/CPU. **Always bump the format version BEFORE changing a binary layout.**
> For the byte-level binary formats themselves, see
> [../file-formats.md](../file-formats.md) (canonical reference).

## Cache Structure on SD Card

**Location**: `.crosspoint/` directory on SD card root

**Structure**:
- EPUB: `.crosspoint/epub_<hash>/{book.bin, progress.bin, cover.bmp, thumb_<height>.bmp, sections/*.bin}`
- XTC: `.crosspoint/xtc_<hash>/{cover.bmp, thumb_<height>.bmp}`
- TXT: `.crosspoint/txt_<hash>/{index.bin, chapters.bin, progress.bin}`

`recent.json` stores the theme-neutral `thumb_[HEIGHT].bmp` path template for
EPUB and XTC entries. Home themes replace `[HEIGHT]` with their requested
thumbnail height. Lyra Carousel redirects only its in-memory `RecentBook` copy
to `cover.bmp`, generating that full cover when missing; switching themes must
not replace the persisted template.

When an EPUB has no `book.bin`, thumbnail generation first builds metadata but
continues to skip CSS. Inx resolves each target and compatibility thumbnail at
most once per Activity, displays the existing localized loading popup while it
generates the missing covers under one framebuffer loan, then performs one
final screen refresh. `Missing` is Activity-local, so leaving and reopening the
theme allows a failed cover to be retried.

On `BOARD_HAS_PSRAM` targets, Inx also keeps successfully validated thumbnail
BMP files in an Activity-local PSRAM cache. Entries are capped at 64 KB and the
cache at 512 KB; oversized files, allocation/read failures, and devices without
usable PSRAM continue through the same SD streaming path. The cache owns no new
on-disk format and is released when the Activity exits or its thumbnail height
changes.

All five Inx recent layouts select their thumbnail height through
`InxRecentActivity::setThumbnailHeight()`. S3 hardware builds with
`BOARD_HAS_PSRAM` use the actual drawn cover height (the center cover in Flow),
reducing rescaling after the existing 1-bit Atkinson conversion. C3 and
emulated builds retain `InxCoverGeometry::thumbnailHeightForCropFill()`.
The `thumb_<height>.bmp` names, conversion and fast-refresh policy are unchanged:
existing thumbnails remain valid when that height is requested, other heights
are generated on demand, and the legacy home-height fallback remains available.
Larger drawn covers still cannot recover detail absent from the source image.

Legacy TXT `index.bin` version 8 stores a partial or complete lazy page index, the
detected source encoding, and the paragraph-spacing mode. It is invalidated by
file-size, viewport, font, margin, alignment, or paragraph-spacing changes; see
[file-formats.md](../file-formats.md#txt-reader-cache) for its byte layout and
legacy-version invalidation behavior.

Historical EPUB section versions 72/73 and TXT index version 8 invalidated older complete and
partial pagination because missing glyphs now occupy an ascender-scaled outline
box. Reopening a book rebuilds pagination automatically; metadata, chapter
indexes and source-offset reading progress are retained.

TXT `chapters.bin` version 1 is an optional source-offset chapter index built
the first time the chapter list opens. It is independent of pagination and is
invalidated by file size, encoding, or chapter-index version.

TXT `progress.bin` stores the current source offset so a chapter jump can be
restored without extending the page index through all intervening text. Legacy
four-byte page-number records remain readable.

**Hash**: `std::hash<std::string>{}(filepath)` → Moving/renaming file = new hash = lost progress

## Cache Invalidation Rules

**Cache is automatically invalidated when**:
1. **File format version changes** (see [../file-formats.md](../file-formats.md))
   - `book.bin` version number incremented
   - `section.bin` version number incremented
2. **Render settings change**:
   - Font family or size (`SETTINGS.fontFamily`, `SETTINGS.fontSize`)
   - Line spacing (`SETTINGS.lineSpacing`)
   - Paragraph spacing (`SETTINGS.extraParagraphSpacing`)
   - Screen margins (`SETTINGS.screenMargin`)
3. **Viewport dimensions change**:
   - Screen orientation change
   - Display resolution change
4. **Book file modified**:
   - Moved, renamed, or content changed (new hash)

Malformed caches are treated as misses. Readers validate every POD/string read,
length and lookup-table boundary before publishing parsed state. A truncated or
oversized `book.bin` is closed and removed; the current call returns no entry,
and the next normal EPUB load rebuilds it. This is a validation change only and
does not alter the version-11 binary layout.

**Manual Cache Clear** (host-side, for development):
```bash
# Force full regeneration of every book's pagination:
#   DROPS HIGHLIGHTS, NOTES, BOOKMARKS AND READING POSITION.
# The device's own "Clear Reading Cache" does not -- see the next section.
rm -rf /path/to/sd/.crosspoint/

# Delete one book's cache
rm -rf /path/to/sd/.crosspoint/epub_<hash>/

# Keep progress, delete only rendered sections
rm -rf /path/to/sd/.crosspoint/epub_<hash>/sections/
```

## What a cache clear must keep

The per-book cache directory (`epub_<hash>`, `pdf_<hash>`, `txt_<hash>`, …) holds two
different kinds of file, and the readers used to treat them alike: every `clearCache()`
ran `Storage.removeDir()` on the directory, which also contains the reader's own data.
So 设置 → 清除阅读缓存 (which sweeps *every* book cache at once) and the per-book
"清除分页缓存" both destroyed highlights, notes, bookmarks and reading position, and
PDF lost them silently whenever the file's size or mtime changed.

`lib/BookCache/BookCachePrune.h` now owns the rule, and
`book_cache::pruneRegenerable()` executes it: enumerate the directory, delete every entry
except the reader's own files, and **keep the directory itself** (every `setupCacheDir()`
is mkdir-if-absent, so a surviving empty directory is not a state any reader misreads).

Kept: `annotations.bin`, `annotations.txt`, `bookmarks.bin`, `progress.bin`, and their
`.tmp`/`.bak`/`.part` siblings. Everything else in the directory is deleted.

The rule is an allowlist of *user data*, not a denylist of cache names, deliberately: a
new cache artifact defaulting to deletable costs one rebuild, whereas a new user file
defaulting to deletable would be a data-loss bug nobody notices until a reader loses their
highlights. Temp siblings are kept because `replaceFile()`/`ProgressFile` write
`<name>.tmp` and rename it over the real file, so after a power loss mid-save the temp
file can be the only copy of a reader's notes.

★ **The sweep runs until a pass removes nothing.** Deleting entries while enumerating a
directory is only safe if the iterator is *entry-positioned* (a freed dir entry is stepped
over, which is how SdFat walks a directory). If it is *index-positioned*, every entry after
a removal shifts down into the hole and a single pass silently clears about half the
directory — which is exactly what the first version of this code did, caught by the
in-memory filesystem in `test/book_cache_prune/stubs/`. One pass per halving, no allocation;
a 32-pass cap bounds a filesystem that reports a removal it did not perform.

Applied at all six drop sites: `Epub/Xtc/Cbz/Txt/Fb2::clearCache()`, the PDF and
generated-EPUB branches of `clearBookCache()`, `ClearCacheActivity::clearCache()`, and
`PdfReaderActivity::ensureCacheValid()`. Deliberate exceptions:

- **Deleting a book** still removes the whole directory. The book is gone, so its notes go
  with it; keeping them would strand data with nothing to attach to.
- `Fb2`'s pre-generation drop of its own directory, and `WeReadBrowse::clearAllCaches()`
  (`/.crosspoint/weread/browse-cache`), hold no reader data.
- `CMD:PDFCACHE` (dev-only serial hook) still rmtrees, on purpose, to test a cold rebuild.
- Sub-directory removals (`sections/`, `html/`) were always selective and are the pattern
  this follows — `Txt::invalidateCache()` already wiped exactly those two, leaving
  `progress.bin`, and the settings-page clear never got that treatment.

The reader's anchors survive a rebuild because they address the *book*, not the cache:
EPUB offsets index the extracted text, PDF re-matches the stored quote against the current
lines (`QuoteAnchor.h`) and simply stops drawing when the text is gone, and PDF/EPUB
progress is clamped to the spine/page count on load.

★ Changing this also changed the confirmation text
(`STR_CLEAR_CACHE_WARNING_2`), which used to promise "All reading progress will be lost!".
Any translation edit changes `I18N_CONTENT_SIGNATURE` (the FNV is over every language's
full string sequence, embedded languages included), so a firmware carrying it **must ship
the matching `i18n_pack.dat`**: otherwise the device rejects the whole pack and the 34
packed languages fall back to English. Key count is unchanged, so the failure mode is the
content signature, not the "built for a different number of keys" rejection.

Host regression: `sh test/book_cache_prune/run_book_cache_prune_test.sh` — the rule
(51 checks) and the sweep against an in-memory filesystem (80 checks), 12 mutations in
total, each pinned to its target by a marker comment the runner requires. The sweep suite
runs both iterator models, and the fake's `reset()` clears every knob: a sticky failure
that leaked between cases silently turned a transient-failure assertion into a
stuck-entry one.

Device check (one command, exercises the product path and SdFat's real cursor):
`CMD:PRUNE <book path>` →
`[ERR] CACHE: serial CMD:PRUNE ok=1 annotations 1->1 progress 1->1 meta 1->0 book 1->0`.
The two directions on that line are the whole rule: derived blobs go, reader data stays.

**When to Clear Cache**:
- EPUB parsing errors after code changes to `lib/Epub/`
- Corrupt rendering (missing text, wrong layout)
- Testing cache generation logic
- After modifying:
  - `lib/Epub/Epub/Section.cpp`
  - `lib/Epub/Epub/BookMetadataCache.cpp`
  - Render settings in `CrossPointSettings`

## Cache File Format Versioning

**Source**: `lib/Epub/Epub/Section.cpp`, `lib/Epub/Epub/BookMetadataCache.cpp`

**Current Versions** (as of [../file-formats.md](../file-formats.md)):
- `book.bin`: **Version 11** (metadata structure) — includes NFC-composed titles and ignores ambiguous EPUB guide text references while remaining above every version shipped by either lineage.
- `section.bin`: **Version 76/77**, complete; partial versions are **206/205**,
  derived as `0xFE - (completeVersion - 28)`.
  Unified Chinese firmware uses 77. The added `paragraphIndentSpaces` byte
  follows `firstLineIndent`, participates in cache validity and preserves the
  fork version sequence above the incoming upstream version.

**Version Increment Rules**:
1. **ALWAYS increment version** BEFORE changing binary structure
2. Version mismatch → Cache auto-invalidated and regenerated
3. Document format changes in [../file-formats.md](../file-formats.md)

WeRead caches invalidate independently through their own magic/version or a
versioned filename. Do not add a global generation that recursively clears the
cache during application startup. Chapter XHTML compatibility is paired with
its image-index magic, so a mismatch causes that chapter to be rebuilt when the
user caches the book. Manual cache clearing remains the fallback for disposable
legacy files.

The JJWXC app follows the same rule with its own magics (`JXT1` per-chapter
catalogue, `JXN1` per-novel metadata; see
[../file-formats.md](../file-formats.md#jjwxc-cache)). Invalidation is
per chapter: a record whose `remoteSize` no longer matches the server, or whose
XHTML file has disappeared, drops back to `Missing` the next time the catalogue
is fetched. There is no whole-book refresh flag, and an existing finished EPUB
never triggers a re-pull; refreshing is an explicit user action. Because the
finished EPUB keeps one fixed path while its contents grow, repackaging must
drop that book's `epub_<hash>` reader cache — the path-keyed `book.bin` and
`section.bin` carry no source size or modification time and would otherwise keep
serving the previous pagination. Reading progress is carried across that drop
only when the run purely appended chapters, and the app marks the library index
dirty afterwards so the shelf re-walks the card. The pre-EPUB TXT products under
`/.crosspoint/jjwxc/books/` are removed once, with their `txt_<hash>` caches,
and are not migrated.

**Example** (incrementing section format version):
```cpp
// lib/Epub/Epub/Section.cpp
static constexpr uint8_t SECTION_FILE_VERSION = 42;  // bump before any layout change

// Add new field to structure
struct PageLine {
  // ... existing fields ...
  uint16_t newField;  // New field added
};
```

## TXT/Markdown reflow migration

TXT and Markdown now use the EPUB reader and section format. Conversion is
streaming: two checked 8KiB buffers, plus one disk-backed chapter record; the
8KiB chapter-index workspace is released before conversion. The HTML and metadata
conversion markers are version 2. `txt-map.bin` version 1 records source byte to
visible codepoint runs (see `docs/file-formats.md`). Chapters are processed one at
a time and parser boundary IDs do not accumulate in the anchor table. The inverse
map resolves the page's visible offset to a source position, then binary-searches
the existing disk chapter index for menu/status/statistics titles. Chapter stepping
and scrubbing use the same pending-anchor path as the Contents panel; the one-spine
conversion never turns chapter navigation into a no-op.

Old `txt_<hash>/progress.bin` remains untouched until and after successful migration.
Eight-byte source offsets map to their containing reflowed page. Four-byte old page
numbers use their old version 4–8 offset index. A missing/corrupt conversion or
unresolved page fails the open with an error and never overwrites progress at page 0.
New reflow progress lives in the EPUB cache. Both cache directories relocate with
TXT/Markdown when the file is renamed. Explicit paragraph indentation remains
Auto/Indent/NoIndent; forced Western width uses 0–5 spaces (default 3), Chinese
remains two characters, and CSS/paragraph spacing retain their independent rules.

## Read Pico idle image warming

Read Pico's existing 400 ms reader idle hook can warm the next image page's
`.pxc` files under the render lock. It retains the text cache's eligibility
checks (including display capabilities, anti-aliasing, normal background and
non-inverted rendering) and its two text-page slots. Image pages use only the
forward slot's reusable framebuffer stash; they do not allocate result planes.
The live framebuffer is loaned as decoder scratch and restored together with
the render mode on every exit. `freePageCache()` releases the stash on exit.

`CancelCheck` is a non-owning function pointer plus context. Its context must
outlive the synchronous operation; no worker task or concurrent decoder is
created. The idle generation is checked between images, extraction and decode,
at ZIP transfer boundaries, each PNG row and each JPEG output block. Prewarm
transfers are capped at 16 KiB. Decoder return values alone do not indicate a
successful operation: JPEGDEC may return success after its callback stops it.
Cancellation suppresses finalization and removes partial extraction/cache files.
Optional prewarm failures do not poison the foreground image failure list.
The `.pxc` layout, grayscale, scaling and transparency remain unchanged.

### Physical acceptance (required before enabling beyond this Draft)

Use the same device, SD card, books and reader settings for the baseline,
PSRAM I/O-only build and prewarm build. Record exact firmware SHAs and book/SD
identity. For PNG, JPEG, mixed and text-only EPUBs, capture at least 30 page
turns per build and cache state; report median and P95 input-to-visible latency.
For cold runs, remove only the selected test book's extracted image/`.pxc`
caches; do not delete progress. Warm runs retain those caches. Distinguish a
warm `.pxc` from an idle-prewarmed page, and keep the 400 ms idle interval fixed.

Record minimum internal/PSRAM free bytes and largest blocks, and timestamp idle
cancellation and render-lock release. Stage 1 needs a measurable cold-image
improvement without a material text/mixed P95 regression. Stage 2 needs a
measurable prewarmed-turn improvement and cancellation-to-unlock P95 <=100 ms,
maximum <=250 ms with a healthy SD card. These are acceptance targets, not
latency guarantees established by host tests.

Exercise rapid paging, menu/exit during extraction and decoding, night mode and
reading-background changes. Confirm no hangs, damaged frames, partial `.pxc`
files, or sustained heap loss, and retry the interrupted image in the foreground.
Until these measurements pass, keep the follow-up PR Draft; other PSRAM boards
receive only the I/O optimization.

# 上游能力吸纳：详细设计汇编

本文是 [`2026-11-08-upstream-absorption-roadmap.md`](2026-11-08-upstream-absorption-roadmap.md) 的附录，
汇集七份逐项设计/重分类原文。判断标准：**只有冲突且没必要才拒绝**；其余给出无冲突吸纳或取长补短的路线。


---

# EPUB 排版引擎吸纳设计

# Absorbing upstream's 2.0.0 EPUB layout engine into CPYOS — conflict-aware design

Read-only research. Repo: `/Users/hongbin/Documents/projects/cpyos/.worktrees/upstream-sync`
(`work/upstream-sync`, HEAD `b39f3706`). Upstream tip inspected: `a6fd569f`.

Subject: `0305aaa6` *"feat: Improve epub rendering (#3891)"* (45 files, +4885/−500) plus the
in-range follow-ups that touch the same files:

| SHA | What it does | CPYOS status today |
|---|---|---|
| `0305aaa6` | the layout/rendering rewrite analysed here | absent |
| `1bd323dd` | CSS `font-weight/style: inherit` through the cascade | **adopted** (`CssParser.h:47 CSS_CACHE_VERSION=13`) |
| `8b2ba5e9` | NBSP / justified-space corrections | **adopted** (Section v86) |
| `8418166c` + `9a7e6b35` | page bottom alignment | **adopted** (Section v84, `lib/Epub/Epub/PageBottomAlign.h`) |
| `9c3fbdd1` + `ad53eb64` | ruby narrower-than-base, ruby overhang/adjacency | **adopted** (`TextBlock.cpp:197-215 RubyDrawInfo{spread,edge}`, `CjkBreaks.{h,cpp}`) |
| `3e2af487` | CJK break offsets without staging | **adopted** |
| `cb2a4c78` | hyphenation with drop caps | **partially** adopted — only the "start `lineWidth` at the ruby overhang" line was folded in (Section v87/88); the bounded-`maxLines` part was rejected (`docs/plans/2026-11-08-upstream-sync-audit.md:313`) |
| `072b6fc7` | line spacing as a percentage | **rejected** (CPYOS keeps 1.3/1.6) |

The audit row for `0305aaa6` (`docs/plans/2026-11-08-upstream-sync-audit.md:306-307`) is
**D_SKIP "structural, keep excluded"**. That verdict is about a *cherry-pick of the commit*, not
about the *capabilities*. This document designs a selective absorption: we take the
capabilities, keep CPYOS's model and cache numbering, and never import the upstream
`Section`/`Page`/`TextBlock` files wholesale.

CPYOS layout stack under discussion (line counts at this HEAD):

```
lib/Epub/Epub/css/CssParser.{h,cpp}            198 / 1191
lib/Epub/Epub/css/CssStyle.h                   285
lib/Epub/Epub/parsers/ChapterHtmlSlimParser.{h,cpp}
lib/Epub/Epub/ParsedText.{h,cpp}               1795
lib/Epub/Epub/Page.{h,cpp}                     397
lib/Epub/Epub/blocks/TextBlock.{h,cpp}         586
lib/Epub/Epub/blocks/BlockStyle.h
lib/Epub/Epub/Section.{h,cpp}                  SECTION_FILE_VERSION 88 (CJK) / 87
```

Upstream at `a6fd569f` for the same files: `CssParser.cpp` 1842 lines, `CssParser.h` 271,
`CssStyle.h` ~430, `ChapterHtmlSlimParser.cpp` ~3.7k. The gap is roughly +2.5k lines of layout
code, of which we want well under half.

---

## 1. Feature inventory

`upstream` = symbol/line in `0305aaa6` (or `a6fd569f` where the follow-up finalised it).
`CPYOS` = what exists at this HEAD. Verdict: **absent / partial / present**.

### 1.1 CSS cascade and selectors

| # | Capability | Upstream | CPYOS today | Verdict |
|---|---|---|---|---|
| A1 | Compound selector: `p.a.b`, `#id`, `p#id.a` | `CssParser::parseCompound`, stored key = element + `#id` + sorted classes (`a6fd569f lib/Epub/Epub/css/CssParser.cpp`, `KeyPieces`) | `CssParser::resolveStyle` (CssParser.cpp:872) tries only `tag`, `.cls`, `tag.cls`; explicit `// TODO: Support combinations of classes` at :882 and :890 | **absent** |
| A2 | Descendant / child combinators (`div.poem p`, `blockquote > p`) | `CssAncestor` + `CONTEXT_SEPARATOR` + prefix range scan in `resolveStyle` | header comment: "Not supported (silently ignored): Descendant/child selectors" (CssParser.h:23) | **absent** |
| A3 | Specificity ordering (ids > classes > elements; source order tiebreak) | `packSpecificity` + sort by `(specificity, selector offset)` | fixed 3-step cascade (tag < class < tag.class) | **absent** |
| A4 | `::first-letter` rules stored and resolvable | `FIRST_LETTER_MARKER`, `hasFirstLetterRules()`, `resolveStyle(..., firstLetter=true)` | none | **absent** |
| A5 | `font-size` (em / % / rem / `small|large|xx-large`) | `CssStyle::fontSize` (CssLength) + `tryInterpretFontSize` | no `font-size` in `CssStyle` at all; property is dropped | **absent** |
| A6 | `font-variant: small-caps` | `CssStyle::smallCaps` → `EpdFontFamily::SMALL_CAPS = 128` → `utf8SmallCapsUpper` + 3/4 scale in `drawText` | `EpdFontFamily::Style` has no bit 128 (`EpdFontFamily.h:13-22`), `Utf8` has no upper-mapping helper | **absent** |
| A7 | `white-space: pre / pre-wrap / break-spaces`, `<pre>` | `CssStyle::preserveWhitespace`, `effectivePreserveWhitespace`, forces `white-space` on `<pre>` | `trimAndNormalize` unconditionally collapses runs to one space (`ChapterHtmlSlimParser.cpp:61-86`); `<pre>` not in `BLOCK_TAGS` | **absent** |
| A8 | `page-break-before/after`, `break-before/after` | `CssStyle::pageBreakBefore/After`, `applyPendingPageBreak()` | none | **absent** |
| A9 | Borders per side (solid/double/dotted/dashed) + light background shade | `CssBorderSide`, `borderWidthsDefined/borderStylesDefined`, `parseBorderDeclaration`, `PageBorderBox` | no border parsing; text-indent/margin/padding only | **absent** |
| A10 | `float`, `initial-letter` (as drop-cap markers) | `floatLeft`, `initialLetter`, `dropCapLines()` | none | **absent** |

Note: CPYOS's CSS cache (bit-packed `defined` flags, `CssParser.cpp:167-185`) uses **19 of 32
bits**. Upstream's set needs exactly **31** (`CssStyle.h:106-118` upstream) — it fits, with zero
headroom left in the uint32.

### 1.2 Block flow / pagination

| # | Capability | Upstream | CPYOS today | Verdict |
|---|---|---|---|---|
| B1 | Orphan/widow control (paragraph first line not stranded; last line carried) | `linesToCarry` (a6fd569f `ChapterHtmlSlimParser.cpp:2941-2959`), `ParsedText::linesAfterCurrent()`, `MAX_CARRIED_LINES=6`, `recentLineOffsets[]` | no carry logic; `currentPageNextY` simply breaks | **absent** |
| B2 | Heading `keep-with-next` | `keepWithNextLines` + `keepWithNextCarry()` | absent | **absent** |
| B3 | Block-level `font-size` (headings, `.title {font-size:2em}`) | `BlockStyle::fontScale/fontId`, `applyBlockFontScale`, `prepareBlockFont`, `Renderer::resolveFontVariant` | single `spec.fontId` for the whole section (`ReaderRenderSpec.h:19`), `BlockStyle` has no font field | **absent** |
| B4 | Whole-block sizing heuristic for `<p><span class="big">…</span></p>` | `InlineSizeState inlineSize` (parser-only; cancels when text follows the span) | absent | **absent** |
| B5 | Table row buffering 160 words / 2 KB, CSS-auto column pre-measure, grid borders | `MAX_GRID_TABLE_ROW_WORDS=160`, `measureTableColumns()`, `planTableColumns()`, `tableBorder` | `MAX_GRID_TABLE_COLUMNS=4`, `MAX_GRID_TABLE_CELL_WORDS=32`, `MAX_GRID_TABLE_CELL_BYTES=512`; equal-width columns; no borders | **partial** (works only for tiny tables; wide tables fall back to stacked flow) |
| B6 | Paragraph start across `<br>` (`paragraphStartEmitted`) | separate from `firstLineConsumed` | `firstLinePending` / `paragraphStartPending` (`ParsedText.h:87-88`) — close, but conflated | **partial** |
| B7 | Bottom alignment interacting with carried lines | `9a7e6b35` excludes carried lines | `applyVerticalBottomAlign` shifts by element index (`ChapterHtmlSlimParser.cpp:2472-2500`) | **present, must be extended** |

### 1.3 Inline typography / rendering

| # | Capability | Upstream | CPYOS today | Verdict |
|---|---|---|---|---|
| C1 | Per-word font id runs | `TextBlock::wordFontData` (12 B font table + 1 B slot/word), `ParsedText::wordSizeSlots`, `SMALL_CAPS_SLOT_FLAG=0x80` | none; one font id per section | **absent** |
| C2 | Baseline sharing across mixed sizes | `lineAscent()`, `wordYOffset()`, `extraAscent()` | n/a | **absent** |
| C3 | Sized font variants (resolve + lazy reload + LRU eviction) | `FontVariantProvider.h`, `GfxRenderer::ensureFontLoaded/resolveFontVariant`, `SdCardFontSystem::resolveVariant/loadVariant`, `MAX_*_VARIANTS = MAX_WORD_FONTS+2`, `ReaderFontSizes::builtinReaderFontVariant` | `GfxRenderer` has `insertFont`/`fontMap` (`GfxRenderer.h:148,259`) and `rebindFamily` but **no** variant provider; the *app* resolves one id (`CrossPointSettings::getReaderFontId()`, `CrossPointSettings.cpp:1050`) | **absent in the renderer; partially available at app level** (see §3.4) |
| C4 | Drop caps drawn by scaling one glyph (bilinear, `scale256`) | `PageDropCap` (TAG 4), `GfxRenderer::drawScaledCodepoint`, `getCodepointMetrics` | `renderCharScaled` exists but is file-static and 1-bit-only (`GfxRenderer.cpp:722`, used for SUP/SUB) | **partial primitive** |
| C5 | Border/shade page element | `PageBorderBox` (TAG 5), `renderBoxBackgrounds` drawn before text, borders after | `fillRectDither` exists (`GfxRenderer.h:548`) but no element | **absent** |
| C6 | Per-word source ranges | upstream v53–v55, 8 B/word, `WordStore` side table | per-**line** `TextBlock::visibleOffset` only (`TextBlock.h:64,72`); annotations key off it (`AnnotationStore`) | **partial** (line granularity) |
| C7 | Ruby geometry | adopted already | `TextBlock.cpp:197-215`, `CjkBreaks` | **present** |
| C8 | RTL focus split (`31dc683f`) | separate commit, audit row `text` | CPYOS still stores `focusSuffixX` | **absent** (out of scope here; noted so the two do not collide) |

---

## 2. Conflict map

Classification: **(a)** parser/CSS-only, safe; **(b)** needs `Page`/`TextBlock` model change +
cache bump; **(c)** needs renderer change; **(d)** needs per-word source ranges (RAM).

| Feature | CPYOS touch points | Collides with | Class |
|---|---|---|---|
| A1–A4 selectors/specificity/`::first-letter` | `css/CssParser.{h,cpp}` (`resolveStyle`, `insertOrMerge`, `SelectorEntry`, `findStyle/lowerBound`), `CssStyle.h` (`CssPropertyFlags`), parser `startElement` (needs an ancestor stack) | CSS cache format (`CSS_CACHE_VERSION`); **silently changes layout** → must also bump `SECTION_FILE_VERSION` (Section header carries no CSS version — `Section::writeSectionFileHeader` at Section.cpp:154-194 has no CSS field) | (a) + version bumps |
| A5 `font-size` (parse only) | `CssStyle.h` (`fontSize`, flag bit 19), `CssParser::parseDeclarationIntoStyle` | none until applied | (a) |
| A6 small caps | `EpdFontFamily.h` (bit 128 free in CPYOS), `Utf8.h`, `GfxRenderer::drawText/drawTextRun` (add `utf8SmallCapsUpper` + 3/4 advance), `ensureSdCardFontReady(..., includeSmallCaps)` | style byte is serialized per word in the arena → cached pages carry the bit only if we bump. 16-level path also has to honour it (`drawTextRun` already branches on `style`) | (a) parse + (c) render |
| A7 `white-space` | `ChapterHtmlSlimParser.cpp:58-86` (`isWhitespace`, `trimAndNormalize`), plus the character callback | changes **word splitting** → all metrics shift | (a) + bump |
| A8 page breaks | `BlockStyle.h` (`pageBreakBefore/After`), parser `startElement`/`endElement` + `makePages` | none structurally; `BlockStyle` is not serialized as a whole — `TextBlock::serialize` writes fields one by one (`TextBlock.cpp:458-473`), so a new field is opt-in | (a) + bump (page boundaries move) |
| B1/B2 orphans/widows/keep-with-next | `ChapterHtmlSlimParser` (page loop), `ParsedText::linesAfterCurrent()` (layout-only), reuses `currentPage->elements` tail removal | `PageBottomAlign` (`applyVerticalBottomAlign`, ChapterHtmlSlimParser.cpp:2472) recomputes shifts per element; carrying lines means the page's element list is mutated after add → bottom align must run **after** the carry. Also `currentPageLinkLines[]` indexes elements (`:2448-2451`) | (a) + ordering hazard |
| B3/B4 block font-size | `BlockStyle.h` (`fontId`, `fontScale`), `makePages` (`renderer.getLineHeight(fontId, lineCompression)` at :2520), `ParsedText::layoutAndExtractLines` (threads `fontId` into every measure), `TextBlock::serialize` (+4 B/line) | **per-block line height**; `ParsedText` erases word vectors per line — a `wordSizeSlots` vector must be erased in lockstep (`ParsedText.cpp:780-796`) | (b) + (c) |
| B5 tables | `ChapterHtmlSlimParser` table state (`MAX_GRID_TABLE_*`), `parseStep` re-read of the file (`measureTableColumns` needs the raw markup again → CPYOS's parser streams from `text` callbacks; upstream re-opens `HalFile`) | CPYOS streams the XHTML via Expat from an already-open file; the pre-scan means buffering/re-reading. On C3, 2 KB/row is a heap risk | (a) + RAM |
| C1/C2 per-word fonts | `TextBlock` (arena!), `ParsedText` (all width paths take one `fontId`: `measureWordWidth` :146, `calculateWordWidths`, `computeLineBreaks`, `computeHyphenatedLineBreaks`, `extractLine`, `calculateRubyExtraStartOffset` :802), `PageLine` (unchanged) | **CPYOS's one-allocation arena invariant** (`TextBlock.h:14-30`: "All per-word data lives in ONE flat heap allocation"). Upstream adds a *second* `unique_ptr<uint8_t[]> wordFontData` per line → breaks the documented invariant and the `arenaSize()` contract, which `serialize` relies on (`TextBlock.cpp:446`) | (b) + (c) |
| C3 variants | `GfxRenderer` (font map + a new provider), `SdCardFontSystem` (lazy faces, LRU), `CrossPointSettings::getReaderFontId` (app already maps pt→id) | CPYOS's reader face is either the system pack (`CJK_READER_SIZE_FONT_ID(pt)`, `fontIds.h:24`) or an SD family (`computeTtfFontId(family, pt)`, `SdCardFontSystem.cpp:31`). Both are **stable, serialisable ids** — good. But loading is done once in `ensureLoaded()` at activity entry (`SdCardFontSystem.cpp:846`), not on demand | (c) |
| C4 drop caps | new `PageDropCap` (TAG 4), `Page::deserialize` switch (`Page.cpp:249-...`), parser `layoutDropCapLines`, `renderer.drawScaledCodepoint` | vertical text (`Page::setVertical` only converts `TAG_PageLine`, `Page.cpp:57-80`); bottom align (must shift the cap); ruby overhang at line start (`calculateRubyExtraStartOffset`); hyphenation (`cb2a4c78` needed `maxLines`); word hit-testing walks `TAG_PageLine` only (EpubReaderActivity.cpp:1091,1298) so the dropped letter becomes unselectable | (c) + (b) |
| C5 borders/shade | new `PageBorderBox` (TAG 5), `Page::render` order (backgrounds first), all render passes (`render`, `renderImages`, `renderWithImagePlaceholders`), `Page::hasImages`/`hasImagesNeedingDecode` unaffected | vertical text (a box in column space); bottom align (box height follows shifted lines) | (b) + (c) |
| C6 source ranges | `TextBlock` arena, `ParsedText` visible-offset tables, `Section` page LUT | see §3.5 | (d) |
| — vertical CJK (`ReaderRenderSpec::verticalText`) | `Page::setVertical`, `TextBlock::renderVertical` | every new element type must either be suppressed or given a column mapping | (c) |
| — 16-level AA | `GfxRenderer::draw4BitGlyphPixel` (:672), `grayscaleTextBinary`, `drawTextRun` 4-bit branch | drop caps must produce 4-bit coverage, not a thresholded bitmap; per-word fonts must all be 4-bit faces (`readerFaceIsFourBit()`, EpubReaderActivity.cpp:4841-4870) | (c) |

---

## 3. Phased absorption design

### 3.0 Cache and versioning plan (applies to all phases)

- `SECTION_FILE_VERSION` 88/87 → **89/88** at L1, **90/89** at L2, **91/90** at L3.
  `SECTION_FILE_PARTIAL_VERSION` is derived (`Section.cpp:96`), so partial builds invalidate
  automatically — no extra work.
- `CssParser::CSS_CACHE_VERSION` 13 → **14** at L1 (selector key format changes).
- **Both must move together.** `Section` does not key on the CSS cache version
  (`writeSectionFileHeader`, Section.cpp:154), so bumping CSS alone would rebuild stylesheets and
  still serve pages laid out under the old cascade.
- Update `docs/file-formats.md` (§`section.bin`, AGENTS golden rule 10).
- No new `ReaderRenderSpec` field, so the cache *key* is unchanged; only the version byte moves.

### 3.1 L1 — parser/CSS only (no Page model, no renderer change)

**Goal:** correct cascade. Today a book whose styling lives in `.chapter p`, `div.body > p` or
`p#lead` renders with default indent/alignment. This is the single highest-value, lowest-risk
piece for third-party EPUBs.

Files:

| File | Change |
|---|---|
| `lib/Epub/Epub/css/CssParser.h` | `CssAncestor` (hash-based, `MAX_CLASSES=4`), `KeyPieces`, `resolveStyle(tag, class, id, ancestors, n, firstLetter)`, `hasFirstLetterRules()`, `makeAncestor()`, `noteRuleShape()`, `CSS_CACHE_VERSION=14` |
| `lib/Epub/Epub/css/CssParser.cpp` | port `parseCompound`, `packSpecificity`, `contextSpecificity`, `compareEntryToPieces`, `lowerBound(KeyPieces)`, `insertOrMerge(KeyPieces)`, `resolveStyle` match-and-sort; keep CPYOS's `applyOver` and the `1bd323dd` inherit flags untouched |
| `lib/Epub/Epub/css/CssStyle.h` | `fontSize` (CssLength) + `smallCaps` + `pageBreakBefore/After` + `preserveWhitespace` flags (bits 19–23) — **parsed** in L1, applied later; plus `borderWidthsDefined/borderStylesDefined` placeholders only if L2 lands in the same release |
| `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.{h,cpp}` | `std::array<CssAncestor, 16> cssAncestors` + `push/pop` in `startElement`/`endElement`; pass `idAttr`; call `firstLetter` lookup only when `hasFirstLetterRules()` |
| `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp:61-86` | `white-space` / `<pre>`: `trimAndNormalize` becomes `normalizeWhitespace(str, preserve)`; `<pre>` sets `preserveWhitespace` on the block |

Data structures: `CssAncestor` = `{uint32 tagHash; uint32 idHash; uint32 classHashes[4]; uint8 count;}`
≈ 28 B; 16 entries ≈ 448 B, lives inside the (heap) parser, not on the layout stack.

Cache impact: CSS cache 13→14; Section 89/88.

Tests (host, no device):
- `test/css_parser` (CMake + gtest; upstream added +332 lines of tests in the same commit — port
  them, they cover compound/descendant/specificity). **Local boxes have no cmake** (AGENTS §4.6),
  so also add a non-cmake runner under `cpyos/src/css_cascade/` following the
  `src/rounded_clip/` pattern (pure-logic + a small `run_*.sh`, with a mutation step: drop the
  context-specificity term and the test must go red).
- `test/chapter_html_slim_parser` for whitespace/pre.

Effort: **4–6 dev-days** (parser plumbing is mechanical; the spec work is in `resolveStyle`'s
match enumeration and the cache serialiser).

### 3.2 L2 — block layout

**Goal:** page breaks, orphan/widow control, borders/shading, wider tables.

| Piece | Model change | Cache | Notes |
|---|---|---|---|
| Page breaks (A8) | `BlockStyle::pageBreakBefore/After` (layout-time; not serialized) | bump (page boundaries) | `applyPendingPageBreak()` in `startElement`; must not fire on the first page |
| Orphans/widows + keep-with-next (B1/B2) | none serialized; adds `ParsedText::linesAfterCurrent` (`int`, layout-only, set in `extractLine` callback), `ChapterHtmlSlimParser::{layoutParagraph, paragraphLinesOnPage, keepWithNextLines, recentLineOffsets[6]}` | bump | must run **before** `applyVerticalBottomAlign` (`:2468`), and carried lines must drop from `currentPage->elements` *and* `currentPageLinkLines` |
| Borders / shade (A9/C5) | new `PageBorderBox` **TAG 5** + `BlockStyle` box fields (`left/right/padTop/padBottom`, 4 `CssBorderSide`, `shaded`) + `BoxScope` stack (max 8) | bump | `Page::deserialize` gains one branch (`Page.cpp:249`); `Page::render` must draw **shaded backgrounds first**, then lines, then borders (upstream `renderBoxBackgrounds`); `TAG 5` must be ignored by `hasImages`-style predicates and by all `TAG_PageLine` walkers in `EpubReaderActivity.cpp` (1091/1298/4297/4936/6082) — they already skip unknown tags, verify each |
| Table pre-measure (B5) | parser only, but needs a second pass over the table markup | bump (metrics) | CPYOS streams Expat from an open `HalFile`; the cheapest port is to buffer the current `<table>` subtree's raw bytes (bounded, 8 KB) instead of upstream's re-open. On C3 keep the 512 B/cell limits and use equal columns |
| Bottom-align interaction | `applyVerticalBottomAlign` must shift `TAG_PageBorderBox`/`TAG_PageDropCap` alongside lines | — | boxes spanning a page break must be split (upstream zeroes the cut side's width) |

Renderer changes: `fillRectDither` already exists; `drawBorderEdge` is ~30 lines of
`fillRect` (port from `0305aaa6 lib/Epub/Epub/Page.cpp`). Must be **suppressed in vertical
mode** (`Page::setVertical` has no column mapping for boxes) and must not disturb
`Page::renderWithImagePlaceholders`.

Effort: **8–11 dev-days** (borders ≈ 3, orphans/widows ≈ 3, tables ≈ 3–4, plus soak).

### 3.3 L3 — typography

Split into four independently shippable pieces, ordered by value/risk.

**L3a — block-level font size (3–4 dd).** `BlockStyle::fontId/fontScale`;
`makePages` uses `renderer.getLineHeight(blockFontId, lineCompression)`;
`ParsedText::layoutAndExtractLines` keeps its single-`fontId` signature but the caller passes the
block font. `TextBlock::serialize` writes `blockStyle.fontId` (+4 B/line, upstream does exactly
this at `0305aaa6 blocks/TextBlock.cpp` `serialize`). Plus the parser-only `InlineSizeState`
heuristic so `<p><span class="big">Chapter One</span></p>` still sizes the block.
This covers headings, `.title{font-size:2em}`, `<small>` blocks — most real-world usage — with
**no per-word runs and no arena change**.

**L3b — per-word runs (7–9 dd).** Only after L3a.
- Model: extend the TextBlock arena with a slots region instead of upstream's second allocation:
  `arenaSize()` gains `+numWords` bytes (slot per word) and the 4×`int32` slot table is stored in
  `blockStyle`-adjacent scalars, e.g. `int32 wordFontIds[3]` written per line (12 B/line) so the
  arena stays one allocation. `TextBlock.h:14-30` invariant preserved.
- Layout: thread a word→font-id lookup through `measureWordWidth` (`ParsedText.cpp:146`),
  `calculateWordWidths`, `computeLineBreaks`, `computeHyphenatedLineBreaks`, `extractLine`,
  `calculateRubyExtraStartOffset`, and the per-line word-vector erase block (`:780-796`) which must
  also erase `wordSizeSlots`.
- Render: `TextBlock::render` resolves `renderFontId()` per word and computes `lineAscent()` once
  per line; decorations move to baseline-relative offsets (upstream removed the fixed
  `yOffset` table).
- Slot resolution at parse time: `ParsedText::sizeSlotFor(scale)` (dedupe, nearest when full,
  `MAX_WORD_FONTS=3`).

**L3c — small caps (1–2 dd).** `EpdFontFamily::SMALL_CAPS = 128` is **free in CPYOS**
(`EpdFontFamily.h:13-22`; bit 64 is `RUBY_CONTINUE`, and CPYOS's style byte has no
`DISCRETIONARY_HYPHEN_FLAG`). Implement in `drawTextRun` (uppercase glyph + 3/4 advance) so the
16-level path and the emoji-splitting wrapper both get it for free.

**L3d — drop caps (4–6 dd).** New `PageDropCap` TAG 4, `drawScaledCodepoint`/`getCodepointMetrics`
on `GfxRenderer` (resample the 4-bit bitmap in `renderCharScaled` so 16-level pages get real AA),
`layoutDropCapLines` in the parser. Guard rails: disabled in vertical text; the first line of a
drop-cap paragraph skips hyphenation (avoids needing upstream's `maxLines`); the dropped letter
must still advance `visibleOffset` so `AnnotationStore` and JJWXC paragraph indices stay aligned;
selection hit-testing ignores the cap (documented limitation). **Lowest priority for CPYOS's
Chinese-web-novel corpus.**

Interaction with ruby/vertical/bottom-align: drop caps reserve width from line 1, so
`calculateRubyExtraStartOffset` must still see the reduced width; in vertical mode both drop caps
and border boxes are skipped (`Page::setVertical` remains line-only).

### 3.4 Font-id resolution for L3 (the CPYOS-specific shortcut)

Upstream had to build `SdCardFontSystem::{resolveVariant, loadVariant}` with LRU eviction because
built-in bitmap fonts only exist at 4 sizes and vector faces are loaded on demand. CPYOS already
owns a deterministic id space:

- system pack: `CJK_READER_SIZE_FONT_ID(pt) = 0x43555200 | pt`, one registered face per size
  10–34 pt (`src/fontIds.h:24`, `system_font_pack`), used by
  `CrossPointSettings::getReaderFontId()` (`CrossPointSettings.cpp:1082-1085`);
- SD families: `computeTtfFontId(family, pt)` (`SdCardFontSystem.cpp:31`) — a hash, stable across
  reboots, which is exactly what a serialised font id needs;
- bitmap profile: `NOTOSANS_{12,14,16,18}_FONT_ID`.

So the provider can be ~40 lines: `resolve(readerFontId, scale)` → nearest available point size
(`snapToNearestPointSize`, `ReaderFontSizes.h:41`) → id; `load(id)` → `sdFontSystem.ensureLoaded()`
or `system_font_pack::activateFace(pt)`. Missing ids fall back to the section font at render time
(a `renderFontId()` guard, as upstream). No dynamic scale256 faces, no LRU beyond what the pack
already does. This is the single biggest cost reduction versus a literal port.

### 3.5 L4 — source ranges: **optional, not required for L1–L3**

Upstream's ranges are 8 B/word (v53–v55) and the audit rejected them for good reason:
a 4,000-word chapter = 32 KB on disk, and a resident page of ~240 words = ~2 KB if ever loaded.

CPYOS does not need them:

- annotations already key on per-line `TextBlock::visibleOffset` (`TextBlock.h:64,72`, consumed by
  `AnnotationStore`);
- per-word fonts do **not** need ranges (the slot array is per word, not ranges);
- the only thing ranges add is byte-exact phrase anchors for selection/dictionary.

If we ever want them, a CPYOS-shaped encoding: **per-run** deltas, not per-word —
`(runStartDelta:varint, runLengthBytes:varint)` with CJK runs = one token, Latin runs = one run
between `<span>`/style changes. Realistic size: 1.5–2.5 B/run; a typical page has 30–60 runs
(the visible ones are already materialised in `TextBlock.linkSpans`-like arrays), so **< 200 B per
page resident, ~2–4 KB per chapter on disk** — 8–15× cheaper than upstream. Class (d), effort
**5–7 dd**, and it unlocks nothing that L1–L3 depends on.

---

## 4. Do-not-take list

| Rejected | Why |
|---|---|
| Upstream `Section.{h,cpp}` (v56–v59 numbering, `HEADER_SIZE` static_assert, upstream LUT layout) | CPYOS's is 87/88 with a derived partial sentinel (`Section.cpp:96`), a `deque` page LUT, and `ReaderRenderSpec` as the cache key. Only the *idea* of bumping is shared. |
| `TextBlock::wordFontData` as a second `unique_ptr` | Breaks CPYOS's documented single-allocation arena (`TextBlock.h:14-30`) and `arenaSize()`; the arena is the fix for C3 fragmentation. |
| `ParsedText::layoutAndExtractLines` signature from upstream (`void` callback, extra `maxLines`) | CPYOS's callback returns `bool` to stop early (`ParsedText.h:154-157`); the reader relies on it. Keep CPYOS's signature; add `maxLines` only if drop caps need it (they do not, by design §3.3). |
| `SdCardFontSystem::resolveVariant/loadVariant` + `MAX_*_VARIANTS` LRU | Upstream's registry exists to manufacture arbitrary scales. CPYOS already has stable per-point-size ids (§3.4). Take the `FontVariantProvider` *interface*, not the registry. |
| `CssStyle::reset()` removal and the upstream `CssStyle` body | CPYOS's `reset()`/`applyOver` shape is what `1bd323dd`'s inherit work was adapted onto; a wholesale `CssStyle.h` import would re-open that. Add fields only. |
| `drawScaledCodepoint` exactly as written | Upstream's `renderCharScaled` writes 1-bit pixels; on CPYOS 16-level pages a thresholded drop cap is visibly jagged (§45/§54 experience). Must write 4-bit coverage through `plotCoverage`/`draw4BitGlyphPixel`. |
| `MAX_GRID_TABLE_ROW_WORDS=160 / ROW_BYTES=2048` unconditionally | 2 KB per open row on a C3 (no PSRAM, ~380 KB total) is a real heap risk; keep CPYOS's 512 B when `!BOARD_HAS_PSRAM`, or gate the pre-measure. |
| Drop caps / border boxes in vertical CJK | `Page::setVertical` only maps `TAG_PageLine` (`Page.cpp:57-80`); boxes and caps have no column semantics. Skip them when `spec.verticalText` is set. |
| `float` beyond drop-cap marking, `initial-letter` layout beyond first-letter | Upstream only uses `float` to *mark* drop caps; a general float engine is out of scope and would require text flow re-ordering. |
| `072b6fc7` line spacing %, `88ea9bcf` 1.1/1.2 defaults | Already rejected; CPYOS chose 1.3/1.6 and percentage spacing would churn the cache key. |
| Upstream's `::first-letter` "leading punctuation + letter, max 12 bytes" *as a universal rule* | CPYOS's CJK text frequently starts with a quote mark; cap capture must be restricted to Latin/CJK ideograph after punctuation, and disabled for vertical text. |

---

## 5. Effort, risk, verification

| Milestone | Content | Dev-days | Risk |
|---|---|---|---|
| **M1 (smallest visible value)** | L1 selectors + specificity + id/compound + `white-space`/`<pre>`; `font-size` **parsed but not applied**; `::first-letter` stored | **3–4** | Low. No `Page` model change, no renderer change. Two version bumps. Visible on any third-party EPUB with `.body p`/`div.chapter > p` styling. |
| M2 | font-size applied at **block** level (L3a) | +3 | Medium-low: per-block line height touches `makePages` + `ParsedText`; font-id serialisation is 4 B/line. |
| M3 | L2 page breaks + orphan/widow + keep-with-next | +5 | Medium: ordering versus `applyVerticalBottomAlign` and `currentPageLinkLines`. |
| M4 | L2 borders/shading + table pre-measure | +5 | Medium: new page tags must be skipped by every `TAG_PageLine` walker; vertical mode. |
| M5 | L3b per-word fonts + L3c small caps | +8 | Highest: arena change + every measure path. Needs a written mutation test (drop the per-word width and the test must go red). |
| M6 (optional) | L3d drop caps | +5 | Medium: 16-level coverage resampling; selection mapping. |
| L4 | per-run source ranges | +5–7 | Low value until a feature needs phrase anchors. |

Total to "complete rendering parity minus drop caps": **~24 dev-days**; with drop caps ~29.

### Device verification (per milestone)

1. **readpico** (16-level AA, vertical text, bottom align, quick page turn): open
   `test/epubs/test_reader_rendering_matrix.epub` (upstream ships 4 fixtures in `0305aaa6`;
   vendor `test_reader_rendering_matrix.epub` + `test_force_paragraph_indents.epub` into
   `test/epubs/`, skip the emoji/synthetic-unicode ones already covered by §33).
   Check: 16-level text still `ON` (`LOG "reader face 4-bit; 16-level text ON"`), no `[GFX]
   Outside range`, no `comment bubble OVERLAPS text`.
2. **C3** (`default` env): must build and fit (currently 89.8 % flash); per-word slots must degrade
   to block font when the pack cannot offer a size.
3. **Vertical text**: a Chinese book with borders/drop caps in CSS must render as before (elements
   skipped), columns intact.
4. **Bottom align on**: borders/carried lines must not leave gaps or overlap.
5. `exp/refresh_soak.py` **36/36** after each milestone (catches page-cache and band regressions).
6. **JJWXC parity**: cached JJWXC EPUBs use paragraph indices and comment bubbles keyed on
   `paragraphIndex`/`paragraphFinal` — the M3 line-carrying must not renumber them; re-run the
   bubble soak (`CMD:PAGEDUMP`, 100+ pages, `bubbles=`, `OVERLAPS text` 0).

### Effort accounting for the parts we skip

Not porting `d7fcc7ce` (clippings), `source ranges`, upstream's `ClipSelectionActivity` (CPYOS has
`DictionaryWordSelectActivity` + its own selection in `EpubReaderActivity.cpp:1091/1298`), and the
`FontVariantProvider` registry removes roughly 1,000 of the 2,500 upstream lines — about 40 % of
the raw commit.

### Residual risks worth stating up front

- **Abrupt reflow.** M1 legitimately changes pagination for books whose CSS previously fell
  through. Users will see different page breaks and indents after the upgrade; the version bump
  makes that a one-time cost and the changelog must say so.
- **CSS cache growth.** Context keys store `ancestor-part + separator + compound`, so the cache
  strings grow. CPYOS bounds growth (`ruleGrowthStopped_`) and degrades to `ParseResult::Partial`;
  verify a CSS-heavy book still parses fully, otherwise the bounded pools need raising.
- **`defined` flags at 31/32 bits** after A5–A10; any future property needs a second word and a
  cache bump — note this in `CssStyle.h` next to the `static_assert`.
- **`Section` partial rebuilds** are rejected by the derived sentinel, so no mixed-version pages
  can survive — confirmed by construction (`Section.cpp:96`), worth one test.


---

# 字体/i18n 吸纳设计

# Absorbing upstream font/i18n work into CPYOS's system-font-pack architecture

Read-only audit of `upstream/main` = `a6fd569f` against this worktree (`b39f3706`).
No edits or builds were performed. All numbers below are measured on this checkout
(fontTools cmap census, file sizes, existing `build/data_manifest.txt`) or quoted from
`AGENTS.md` with the source line.

## 0. Premises corrected before designing

1. **CPYOS already ships Cyrillic and Greek.** `assets/fonts/CPYOSHanSansSC-Regular-8105.ttf`
   (9,031 cmap entries) covers 66/256 of U+0400–04FF, 48/144 of U+0370–03FF, `№` (U+2116),
   170/192 kana. Upstream `1899de8f`/`3f0f4fad`'s headline ("Cyrillic renders as nothing")
   is already false for every env that defines `CROSSPOINT_SYSTEM_FONT_PACK`
   (`[env:readpico…]` line 1194, `c3_hardware` line 332).
   Missing from the pack: Latin-Ext-A/B beyond a handful (`Ə/ə` absent), Hebrew, Arabic,
   Devanagari, Thai, Hangul, Armenian, Georgian, Ethiopic, Tifinagh, Vietnamese Ext.
2. **`NOTOSERIF_*` IDs are not dangling.** `src/main.cpp:884-888` registers them against
   `offlineReaderFontFamily`; `src/SystemFontPack.cpp:199-226` re-registers them per size.
   They are real faces, but label-only: `CrossPointSettings.cpp:707-708` forces
   `NOTOSANS` when no SD family is selected, and `src/SettingsList.h:28-34` hides the
   Serif option under `ENABLE_CHINESE_VERSION` (defined in `[base]`, `platformio.ini:127`).
   The dead weight is 16 uncompiled headers (`lib/EpdFont/builtinFonts/notoserif_1*.h`,
   4.8 MB tree), not the IDs.
3. **`98da1782`/`c6a2e311` are already present.** CPYOS has the compacted style API
   (`lib/EpdFont/SdCardFont.h:317-320`: `styleSlots_`, `styleData`, `hasStyle`) and
   `buildAdvanceTablePacked` already uses `hasStyle` (`SdCardFont.cpp:1948`).
4. **`2648933a` has no precondition in CPYOS.** There is no CSS `font-size`: `CssStyle.h`
   carries weight/style/alignment only and `CssParser.cpp` has zero `font-size` hits, so
   the "same SD size loaded twice as a CSS variant" scenario cannot occur.

## 1. Capability gaps

| Commit | Real capability (not the diff) | CPYOS today | Gap |
|---|---|---|---|
| `40b313a3` | (a) complete `az` locale; (b) `Ə/ə` in the *built-in UI* fonts so Azerbaijani chrome renders; (c) Noto Sans as Ubuntu's Latin fallback | 35 locales, no `az` (`lib/I18n/translations/`); pack lacks U+018F/U+0259 (measured) | **Yes** (b) for pack UI; (a) optional product call; (c) irrelevant (pack is the Latin face) |
| `1899de8f` | `U+0400-04FF,U+2116` in the two `pyftsubset --unicodes` lists that build the CJK bitmap faces | Pack covers it; `lib/EpdFont/scripts/build-cn-builtin-fonts.sh:106,119` still lacks the ranges | **Only for non-pack envs** (`simulator*`, `slim`, `sticky`); negligible |
| `3f0f4fad` | (a) ship Cyrillic in generated CJK faces; (b) **fold the CJK reader face's content into the generated font IDs so section caches rebuild when font data changes**; (c) publish IDs transactionally with the font set | (a) done via pack; (c) N/A; (b) **missing**: reader IDs are fixed literals `CJK_READER_SIZE_FONT_ID(pt)` (`src/fontIds.h:24`, `UiHighDpiProfile::reader12FontId` at `src/components/UiHighDpiProfile.h:13`); `lib/Epub/Epub/Section.cpp:250` compares `spec.fontId`, so a pack with different metrics keeps stale pagination until `SECTION_FILE_VERSION` (88/87, `Section.cpp:76-78`) is bumped by hand | **Yes — highest-value font gap** |
| `fc2de152` | Remove built-in Serif; migrate stored `fontFamily`; single default | Already effectively done (see §0.2); dead headers + dead `switch` branch remain | Cosmetic only |
| `7242d9ae` | One Python generator for font IDs, no Ruby, checked by a test | `lib/EpdFont/scripts/build-font-ids.sh` is bash+Ruby; **three** ID sources: that file, `src/SdCardFontSystem.h:22` (`CJK_UI_*`), `UiHighDpiProfile.h:13`; `CJK_UI_18_FONT_ID` is duplicated as a literal in `fontIds.h:20` and the Ruby script:119; no `--check`, no `test_font_ids.py` | **Yes — drift risk** |
| `cf50ce73` | Ubuntu 8 for the small UI slot so 8/10/12 match; Vietnamese tone marks | `SMALL_FONT_ID` → `notosans_8_regular` (`all.h`, `main.cpp:899`), but `typography::apply` rebinds `SMALL` to the pack at caption size on ReadPico (`src/Typography.cpp:96`) | Marginal (C3 chrome only) |
| `2648933a` | Stop loading a CSS size variant that equals an already-resident UI size (~2.9 KB X3) | No CSS `font-size` (see §0.4); `SdCardFontManager::loadFamilyExtraSize` already reuses a resident point size (`SdCardFontSystem.cpp:407`) | **None today** |
| `ce781eb8` | Two-pass UI scan (`ScanMode::UiCjk` + `FontDecompressor::prewarmCache`) before list painting so glyph decompression doesn't happen mid-paint | Reader has `PrewarmScope` (`EpubReaderActivity.cpp:4786`); FileBrowser/Recent/chapter-list prewarm explicitly; **no generic `UiListActivity` scan pass and no `TtfEpdFont` prewarm API** | Partial; the exact mechanism is bitmap-decompressor-specific |
| `98da1782`/`c6a2e311` | Fix advance scan after style compaction + test | Already present | None |
| `965722cf` | One catalog entry (Libron serif) | `sd-fonts.yaml` exists, but the device fetches `fonts.json` from `crossmux.com`/`.cn` (`src/network/CrossMuxEndpoints.h:36`) | Data-only; no firmware work |
| `072b6fc7` | 7-step line spacing as % of font size, replacing 4 abstract levels | 4 levels **plus per-family factors** (`CrossPointSettings.cpp:944-990`: SD 0.95/1.0/1.3/1.6; Serif 0.95/1.0/1.1/1.2; Sans 0.90/0.95/1.0/1.05) | Model conflict, not a gap |
| `88ea9bcf` | Revert the SD scale to 1.1/1.2 | CPYOS deliberately kept 1.3/1.6 | **Regression if taken** |
| `fdf5f76c` | Glyph-driven (not CJK-range) UI fallback + broader script probe list; delete `utf8IsCjkCodepoint` | `resolveTextFontId` is already glyph-driven in both branches (`GfxRenderer.cpp:349-451`); `utf8IsCjkCodepoint` is dead but still defined (`lib/Utf8/Utf8.h:80`) | Probe list is stale/disabled, not the resolver |

## 2. Absorption designs

### 2.1 System pack coverage vs FEXT1 extras

**Keep the split.** Pack = CJK + Latin + punctuation + kana + Greek + Cyrillic (measured);
everything else stays in FEXT1 (`font-extras/packs.json`: 844 KB emoji, 10.4 MB CJK-full,
6.76 MB world) because:
- app0 headroom is ~94 KB (last recorded readpico image 7,442,672 B of 7,536,640 B, `AGENTS.md` §62);
- one Noto CJK face cannot carry Arabic/Hebrew/Devanagari shaping alternatives, and merging
  them would multiply `assets/fonts/cpyos_han_sans.def` beyond the partition's 47,713 B
  i18n/emoji headroom and the C3 spiffs budget (`scripts/tests/test_system_font_pack.py::
  test_c3_pack_fits_spiffs_with_headroom`).

**Add `Ə/ə` (only if `az` ships).** New `scripts/add_pack_codepoints.py` that loads the
committed TTF plus a Latin source, merges the two outlines, adds cmap entries, and writes the
same path; do **not** put them in `chars_8105.txt` (the test asserts exactly 8,105 chars,
`test_system_font_pack.py::test_charset_is_covered`). Add `assets/fonts/chars_extra.txt`
and a `test_extra_codepoints_are_covered` sibling to `test_byline_punctuation_is_covered`.
Rebuild carriers with `python3 scripts/build_system_font_pack.py --embedded-out
assets/fonts/cpyos_han_sans.def`. Delta: TTF +~2 KB raw, `.def` +~1 KB, app0 +~1 KB.
The cmap **alias** trick already used for `«» − ›` (`assets/fonts/README.md`) cannot apply to
`Ə/ə` — there is no visually equivalent glyph in the subset; do not fake it with `E`/`e`.

**`az` locale.** Copy upstream `lib/I18n/translations/azerbaijani.yaml` with `_bcp47: az`.
Safe ordering: the language is persisted as a BCP47 code, not an index
(`CrossPointSettings.cpp:718` `I18n::languageFromCode`), even though `scripts/gen_i18n.py:473`
sorts the enum by tag. Cost: ~+9–10 KB i18n pack (currently 345,503 B; free window
0x100000–0x160000 = 393,216 B), and a **mandatory full data-partition flash**.

**Non-pack targets.** Do not regenerate the six CJK bitmap headers. Instead document the
limitation in `docs/engineering/chinese-build.md`; the only shipping env without the pack is
the simulator, and `readpico_nightly`/`default` both define it.

### 2.2 Per-script UI fallback in `typography::apply`

Concrete defect today: `SdCardFontSystem::loadSupplementFonts` (`SdCardFontSystem.cpp:874`)
builds UI supplement faces at the **fixed** `kUiFontSizes` points (12/14/16 on ReadPico,
`SdCardFontSystem.cpp:71-107`) regardless of `uiFontScale`. With `Compact` the pack rebinds
`UI_10`→10pt and `UI_12`→12pt (`Typography.cpp:96-104`), but the supplement is attached at
14/16pt, so a rare-script glyph inside a 10pt row renders at 14pt (wrong size, and a second
face). `font_extras` users are exactly the ones reading those scripts.

Design:
- Add `SdCardFontSystem::loadSupplementFonts(renderer, readerPointSize, const uint8_t*
  rolePoints, size_t roleCount)`; build one face per distinct active point (cache key
  `(family, point)`), attach via `renderer.addSupplementFont()` to
  `SMALL/UI_10/UI_12/CONTROL_18/NOTOSANS_18/READER_STATUS/READER_ESTIMATE` and to
  `CJK_UI_*` ids the pack registers.
- Call it from `typography::apply()` after `system_font_pack::configure()`, and from
  `FontExtrasActivity` install/remove (`:608`, `:634`) with `typography::pointsFor(scale)` —
  do **not** add a script probe list; `resolveTextFontId` already selects by codepoint
  coverage, so the requirement is only "a supplement exists at the row's point size".
- Leave the existing `kFallbackProbes` gate in `setupUiFallbacks` alone: on ReadPico the whole
  function returns early under `CROSSPOINT_SYSTEM_FONT_PACK` (`SdCardFontSystem.cpp:339`),
  and on C3 there is no room for world UI faces anyway.

### 2.3 Font-ID generation (adopt `7242d9ae` + `3f0f4fad` (b))

New `scripts/build_font_ids.py` (port of `lib/EpdFont/scripts/build-font-ids.py`) that owns
**all** ID sources and emits `src/fontIds.h`:

- header-derived IDs: unchanged hashing (sum of SHA-256 mod 2^32, signed) over the four
  `notosans_*` sizes + `chinese_chess_16.h` + `ubuntu_10/12`; unchanged values for unchanged
  headers, so `NOTOSANS_*` stay stable.
- move the hand-written `CJK_UI_8/10/12/14/16` (`src/SdCardFontSystem.h:22-26`),
  `CJK_UI_18` (`fontIds.h:20`) and `UiHighDpiProfile::reader12FontId` into the generated file;
  delete the duplicate literal in `build-font-ids.sh:119`.
- **pack signature**: `CJK_PACK_SIGNATURE = crc32(CPYOSHanSansSC-Regular-8105.ttf) & 0xFFFF`,
  then
  `#define CJK_READER_SIZE_FONT_ID(pt) (0x43000000 | (CJK_PACK_SIGNATURE << 8) | (pt))`.
  `pt` ≤ 34 fits the low byte; keep the existing `static_assert(CJK_READER_SIZE_FONT_ID(10/34)
  != 0)` and add a Python uniqueness assertion over the whole table.
  Every consumer (`SystemFontPack.cpp:226`, `Typography.cpp:122`, `SdCardFontSystem.cpp:850`,
  `WebBrowserActivity.cpp:173`) uses the macro, so one edit keeps them coherent, and
  `Section.cpp:250` then rebuilds pagination exactly once after a pack change — no more
  manual `SECTION_FILE_VERSION` bump for font edits.
  Cost: IDs change once on first boot after flashing (one-time cache rebuild); 0 flash/RAM.
- Add `scripts/tests/test_font_ids.py`: generated text == committed header; all non-zero and
  unique; mutating a copy of the `.def` changes only pack-derived IDs; mutating
  `notosans_cjk_12.h` changes only header-derived IDs; missing input raises instead of
  emitting an ID.
- Keep Ruby out of the loop; `build-font-ids.sh` becomes a `python3` wrapper or is removed.
  Update `docs/engineering/generated-files.md`.

**Are content-hash IDs worth it for SD TTF families?** No. `computeTtfFontId` is FNV(name,
size) (`SdCardFontSystem.cpp:31-42`); folding size/mtime in would churn IDs on every file
copy, change bookmark/cache keys, and can collide with the UI id space. The pack is the one
face CPYOS builds and ships, so hashing it is enough. (A user replacing a same-named `.ttf`
with different metrics still gets stale pagination — that is a pre-existing limitation, not
made worse here.)

### 2.4 NotoSerif: wire or remove?

Neither as upstream did it. Keep the IDs (they are cheap aliases that also keep old
`fontFamily=0` files readable and seed the `typography::apply` emergency chain,
`Typography.cpp:114-118`), delete the 16 unused `notoserif_1*.h` headers and drop them from
the generator inputs; keep `notoserif_title_23.h` (used by `titleFace()`, `main.cpp:352-377`).
Removing the enum value would require a settings migration (`CrossPointSettings.cpp:668-669`,
`:700`, `:853-855`) for zero user-visible change. If a serif reading face is wanted, it belongs
in the SD/FEXT channel, not in the pack.

### 2.5 Catalog

Two channels must not be conflated:
`.cpfont` `fonts.json` (`FontDownloadActivity`, `FONTS_MANIFEST_VERSION 1`) served by
`crossmux.com/.cn`, versus FEXT1 vector packs self-hosted on Pages
(`font-extras/packs.json`, `scripts/build_font_extras_manifest.py`).

- **Libron (`965722cf`)**: add the block to `lib/EpdFont/scripts/sd-fonts.yaml` so CPYOS's
  generator stays upstream-compatible. Zero firmware change; it only appears if the *served*
  manifest is regenerated.
- **Do not host a second `fonts.json` yet.** The current endpoint is upstream-owned; hosting
  our own duplicates the manifest pipeline and the `.cpfont` artifacts. Revisit only if
  upstream retires the service, and then publish it from `site/` alongside the FEXT assets.
- **Do not express Libron as FEXT1.** FEXT `kind=fallback` registers a *supplement*
  (rare-glyph face, `src/FontExtrasFormat.h:18-20`), not a selectable reading family.

### 2.6 Line spacing

CPYOS's factors are deliberate and per-family (`CrossPointSettings.cpp:944-990`) — `88ea9bcf`
would collapse SD 1.3/1.6 to 1.1/1.2 and undo that. Do not replace the 4-level enum.

Absorb the *idea* as an advanced override: `readerLineSpacingPercent` (uint8, 0 = off),
applied at the end of `getReaderLineCompression()` as
`if (readerLineSpacingPercent) return readerLineSpacingPercent / 100.0f;`
(keeping the family factors when off). New row in `src/SettingsList.h` under Reader/Text, a
`SettingAction` stepper in `TextSettingsActivity`, one i18n key (×35 locales, ~+1 KB in the
pack). No cache work: `lineCompression` is already in the section header
(`Section.cpp:159`, compared at `:250`). Migration is a no-op (new key defaults to 0).

### 2.7 Non-CJK GUI fallback (`fdf5f76c`)

The portable idea is "decide from coverage, not from a script table". CPYOS's resolver already
does that; what is stale is the probe gate (`SdCardFontSystem.cpp:372-378`, 10 codepoints) and
the dead `utf8IsCjkCodepoint` (`lib/Utf8/Utf8.h:80`, unused). Actions:
- delete `utf8IsCjkCodepoint` (trivial);
- do **not** extend the probe list to 14/16 codepoints ("probe 14 UI sizes vs 10" is a
  non-question on ReadPico because the gate is compiled out and the pack supplies every role);
  if C3 world-script UI is ever requested, replace the probe list with a coverage test against
  the *active role faces* rather than adding more constants.

## 3. Phased plan

### P0 — quick wins (≈1.5 days total, no layout risk)

| Item | Files | Effort | Delta | Gates |
|---|---|---|---|---|
| Python font-ID generator + pack signature | new `scripts/build_font_ids.py`, `src/fontIds.h`, `src/SdCardFontSystem.h`, `src/components/UiHighDpiProfile.h`, delete Ruby path in `build-font-ids.sh` | 0.5 d | Flash/RAM 0; one-time pagination rebuild | `readpico` + `default` build; `test_font_ids.py`; open a book twice (2nd open must not log a rebuild); swap the `.def` in a scratch build and confirm `Unknown version`/rebuild in `Section` logs |
| Supplement UI faces follow `uiFontScale` | `src/SdCardFontSystem.{h,cpp}`, `src/Typography.cpp`, `src/activities/settings/FontExtrasActivity.cpp` | 0.5 d | PSRAM +~20–30 KB worst case (one extra face+16 KB arena per distinct point); Flash <1 KB | with `world-scripts` installed: Compact/Large/XL, verify Hebrew/Arabic glyph size matches row text; `test_ui_font_fallback.py` |
| `Ə/ə` + `az` locale (product decision) | new `scripts/add_pack_codepoints.py`, `assets/fonts/chars_extra.txt`, `assets/fonts/*.ttf/.def`, `lib/I18n/translations/azerbaijani.yaml` | 0.5 d | app0 +~1 KB; data partition +~10 KB; C3 spiffs +~2 KB | `test_system_font_pack.py` (new coverage case), `test_emoji_pack`/i18n pack equivalence, full data flash, device check of Azerbaijani UI |
| Delete `utf8IsCjkCodepoint`, dead NotoSerif headers | `lib/Utf8/Utf8.h`, `lib/EpdFont/builtinFonts/` | 0.1 d | tree only (4.8 MB) | full build (both envs) |

### P1 — deeper (≈2 days)

| Item | Files | Effort | Delta | Gates |
|---|---|---|---|---|
| Line-spacing percent override | `CrossPointSettings.{h,cpp}`, `SettingsList.h`, `TextSettingsActivity.cpp`, i18n YAML | 1 d | Flash ~+1 KB, data +~1 KB | compare 100/130/160 % on a 5,000-char chapter; section cache re-key check; `test_chapter_html_slim_parser` if it asserts line counts |
| Libron catalog entry | `lib/EpdFont/scripts/sd-fonts.yaml` | 0.1 d | 0 | `scripts/generate-font-manifest.py --check` on a rebuilt manifest |
| Probe-list cleanup (only if C3 world UI is wanted) | `SdCardFontSystem.cpp:360-398` | 1 d | 0 | C3 build; world pack on SD; file-browser screenshot |

### P2 — explicitly deferred

- **CSS `font-size` / relative sizes** (the precondition for `2648933a`): multi-week layout
  work touching `CssStyle`, `ChapterHtmlSlimParser`, `Section` format and cache versions.
  Out of scope; revisit with a dedicated plan.
- **TTF prewarm + `UiListActivity` scan pass** (the portable half of `ce781eb8`): needs
  `TtfEpdFont::prewarmText()` and a cache-manager equivalent for the vector path. Only worth
  starting if `[PERF] … first frame`/`render` logs show glyph rasterization as a list-paint
  cost on ReadPico (currently the pack's UI arenas are 128–512 KB, so mid-page flushes are
  unlikely). 2–3 days, no flash delta, PSRAM +arena.

## 4. Reject list (wrong for CPYOS even under generous absorption)

1. **Regenerating 38 bitmap headers to add `Ə/ə`** (`40b313a3`'s mechanism): ~68k changed
   lines, doesn't reach the ReadPico pack at all, and the pack is the shipping face. Merge two
   outlines into the pack instead.
2. **`1899de8f` + `3f0f4fad`(a) header regeneration**: 13 MB / ~106k changed lines of
   `notosans_cjk_*` for envs CPYOS does not ship; the pack already has Cyrillic and `№`.
3. **`fc2de152`'s deletion of NotoSerif headers *and* IDs**: forces a stored-`fontFamily`
   migration and removes aliases the emergency chain and `SystemFontPack::configure` rely on.
   Deleting only the 16 dead headers is fine.
4. **`2648933a` as-is**: CPYOS has no CSS size variants; porting it adds `getFontId(family,
   size)` plumbing and a variant-id convention for zero measurable gain.
5. **`98da1782` / `c6a2e311`**: already in the tree in compacted form; no action.
6. **`cf50ce73`**: `SMALL_FONT_ID` is rebound to the pack on the shipping target; swapping in
   `ubuntu_8_regular.h` changes an ID and adds ~5 KB for C3 chrome only.
7. **`88ea9bcf`**: reverts CPYOS's deliberate 1.3/1.6 SD line-spacing scale and per-family
   factors (documented at `CrossPointSettings.cpp:944-950`).
8. **`965722cf` as a firmware feature**: it is catalog data; no code path changes.
9. **`ce781eb8`'s `ScanMode::UiCjk` scope verbatim**: it prewarms `FontDecompressor` arenas,
   which the ReadPico system pack (streamed `TtfEpdFont`) does not use; adopting the scope
   without a TTF prewarm API would be a no-op that looks like a fix.
10. **Hosting a second `.cpfont` catalog**: two manifests to keep in sync; CPYOS's own
    hosting budget is already spent on FEXT assets.
11. **Content-hash IDs for SD families**: churns IDs and cache keys on every file copy; the
    pack is the only face whose bytes CPYOS controls at release time.

## 5. Test/device summary for the whole batch

Host: `scripts/tests/test_font_ids.py` (new), `test_system_font_pack.py`, `test_emoji_pack.py`,
`test_ui_font_fallback.py`, `test_builtin_font_shrink.py`, `scripts/tests/test_font_extras_manifest.py`,
`src/i18n_pack/run_i18n_pack_test.sh`, `src/font_extras/run_font_extras_format_test.sh`.
Builds: `readpico`, `default` (C3), `readpico_nosource`; verify `esp_app_desc.project_name`.
Device: after flashing, open a cached book (pagination rebuilds exactly once), switch
`uiFontScale` with `world-scripts` installed, and re-check the Wi-Fi list SSID with emoji
(the 250-codepoint subset behaviour is unchanged by this plan).


---

# 批注/短语查询吸纳设计

# Absorbing upstream clippings / source ranges into CPYOS annotations + phrase lookup

Read-only audit. Worktree `/Users/hongbin/Documents/projects/cpyos/.worktrees/upstream-sync` @ `b39f3706`.
Upstream refs: `d7fcc7ce` (clipping/highlight + source ranges), `c201af43` (multi-token lookup via clipping),
`ad24aea8` (clipping-delete popup blink). Verdict up front: **take the phrase-lookup UX, reject the clipping
subsystem and the per-word source ranges.**

---

## 1. Current-state map

### 1.1 Selection

Selection is entirely inside `EpubReaderActivity` — there is no selection Activity (unlike upstream's
`ClipSelectionActivity`, 858 lines).

- Gesture: self-timed 650 ms press, 64 px slop (`EpubReaderActivity.cpp:995,1002`), driven from live contact in
  `handleSelectionTouch()` (`:1388`) because the SDK long-press is gated by pinch flags (`EpubReaderActivity.h:277-280`).
- Anchor: `beginSelection()` (`:1490`) loads **one** page (`loadPageForSelection()`, `:1482`), flattens it to
  `AnnoWordRect{line, word, start, end, text, x, y, w, h}` via `layoutPageWords()` (`:1288-1329`), snaps to the
  nearest word, then re-selects the whole range if the press lands on an existing annotation (`:1551-1571`).
- Extend: two drag handles (`drawSelection()`, `:1615-1702`; `handleSelectionHandleTouch()`, `:1704`) and
  arrow keys (`handleSelectionInput()`, `:1739`), capped at `kSelectionRedrawMs = 320` repaints.
- Action bar: two chips only — `labels[2] = {delete|highlight, note}`, hit arrays `selectionMenuHitX/Y/W/H[2]`
  (`EpubReaderActivity.h:322-326`), drawn by `drawSelectionMenu()` (`:1904-1981`), committed by
  `activateSelectionMenuChoice()` (`:1883`).

### 1.2 What is stored

`annotations::Annotation` (`AnnotationStore.h:33-43`): `spineIndex` + `startOffset`/`endOffset` (exclusive,
**chapter-wide visible-codepoint offsets**) + `kind` + `quote` (≤120 B) + `note` (≤400 B) + `chapterTitle`.
Codec is whole-file strict (`deserialize()` `AnnotationStore.cpp:107-145`); saved atomically to
`<epub cache>/annotations.bin` (`saveAnnotations()`, `EpubReaderActivity.cpp:1049-1077`). Same store is reused by
`PdfReaderActivity` with page ordinals, so **any format change must stay meaningful for PDF**.

The offset is produced by the parser, not by the reader: `ChapterHtmlSlimParser::characterData()` counts *every*
visible codepoint (`ChapterHtmlSlimParser.cpp:1685-1691`), stored as u16 deltas in `ParsedText::pushVisibleOffset()`
(`ParsedText.cpp:287-300`) and stamped on each line by `TextBlock::setVisibleOffset()` (`ParsedText.cpp:1766,1793`).

### 1.3 How highlights survive reflow

Rendering re-derives each word's range from the cached line and the codepoint count
(`drawAnnotations()`, `:1081-1138`: `wordStart = offset; wordEnd = wordStart + countUtf8Codepoints(text); offset = wordEnd`).
Because the anchor is a chapter-wide character offset, a font/size/spacing/writing-mode change re-lays out and the
same range lands on the same characters. **No layout signature is needed** — better than upstream, which keeps
`layoutSignature` (`ReaderRenderSpec.h`, `clippingStoredRangeMatchesLayout()`) and falls back to page/word ordinals
when the signature does not match (`ClippingStore.h:clippingContainsWord`).

### 1.4 How the dictionary is invoked today

`openDictionaryWordSelect()` (`EpubReaderActivity.cpp:840-871`) pushes `DictionaryWordSelectActivity` with **one**
`Page`. That activity owns `Dictionary dict` and implements a full-screen word picker
(`DictionaryWordSelectActivity.cpp:156-208`: `dict.lookup(words[selected].text, ...)`, then
`DictionaryDefinitionActivity::enableLookup(dict, query)` so recursive lookups stay in that activity's back stack).
`Dictionary::lookup` is exact-match + `.syn` + English stemmer (`Dictionary.cpp:855-960`); `cleanWord()` strips
punctuation only at the **edges** (`:793-820`) and does not touch internal whitespace.

So: **there is no "look up this selection" path today**, and a selected phrase can only be looked up by tapping
words one at a time in the picker.

### 1.5 Real weaknesses

| # | Weakness | Evidence |
|---|---|---|
| W1 | No phrase entry point from a selection. | `drawSelectionMenu()` `:1904` has exactly two chips; `DictionaryWordSelectActivity` is page-first, not text-first. |
| W2 | **Intra-line offset drift.** `layoutPageWords()` adds only each word's codepoint count (`:1311-1313`), while the parser counted the intervening whitespace (`ChapterHtmlSlimParser.cpp:1691`). So from the second word of a line on, `start/end` are short by the number of gaps before that word. Storage and drawing share the drift (`:1101-1103`), so the screen looks right — but after repagination the gap count changes and a highlight's start/end moves by a few characters. It also mislabels the first/last word of a re-selected annotation. | cited above |
| W3 | `quote` is concatenated with **no separator** (`commitSelection()` `:2023-2042`), so a Latin quote becomes `thequickbrown`; CJK is accidentally fine. Export (`formatExport()`) inherits this. | `:2031` |
| W4 | Selection is **single-page**: anchors and words come from one `Page` (`:1482`, `:1516`). Multi-paragraph works (nothing stops it) but the paragraph boundary is invisible — `TextBlock::paragraphIndex` exists (`TextBlock.h:57`) yet `layoutPageWords` ignores it. | |
| W5 | `AnnoWordRect::text` points into the page that `loadPageForSelection()` returns *that call*; every drag step reloads the page. Any new consumer must copy within the call. | `:1349,1362` |
| W6 | `quote` is capped at 120 B (`kMaxQuoteBytes`), so a long selection's stored quote is truncated — a "look up again from the annotation list" action must use the live selection, not the stored quote. | `AnnotationStore.h:24` |

Not weaknesses: anchoring across reflow, delete-by-tapping-the-bubble (`:1258`), and the strict codec are all
sound and should stay.

---

## 2. Phrase lookup design

### 2.1 Where the button sits

A third chip in the existing bar. `drawSelectionMenu()` already handles both orientations (row below the
selection, or a vertical stack beside the column, `:1904-1981`), and **every touch is consumed while a selection
is open** (`handleSelectionInput()` returns `true` for all input, `:1739+`; `handleSelectionTouch()` swallows the
press `:1400-1410`), so a third chip cannot leak into page-turn zones.

- Two layouts, no shared index:
  - **new selection** → `[查词][高亮][批注]`, lookup first and default (`selectionMenuChoice = 0`), because it is the
    most frequent action and the first chip is the one the anti-double-tap guard protects.
  - **editing an existing annotation** (`selectionEditingIndex >= 0`) → keep today's `[删除][批注]`. Do not offer a
    lookup there: the stored quote is truncated at 120 B (W6) and the range is what the user came to change.
  So `activateSelectionMenuChoice()` (`:1883-1902`) branches on `selectionEditingIndex` before indexing the new
  3-slot mapping — the one place where an off-by-one would silently delete a highlight.
- Reuse `STR_LOOKUP` (already translated in 31/35 language files, en `"Look Up"`, zh `"查词"`). No new key for the
  button.
- Geometry: widen `labels[2]`→`[3]`, `selectionMenuHit*[2]`→`[3]`, cycle range in `handleSelectionInput()`
  `0..1`→`0..2` (`:1745-1751`), vertical stack height `2*chipH+10`→`3*chipH+20`, and clamp the horizontal row to
  `screenW-12` (3 CJK chips ≈ 3×88+2×14 = 292 px, comfortable).

### 2.2 Data flow

```
long-press → selection (unchanged)
tap "查词" → activateSelectionMenuChoice(2)
          → buildPhraseQuery(words, selection range)      // new pure helper
          → endSelection()                                // drops overlay before pushing
          → startActivityForResultWith<DictionaryWordSelectActivity>(
                 page, marginLeft, marginTop, PhraseQuery{phrase, anchorWord})
          → child onEnter(): lookupPending path (like upstream c201af43), no picker UI
          → dict.lookup(phrase) → DictionaryDefinitionActivity (recursive lookup armed)
Back      → child finishes → reader's handler: READING_STATS.resumeSession(); requestUpdate()
```

The reader keeps its `section`/page index/`visibleTextOffset` untouched; the child is pushed, not replaced, so
"return without losing position" is free — it is exactly the existing `openDictionaryWordSelect()` contract
(`:840-871`), plus skipping the picker.

Required changes to the child: a `PhraseQuery` ctor + a `lookupText_` field and an `onEnter()` early path (mirror
`c201af43`'s `DictionaryWordSelectActivity`, which deleted ~300 lines of picker code for this). Keep the picker
ctor intact — `CMD:DICT`-style debugging and the single-word path still use it.

### 2.3 Text assembly (fixes W3 and feeds W2)

New pure header `src/util/SelectionText.h` (no HAL, host-testable like `src/util/PlainTextTokens.h`):

- `joinSelection(tokens, paragraphs)` with the upstream separator rule (`ClipSelectionActivity::buildSelectedText`,
  `d7fcc7ce`): `'\n'` when the token starts a new paragraph, `' '` when the source had a gap, nothing when glued.
  Paragraph comes from `TextBlock::getParagraphIndex()`; the gap comes from W2's fix (or, until then, from a
  CJK-adjacency rule: no space between two CJK/Hangul codepoints).
- `normalizePhrase()`: collapse whitespace runs, drop a trailing discretionary `'-'` on a token that ended a line
  (the artifact upstream strips in `clippingText::append`), strip the em-space/U+00A0 prefixes upstream's
  `cleanWordStart` removes, then apply `Dictionary::cleanWord` semantics per edge. **Do not lowercase the whole
  phrase** — `cleanWord` already lowercases Latin (`Dictionary.cpp:820-821`) and would mangle CJK.
- `phraseCandidates()` returns at most 3 candidates (see 2.5).
- Clamp to 64 bytes on a token boundary: `Dictionary::lookup` refuses when `freeHeap < 5*wordBytes` and builds up
  to 5 stem variants (`Dictionary.cpp:861-869`), so an unbounded phrase is a real OOM path on C3.

### 2.4 Minimum selection / when to enable

Accept when the joined phrase has **≥2 visible codepoints and at least one token that survives `cleanWord`**.
That admits a single Latin word (still a word lookup) and a single CJK character, while rejecting a lone
punctuation mark. If it fails, `toast_.show(tr(STR_NOT_AVAILABLE))` as `beginSelection` already does (`:1504,1519`).

CJK vs Latin tokenization: reuse `tokenizePlainTextLine()` (`PlainTextTokens.h:27-59`) — it already splits Han/Kana
per codepoint, keeps Hangul word-whole, and never starts a token on a combining mark. That is the vocabulary the
dictionary side needs anyway (the definition viewer uses the same function to build selectable tokens), so
candidate generation and definition selection agree.

### 2.5 Failure UX (phrase not a headword)

StarDict idiom headwords are real but rare; a bare `STR_DICT_NOT_FOUND` card after a deliberate 3-second
long-press reads as a bug. Ladder, **hard-capped at 3 lookups for Latin and 4 for CJK** (each `lookup()` reopens
`.idx` and binary-searches, so the cap is about SD latency, not memory):

- Latin (tokens ≥2): whole phrase → longest token → (if ≥3 tokens) the token pair around the anchor word.
- CJK (one unspaced run): whole run → 4-codepoint window at the anchor → 3-codepoint window → 2-codepoint window.
  Chinese headwords are 2–4 characters, so this is the only fallback that can hit.

On a fallback hit, show the definition with a one-line hint above the headword ("短语未收录，已查：<headword>").
That needs **one** new key (`STR_DICT_PHRASE_FALLBACK`), i.e. +1 key × 35 languages, pack rebuild, and a data-partition
reflash — the same cost class as §59's `STR_USE_CACHE`. If the whole ladder misses: keep the standard NotFound
popup, but make Confirm re-open `DictionaryWordSelectActivity` in picker mode at the same page (no new screen, one
extra hop) so the user can pick the exact word. Do not silently open the picker unasked.

### 2.6 Non-goals

No cross-page selection in phase A (see §3.3, rejected), no annotation-sheet lookup of a *stored* quote (W6),
no automatic lookup on long-press (it would make every highlight cost an SD lookup and steal the gesture).

---

## 3. Upstream ideas worth taking (and what to reject)

### 3.1 Take

1. **Three-action selection sheet with Lookup** (`SELECTION_ACTIONS = {Lookup, Clip, Bookmark}`,
   `ClipSelectionActivity.cpp:33`). Confirms the 3rd chip and its default slot.
2. **Selection text builder semantics** — paragraph → `'\n'`, gap → `' '`, glued → nothing, plus `cleanWordStart`'s
   em-space/U+00A0 stripping and the discretionary-hyphen removal in `clippingText::append`
   (`src/clippings/ClippingText.h`). This is a *port of rules*, not code: CPYOS's `commitSelection` needs it for its
   own quote bug (W3).
3. **Controller-shaped lookup** (`c201af43`'s `DictionaryWordSelectActivity`): a lookup that owns no picker, just a
   `lookupText` and a busy popup, keeps `Dictionary` alive for the definition's recursive back stack. That is the
   cheapest way to add a phrase path without a second dictionary instance.
4. **Popup ordering idiom** (`ad24aea8`): render the confirm popup before/with the list and use
   `wasLongPressed(Confirm, ms)` rather than reading `getHeldTime()` on release. CPYOS's `OptionPopup.processRender()`
   already draws on top, so nothing to fix today — but if a clipping/phrase **list** is ever added, copy this shape:
   it is precisely the bug class CPYOS hit with the bookmarks tail-button (AGENTS §28.4).
5. **One source-geometry module** (`SelectionGeometry.h`: `nearerWord`, `actions`, `actionAt`) — CPYOS keeps the same
   math inline twice (`:1525-1539` `beginSelection` and `:1331-1345` `wordIndexAt`) and the chips in a third place.
   Extracting it is optional polish; only do it while touching the file.

### 3.2 Source ranges: evaluate, then reject

Upstream stores `TextBlock::SourceRange{uint32 start, end}` **per word** in the block arena (`d7fcc7ce`,
`TextBlock::arenaSize` +8 B/word, `wordSourceRange(i)`), which makes
- highlight matching exact without re-deriving offsets (`drawClippingHighlights` → `clippingContainsWord`), and
- cross-page clip export ordered by source, not by visual order (`buildSelectedText` sorts by `startOffset`).

Cost in CPYOS terms: the arena is currently 5 B/word (`textOff` 2 + `xpos` 2 + `style` 1); +8 B is a **+160 %**
arena, on the target whose section build already stalls on ~64 KB CSS against ~48 KB internal RAM (AGENTS §33.2).
Every cached section is invalidated (`SECTION_FILE_VERSION` is 88/87 at `Section.cpp:76-78`), and `PdfReaderActivity`
shares the store.

A compact alternative, if anchoring is ever fixed: **1 byte/word** `gapBefore` (clamped 0..255), which is enough to
reconstruct exact offsets as `line.visibleOffset + Σ(chars + gapBefore)`. That fixes W2 for **+20 %** arena, not
+160 %, and needs no u32 pair. Note the style byte cannot carry the flag for free — its high bits are already
`RUBY_CONTINUE`/`UNDERLINE` (`TextBlock.h:121`, `TextBlock.cpp:223`), and the Section format would have to change
anyway.

**Does phrase lookup need source ranges?** No. It needs the *selected words' text plus their order*, both of which
`layoutPageWords()` already produces; offsets are only used to decide separators (2.3) and, later, to re-anchor a
saved phrase. So phase A can ship with zero layout changes.

### 3.3 Reject: the clipping subsystem itself

`src/ClippingStore.{h,cpp}` (691 + 128 lines) duplicates `AnnotationStore`: per-book file, 1024/256 records,
`id[65]`, `pendingUpload`/`syncRevision`, `timestamp`, `layoutSignature`, `startPage/endPage/startWordIndex`, text
on SD (`CLIPPING_TEXT_MAX = 4 KB`), `prepareSync`/`finishUploads`/`applyRemote`/`nextDeletion` for KOReader sync,
plus `ClippingPreview.h`, `ClippingSync`, `EpubReaderClippingListActivity`, WebDAV/webserver hooks. CPYOS already has
a list (`EpubReaderBookmarksActivity` + the panel in `ReaderToolbarUi`), atomic replace, PDF reuse, and export.
Taking it would create **two** stores for the same user object and a second on-disk format to migrate.

Also rejected: the 3-page selection window (`CLIP_SELECTION_PAGE_WINDOW = 3`, `startClipSelection`) — it exists to
allow cross-page clips and hold-to-page-turn; useful only if cross-page selection is a product goal, and it triples
page memory while the bar is open. Revisit as its own change, not as part of lookup.

---

## 4. Phased plan

### Phase A — phrase lookup button (1.5–2 days, low risk)

| File | Change |
|---|---|
| `src/util/SelectionText.h` (new) | `joinSelection`, `normalizePhrase`, `phraseCandidates`, `kMaxPhraseBytes = 64`. Pure, no HAL. |
| `src/activities/reader/EpubReaderActivity.{h,cpp}` | 3rd chip + default slot; hit arrays 2→3; key cycle 0..2; `startPhraseLookup()`; `endSelection()` before pushing; reuse `STR_LOOKUP`. Optionally route `commitSelection`'s quote through `joinSelection` (fixes W3 in the same edit). |
| `src/activities/reader/DictionaryWordSelectActivity.{h,cpp}` | `PhraseQuery` ctor, `lookupText_`, `onEnter()` early path; candidate ladder inside `performLookup()` (≤3 Latin / ≤4 CJK); hint line for a fallback hit. |
| `lib/I18n/translations/{english,chinese}.yaml` | `STR_DICT_PHRASE_FALLBACK` (others fall back to English; pack rebuild → full data-partition flash). |

Tests:
- **Host** `test/phrase_query/run_phrase_query_test.sh` (same shape as `test/dictionary_selection/run_dictionary_selection_test.sh`:
  bare `g++`, `-Werror`, include `-I lib/Utf8 -I src`): CJK run vs Latin join, paragraph `'\n'`, glued punctuation,
  em-space/U+00A0 prefix, trailing `'-'`, 64-byte clamp on a token boundary, empty/punctuation-only rejection,
  candidate ladders for both scripts, and a mutation step (drop the CJK-gap rule ⇒ a Chinese phrase must join with a
  space and the test must go red).
- **Device** (serial): add `CMD:PHRASE` that opens the 3-chip bar on the current page (a debug hook like the existing
  `CMD:TEXT`/`CMD:PAGENEXT`), `CMD:SCREENSHOT` for the chip row, then the manual script: long-press a 2–3 word Latin
  phrase → `查词` → definition → Back lands on the same page and paragraph; repeat on a CJK 4-character run; repeat
  with a non-headword phrase to see the fallback hint; `BACK` from the bar must not advance the page.
- Regression: `exp/refresh_soak.py` on the device after the build (it also exercises the reader panel open/close and
  asserts no band/underrun regressions), plus `test/dictionary_selection` (the tokenizer contract shared with the
  definition viewer).

### Phase B — anchoring/selection improvements (2–3 days, only if phase A shows drift or a user reports it)

| File | Change |
|---|---|
| `lib/Epub/Epub/blocks/TextBlock.{h,cpp}` | `gapBefore` u8 per word in the arena + `wordGapBefore(i)`, serialized; arena 5→6 B/word. |
| `lib/Epub/Epub/ParsedText.{h,cpp}`, `parsers/ChapterHtmlSlimParser.cpp` | compute and pass the gap per token (the parser already knows whether whitespace separated the words: `wordNoSpaceBefore`). |
| `src/activities/reader/EpubReaderActivity.cpp` | one `computeWordOffsets(block)` used by `layoutPageWords`, `drawAnnotations` and `commitSelection` (kills the duplicated `countUtf8Codepoints` loop at `:1101` and `:1311`, helper at `:981`). |
| `lib/Epub/Epub/Section.cpp` | `SECTION_FILE_VERSION` 88→89 (87→88 CJK). |

Tests: host test that reconstructs offsets from (gap array, word texts) and compares to a fixture table; device test
that highlights a **mid-line** word, changes the font size, and re-checks the highlight bounds (screenshot diff or
`CMD:` dump of the annotation range vs `layoutPageWords`), plus a round-trip of the section cache.

### Phase C — highlight/UX parity (0.5 day, optional)

Nothing to fix in rendering: CPYOS's 25 % dither wash (`:1115`) already reads as a continuous band, so upstream's
band-merging (`previousHighlightX/Width` in `drawClippingHighlights`) buys nothing. The only parity gap worth spending
on is a **"look up again"** action on the annotation list row (`:6407` builds the row subtitle from the stored quote) —
it must re-derive the text from the live page by `spineIndex`+offsets (W6), not from the truncated quote.

---

## 5. Explicit rejects

1. **`src/ClippingStore` and everything hanging off it** (preview/sync/list/WebDAV): duplicates `AnnotationStore`,
   adds a second on-disk format, a 4 KB-per-clip SD payload, page/word ordinals and a `layoutSignature` fallback that
   CPYOS's chapter-wide offsets make unnecessary.
2. **`TextBlock::SourceRange` (8 B/word)**: +160 % block arena, full section-cache invalidation, and phrase lookup
   does not consume it. If exact anchors are ever needed, use the 1 B/word `gapBefore` (Phase B).
3. **`ClipSelectionActivity` as a separate Activity / 3-page preload**: CPYOS's in-place selection is cheaper (no extra
   Activity, no page triple, existing handles and key control). Cross-page selection is a separate product decision.
4. **`ad24aea8`'s delete-popup fix as code**: the bug it fixes (popup rendered after the list, so the delete
   confirmation flickers) does not exist in CPYOS's `OptionPopup` path; take the idiom only if a list is added.
5. **KOReader clipping sync**: separate feature, needs `ClippingSync` and its network surface; out of scope here.


---

# 圆角/安全区/设备抽象设计

# Absorbing upstream's content-inset / corner-radius work into CPYOS (design only)

Read-only audit. Worktree `/Users/hongbin/Documents/projects/cpyos/.worktrees/upstream-sync` @ `b39f3706`.
Upstream refs: `01f09e0e` (content insets + corner radius), `fb2a8a8f` (Picco/Sticky displays),
`8f4d035c` (Picco switch → hardware setting), `261726f6` (BOOX Picco), `809c1dab` (LilyGO expander).

Verdict: **take the abstraction (panel-derived corner/inset metrics + one pure geometry helper), set
`cornerRadius = 0` until the Read Pico corner is measured on glass, and ship no board code.** Two findings
below correct the earlier KEEP_REJECTED and change the cost from "8 files + freeink-sdk bump" to "one helper,
one theme accessor, four call sites, no SDK change".

---

## 0. What changed since `docs/plans/2026-11-08-upstream-bucket3-reaudit.md:119`

The re-audit said `cornerRadius` / `headerOffsetY` / `tabBar*Pct` **do not exist at pin `96de1be`** and that
adopting `01f09e0e` needs a freeink-sdk bump. Both are stale:

- The gitlink is now **`d9a1304`** (`git ls-tree HEAD freeink-sdk`) — an `0x1abin` commit on
  `origin/main` of `0x1abin/freeink-sdk` — and `BoardConfig.h` at that commit already carries `headerOffsetY` (:965), `cornerRadius` (:969), `tabBarOffsetY/HeightPct/WidthPct` (:974-976),
  `wifiMaxTxPower` (:978), `usbDetectActiveHigh` (:983), `statusBarOffsetY` (:987) and
  `bottomCornerRadius` (:991).
- `patches/freeink-sdk/d9a1304c-local.patch` **does not touch `BoardConfig.h`** (`git -C freeink-sdk diff` on it
  is empty), so those fields are pure upstream: **no SDK patch, no gitlink bump, no board-profile surgery.**
- The re-audit also wrote "`READ_PICO` viewableInsets = 0". It is `{5, 5, 8, 5}` (`BoardConfig.h:2317`).

So the cost of the *generic* half is now purely app-side. What still blocks a nonzero radius is not code — it is
that nobody has measured the panel.

---

## 1. Panel truth

### 1.1 What the repo states

| fact | source | note |
|---|---|---|
| Read Pico `viewableInsets = {5,5,8,5}` (portrait TRBL) | `BoardConfig.h:2317` | applied only via `GfxRenderer::getOrientedViewableTRBL` (`lib/GfxRenderer/GfxRenderer.cpp:4542-4582`, reads `BoardConfig::ACTIVE.viewableInsets` generically) |
| those insets are **layout insets, not measured bezel dimensions** | `docs/engineering/read-pico.md:223-227` | the doc's own words; the old `{9,3,24,3}` at `:333-335` was "provisional" |
| `cornerRadius` for `READ_PICO` | `BoardConfig.h:2250-2320` | the profile's aggregate init **stops at `grayscaleLevels = 16` (:2320)**, so `cornerRadius` stays at its default `0` |
| panel / bezel corner radius anywhere in the tree | — | **not measured anywhere**; `docs/`, `docs/images/`, `site/` hold framebuffer screenshots and no glass photo |

`ReadPico` is therefore the one board where CPYOS has *insets but no radius*.

### 1.2 Photo evidence (first measurement, `artifacts/photo_gate/`)

The dev tree's photo set (HEIC→PNG, 3072×4096) is a real photo of the running device, not a screenshot:
`artifacts/photo_gate/demo0.png` and `read_131727.png`. The **bezel aperture is visibly rounded** — the seam
between the white printed frame and the grey panel is a clean arc, tens of pixels across (crop
`(1000,440)-(1320,760)` of `demo0.png`).

Measured on `demo0.png` (panel-vs-bezel mask at grey 148–170, then a least-squares circle fit on the aperture
boundary near the top-left corner):

- aperture quad fitted from the straight mid-edges: TL `(986, 379)`, TR `(2734, 405)`, left edge 3283 px,
  top edge 1748 px (oblique photo ⇒ 5% aspect error, so absolute scale is ±10%);
- scale at that corner: **≈2.6 photo px per panel px** (674 visible columns ≈ 1748 px);
- circle fit through the corner boundary: `r_photo = 87.6 px` (residual RMS 25 px — the fit is noisy; the
  straight-edge departure points give 44 px top / 111 px left, so the truth is bracketed).

**Estimate: `r ≈ 30 panel px`, usable range 20–45** — i.e. 1.6–3.7 mm of arc on a 4.7" panel, and 4–7% of the
684 px axis. That is a *visible* rounding, not the "very slight" one the glass itself may have: what clips
content is the bezel aperture, and that is what this measures.

What matters for layout is not `r` but the corner *cut depth*, `cut(d) = r - sqrt(r² - (r-d)²)`, where `d` is the
distance of the nearest ink row from that edge:

| `r` | cut at `d = 8` | cut at `d = 12` | cut at `d = 20` | cut at `d = 30` |
|---:|---:|---:|---:|---:|
| 8 | 0.7 | 0.2 | 0 | 0 |
| 30 | **9.6** | 6.0 | 1.7 | 0 |
| 40 | 14.7 | 9.0 | 2.8 | 0 |

So a 30 px aperture radius eats up to ~10 px of the bottom-left/right corner rows and nothing elsewhere. That is
exactly the class of bug `01f09e0e` fixes, and it is small enough to explain why nobody has noticed:
`baseStatusBar` clusters sit at `margin + 1` from the left edge and their lowest row is 8–12 px above the safe
bottom (`BaseTheme.cpp:1122-1140`).

### 1.3 Where CPYOS actually draws near a corner today

| surface | geometry | corner risk |
|---|---|---|
| mast (title band: duck + clock/date + battery pill + hairline) | `CpyTheme::drawMainTabTitle` (`CpyTheme.cpp:967-1070`), rect from `MainTabs::layout` | none: band starts at `safe.y + topPadding` (≥21 px) and text at `rect.x + kRowPadding` |
| bottom nav (wave line + icon/label block) | `CpyTheme::drawMainTabBar` (`CpyTheme.cpp:580-650`) | none: wave is at `tabRect.y`, 96 px above the edge; labels are centred per tab |
| low-DPI status strip | `CpyTheme::drawMainTabStatusBar` (`CpyTheme.cpp:1096-1160`), `edgeInset = 32` | none (32 px ≫ cut) |
| **reader→PDF/CBZ/XTC status bar clusters** | `BaseTheme::drawStatusBar` (`BaseTheme.cpp:1101-1140`), `leftClusterX = margin + 1` | **yes** — the only corner-resident text lane; already carries a hardcoded EEGO clamp (`:1127-1136`, `constexpr 28`) |
| reader footer (chapter title / page numbers / wave) | `EpubReaderActivity::readerChromeMetrics` (`EpubReaderActivity.cpp:5392-5410`), `m.left = viewLeft + screenMargin`, `m.pageY = H - viewBottom - 8 - lineH` | **marginal** — 5 px from the side, lowest row ~8 px above the inset bottom |
| classic-mode library readout ("12 / 340") | `LibraryListActivity::drawPositionReadout` (`:2213-2235`), right-aligned at `screenWidth - width - SIDE_PADDING(12)` on the `screenH - buttonHintsHeight - lineH` row | marginal (right corner, 12 px in). CPY mode is exempt: the same readout moves two line-heights up into the content margin (`:2230-2235`) |
| toast capsule | `Toast::paint` (`src/components/Toast.h:20-40`), `pillY = safe.y + safe.height - 56 - inset` | marginal at the extreme corners only |
| screenshots / sleep / images | full-bleed by design (`ScreenshotUtil.cpp:91-101` draws its own border at the TRBL rect) | physical crop, not fixable and not fixed upstream either |

Nothing draws **ink** into the corner *arc region* today except those four bottom clusters/lanes, all of which
keep ≥5 px. That is why the radius can ship as a data-only change and why it must not be applied globally
(§2.4).

---

## 2. Generic abstraction

### 2.1 Ownership: board profile is truth, theme is policy

Keep the number in `BoardConfig::ACTIVE` (it is a property of the glass, not of a design). Add one accessor so
app code never reads `BoardConfig` directly (Golden Rule 8 / HAL boundary):

```cpp
// src/components/UITheme.h
int getCornerRadius() const;        // top corners: BoardConfig::ACTIVE.cornerRadius
int getBottomCornerRadius() const;  // BoardConfig::ACTIVE.bottomCornerRadius ?: cornerRadius
```

Both return `0` on X3/X4/Read Pico, so every consumer below is a no-op there.

### 2.2 One pure helper, not `std::max(margin, corner)`

Upstream's rule is coarse: `max(orientedMarginLeft, corner)` (`01f09e0e`, `BaseTheme.cpp` status bar). For a
30 px radius that shifts the reader footer 25 px inward for a defect that is at most 10 px — a visible
regression in exchange for correctness. Use the actual cut instead:

```cpp
// lib/GfxRenderer/PanelInsets.h  (pure, no HAL, host-testable)
struct PanelInsets { int top, right, bottom, left; };
// Horizontal space the rounded corner steals on a row `depth` px in from that edge.
// depth >= radius => 0; radius == 0 => 0. Isqrt so it needs no libm.
constexpr int cornerCut(int radius, int depth);
// max(straight-edge inset, cornerCut(radius, depth-of-nearest-ink-row))
constexpr int sideClearance(int inset, int radius, int depth);
```

That is the whole abstraction: two constants per edge (`viewableInsets`) plus one radius per corner pair, and a
closed-form clamp. `cornerCut` is monotonic in `radius` and `0` at `radius == 0`, which is the byte-identity
proof we need (§2.4).

### 2.3 Consumers, and what each becomes

1. **`BaseTheme::drawStatusBar`** (`BaseTheme.cpp:1101`) — replace the EEGO-only block (`:1127-1136`) with
   `leftClusterX = max(leftClusterX, cornerCut(bottomR, nearestRow(depth)))`, same on the right. Keep the
   `textY -= 4` lift EEGo needs as its own profile data, not as a `#if`. *(For EEGO the numbers must be
   re-checked: profile says 24/40, the hardcode says 28.)*
2. **`EpubReaderActivity::readerChromeMetrics`** (`:5392`) — `m.left = max(viewLeft + screenMargin,
   cornerCut(bottomR, pageH + 8))`, `m.right` symmetric, so the footer's outer glyph cannot land in the arc.
3. **`CpyTheme::drawMainTabTitle` / `drawMainTabBar`** — clamp only the elements that hug the band ends (duck
   origin, battery pill right edge, wave line ends) with the *same* helper. On Read Pico nothing moves (the
   band's rows are ≥21 px deep), which is the point: the rule exists for the next device.
4. **`LibraryListActivity::drawPositionReadout`** and **`Toast::paint`** — same clamp on their single edge
   coordinate.
5. **`MainTabs::layout`** (`src/activities/MainTab.h:69`) — *do not* add radius arithmetic here. It already
   takes an inset `safeArea`; the corner is a per-row property, so it belongs at the draw sites, not in the
   band partition. (Upstream put `max(safe.x, corner)` into `CoverGridHomeUi` because its grid columns do
   reach the corners; CPY's cover grid is `LibraryListActivity`, whose rows are inset by `rowInset` already.)

### 2.4 Keeping Read Pico byte-identical at `radius = 0`

Three rules make `radius == 0` provably inert:

- every expression is `max(existing, cornerCut(0, d))` and `cornerCut(0, ·) ≡ 0`;
- no `+ radius` anywhere, no `- height` change (upstream's `headerOffsetY` does both — excluded, §2.5);
- the helper is `constexpr`, so a host test can assert `sideClearance(i, 0, d) == i` over the shipped TRBL sets
  for Read Pico (5/5/8/5), X4 (9/7/3/7) and X3, at `d ∈ 0..64`.

**Do not port `BaseTheme::headerBand()`.** Upstream's band both *insets* the caller's rect by the viewable
margins and *bleeds* the fill/rule back out (`props.bleed`), because upstream's callers pass full-width rects.
CPYOS's callers already pass safe-area rects — `WifiSelectionActivity.cpp:1139,1147`,
`CatalogScreens.cpp:26-56`, `LibraryListActivity::buildHeader` (CPY, which does not draw the fui header at
all) — and `drawHeaderWithStyle` uses `rect` verbatim for the band (`BaseTheme.cpp:659`). Porting `headerBand`
would double-inset every header on a bezel board. The one line worth taking is the *status* clearance
(`01f09e0e`'s `props.status.edgeInset += corner - min(left,right)`), which lands in `applyHeaderStatus`
(`BaseTheme.cpp:596`) as `status.edgeInset += cornerCut(r, statusRowDepth)`.

### 2.5 Explicitly deferred board knobs

`headerOffsetY`, `statusBarOffsetY`, `tabBarOffsetY`, `tabBarHeightPct`, `tabBarWidthPct` exist in the SDK but
every board CPYOS ships leaves them at `0/0/0/100/100`, i.e. identity. Adopting them means threading five more
numbers through the CPY mast and nav for zero target. Defer until a board with a tab-fit override is actually
built; then apply them in `MainTabs::layout` (the single band partition), not per page.

---

## 3. Device groundwork — adopt / defer / trap

| item | status at `d9a1304` | decision |
|---|---|---|
| `BoardConfig::ACTIVE` profile fields (`cornerRadius`, `bottomCornerRadius`, `viewableInsets`, `statusBarOffsetY`, `tabBar*`) | **present** | **adopt** the two corner fields now (data only); the rest deferred (§2.5) |
| runtime profile switching (`selectDevice()` `BoardConfig.h:2487`; C3 binary already holds X3+X4 and picks at boot, `lib/hal/HalGPIO.cpp:182-190`, `XteinkDetect.cpp:698-704`) | present and exercised | **pattern is safe**: a new S3 board = profile + env + one boot-time `selectDevice`. No app change. |
| display-variant hook (`displayControllerVariant`, `BoardConfig.h:946`; `Board::XteinkX3Uc8279`) | present, unused by Read Pico | **defer**. CPYOS's real variant predicate is a hardcoded family test (`EpubReaderActivity.cpp:122-125`, `xteinkClassPanel()`); generalizing it into capabilities needs a third panel to validate against. |
| `FREEINK_CAP_FRONTLIGHT` / `FrontlightManager::present()` | `READPICO` is **excluded** from the cap list (`BoardConfig.h:309-313`), so `present()` compiles to `return false` (`FrontlightManager.h:99-102`) | **use for feature rows only** (brightness/warmth), never to gate the panel — see §5 |
| hardware switch plumbing (`InputManager::isToggleSwitchOn()` `InputManager.h:268`, `ToggleSwitch` config `BoardConfig.h:684`) | present; **no consumer in `src/`** (`grep -r SlideSwitch src lib` → 0) | **defer**. Safe shape if a board ever has one: position → owned function in one `util/` header, mirrored from upstream `SlideSwitch.h`; Read Pico has no switch. |
| `startDeepSleep(..., wakeOnHeldButton)` (`261726f6`) | **absent** at this pin | **reject**; Read Pico's PMU hand-off already gives wake-by-key (`docs/engineering/read-pico.md`, `BoardReadPico` `pmuTakeKeyWakeup`) |
| `BwFrameKeeper` (`fb2a8a8f`) | mechanism exists (`storeBwBuffer`, used by `ScreenshotUtil.cpp:91`) | **inert** where absolute gray exists — Read Pico is `grayscaleLevels = 16` (`BoardConfig.h:2320`), so `grayscaleCapabilities(...).supported()` is true and the keeper never engages. Reject with the board. |

---

## 4. Phased plan

### (a) Safe-area metrics with zero visual delta — ~1 day

| file | change |
|---|---|
| `lib/GfxRenderer/PanelInsets.h` (new) | `cornerCut` / `sideClearance`, `constexpr`, no HAL, `isqrt` of integers only |
| `src/components/UITheme.{h,cpp}` | `getCornerRadius()` / `getBottomCornerRadius()`; also fold the `#if FREEINK_DEVICE_READPICO` block at `UITheme.cpp:177-181` into "read TRBL, then the theme's opt-in flag" so the next board does not add another `#if` (keep C3's current behaviour bit-for-bit) |
| `src/components/themes/BaseTheme.cpp` | EEGO `#if` → `sideClearance(...)`; `applyHeaderStatus` status edge inset |
| `EpubReaderActivity.cpp`, `CpyTheme.cpp`, `LibraryListActivity.cpp`, `Toast.h` | clamps per §2.3 (all inert at 0) |
| `test/safe_area/test_panel_insets.py` (new, modelled on `test/inx_navigation/test_main_tab_title_band.py`) | compiles `PanelInsets.h`: identity at `r=0` for all three shipped TRBL sets, `cornerCut` table from §1.2, monotonicity in `r` and `d`, `d ≥ r ⇒ 0`, `isqrt` exactness on the radius² boundary; plus a **mutation step** (drop the `depth >= radius` guard → must go red) |

Acceptance: `readpico` and `default` build; `exp/refresh_soak.py` 36/36; **no** pixel change in an A/B
screenshot set — literally compare `CMD:SCREENSHOT` frames of home / library / reader / settings before and after
(they must be byte-identical, which is the only honest proof that a `max()` refactor is inert).

### (b) Apply the tiny radius if it is real — ~0.5 day + one photo session

1. Confirm `r` on glass. A `CMD:PANELEDGE`-style dev draw (1 px frame at each of `0,2,4,…,32` px inset plus a
   diagonal ruler in each corner, full-screen refresh) photographed at 1:1 gives `r` directly, or the panel
   drawing from the vendor kit does. **Do not guess a number into `READ_PICO.cornerRadius` from §1.2's photo.**
2. Set `cornerRadius` (and `bottomCornerRadius` if the bottom differs) in `BoardConfig.h`'s `READ_PICO` block —
   that is a freeink-sdk **working-tree** edit, so it must go into `patches/freeink-sdk/d9a1304c-local.patch`
   with its own idempotence guard, exactly like the display patches (AGENTS §15).
3. Re-shoot the four corner-adjacent surfaces and diff against (a): the only expected deltas are the
   footer/status-bar clusters, by ≤10 px.

If the measured `r` is ≤ 8 px, stop at (a) and record `cornerRadius = 0` with the measurement in
`docs/engineering/read-pico.md`; the cut at 8 px is <1 px, below the panel's own edge tolerance.

### (c) Future-board plumbing — 1–2 days per board, only with hardware

Profile + env + `selectDevice` at boot (S3 boards stay compile-time), then *only* the `cornerRadius` /
`viewableInsets` numbers need entering. Everything in §2 then follows without touching a page.

---

## 5. Explicit rejects

1. **Any board code CPYOS cannot flash** — `PICCO`, `STICKY`, `EEGO_A4`, `MURPHY_M4`, `X4PRO`, `X4C`,
   `LILYGO` (`platformio.ini` still defines 43 `[env:]` / those `*_hardware` sections; only `default` (X3+X4)
   and `readpico*` are built or shipped). That rejects all of `261726f6`, `fb2a8a8f`, `8f4d035c`'s device
   halves and all of `809c1dab` (LilyGO T5S3 expander → `HalGPIO` down-input; no expander on X3/X4/Read Pico).
2. ★ **`fb2a8a8f`'s `Frontlight.present()` gate** — `ActivityManager::loop()` would gain
   `if (Frontlight.present() && …)` around the control-centre push. On Read Pico `present()` is hard `false`
   (`FREEINK_CAP_FRONTLIGHT` excludes `FREEINK_DEVICE_READPICO`; `FrontlightManager.h:99-102`), and CPYOS opens
   that panel from the top-edge swipe with no gate (`ActivityManager.cpp:254`,
   `MappedInputManager::wasLightPanelGesture()` `:536-538`, comment: *"The control center also serves touch
   boards without a frontlight"*). The panel's four rows are Night mode / Force refresh / Orientation /
   Touch controls (`FrontlightPanelActivity.cpp:405-419`) — none of them need a light. **The gate deletes a
   shipped feature**; reject permanently, and use `FREEINK_CAP_FRONTLIGHT` only to hide the brightness/warmth
   rows.
3. **`BaseTheme::headerBand()` as written** (§2.4) — bleed-plus-inset assumes full-width callers that CPYOS
   does not have.
4. **`headerOffsetY` / `statusBarOffsetY` / `tabBar*Pct`** — identity on every shipped board; adding them now
   breaks the byte-identical guarantee for no target.
5. **Guessing `cornerRadius = 8`** "because the user said slight". The number must come from the glass or the
   panel drawing; a wrong value silently moves the reader footer.

---

## 6. Measurement appendix (method used, reproducible)

```python
# panel mask: grey 148..170 separates the panel (152..162) from the bezel (190..200)
# and the table (176..186); fit the 4 straight mid-edges, intersect => aperture quad,
# then Kasa circle fit on boundary points within 170 px of the aperture corner.
# scale = aperture edge length / (visible panel px), here 1748/674 = 2.59
```

Photo set: `artifacts/photo_gate/{demo0,read_131727,read_131746,read_131813,demo1}.png` (3072×4096, oblique ⇒
±10% scale, hence the 20–45 px band). A 1:1 photo of a drawn ruler removes the obliquity and should be the
acceptance artefact for step (b); store it next to `artifacts/photo_gate/` and cite it in
`docs/engineering/read-pico.md` so the next person does not re-derive this.


---

# UI/设置：23 项新标准重分类

# Reclassification of the kept-rejected UI/settings items (stricter principle)

Scope: the 23 `KEEP_REJECTED` items of `docs/plans/2026-11-08-upstream-kept-rejected-review.md` §4
(reader interaction / settings), at HEAD `b39f3706`, `upstream/main` = `a6fd569f`.

New rule: **REJECT only when a change is BOTH conflicting AND unnecessary.** Each verdict below was
re-derived by reading the diff against the current CPYOS code (file:line cited), not by re-quoting §4.

Verdicts: `ABSORB_NOW` / `ABSORB_PARTIAL` / `PLAN_LATER` / `REJECT`.

| sha | what changes | conflict? | necessary/useful? | verdict | route |
|---|---|---|---|---|---|
| `471243b2` | `confirmLongPressThreshold()` returns 0 when `BoardConfig::hasTouch()`, i.e. kills long-press menu/ dictionary/bookmark on every touch board | **yes** — readpico `hasTouch()` is true; long-press Confirm drives bookmark/dictionary/menu (`EpubReaderActivity.cpp:2568/3189`) | no — it removes a shipped feature | **REJECT** | Deliberate CPYOS decision: Read Pico keeps the long-press menu; CPYOS hides nothing on touch boards. Do not re-open unless CPYOS ships a board whose touch replaces long-press. |
| `5407ffcd` | Moves the long-press-menu hiding out of `SettingsActivity::showOnDevice` into `SettingsList::getSettingsList` (single filter site) | **partial** — the `hasTouch` predicate conflicts, the *move* does not | yes — CPYOS still has a stray filter at `SettingsActivity.cpp:472` while every other board filter lives in `SettingsList::isSettingAvailableOnBoard()` (`SettingsList.h:788-800`) | **ABSORB_PARTIAL** | Move only that one predicate into `isSettingAvailableOnBoard()` with CPYOS's `BoardConfig::hasHomeKey()`; drop the `hasTouch` half. 3 lines, no behavior change. |
| `b823d54b` | Reorders `toggleSetting()`/`settingValueText()`/web API so a getter/setter pair wins over `valuePtr`; adds `touchControlsVersion` + migration that moves the Inverted-Tap side swap into `tapZones`; `buildShortOptions()` uses `gestureAllowsTap` | **yes** — CPYOS's `pageTurnGesture` rows are plain `SettingInfo::Enum` with 5 labels (`SettingsList.h:486-493`), so stored value == row index and there is no DynamicEnum ordering bug; CPYOS also folds `INVERTED_TAP` at *read* time (`flip = mirrored != invertedTap`, `ReaderUtils.h:167`) instead of rewriting zones | partially — the branch-order guard is cheap insurance for a future DynamicEnum that also sets `valuePtr` (CPYOS `DynamicEnum()` today leaves it null, `SettingsActivity.h:186-200`) | **ABSORB_PARTIAL** | (a) Put the getter/setter branch before the `valuePtr` branch in `toggleSetting()` / `settingValueText()` / `CrossPointWebServer` GET+POST, with upstream's comment. (b) Skip `touchControlsVersion`, the migration rewrite and the zone swap — CPYOS's read-time fold already gives the same on-screen result. |
| `e41982a4` | Makes the touch migration leave a saved gesture pair intact; zone swap fires when *either* direction holds Inverted Tap | **yes** — CPYOS migrates only when *both* keys are absent (`CrossPointSettings.cpp:578-589`) and never rewrites a stored pair; the either-direction swap already exists at read time (`ReaderUtils.h:164-167`) | no — the bug it fixes cannot occur | **REJECT** | Deliberate CPYOS decision: `INVERTED_TAP` is a read-time flip; the migration never overwrites a saved pair. Re-open only if CPYOS adopts upstream's "write the swap into the zones" model. |
| `c7996b9f` | `entryCount` 87→88 for the new page-bottom-align row | **yes** — CPYOS's count is its own (`SettingsList.h:239`, base 98 + capability guards) and has already drifted | yes — invariant, not a port: an under-sized `reserve()` makes `-fno-exceptions` OOM-fatal and loses the C3 headroom | **ABSORB_PARTIAL** | Do **not** port the number. Add the upstream invariant to CPYOS's table: a `static_assert`/constexpr check that `entryCount ==` the actual batch sum, plus fix `test/settings_api/test_settings_api_keys.py` (see below). |
| `35dccb87` | Splits a 5-item `v.insert()` batch into 2+3 (batches stay ≤4) | **yes** — CPYOS's own batches differ | yes — the ≤4 batch rule is what keeps C3 stack frames small (`SettingsList.h:259` comment) | **ABSORB_PARTIAL** | Apply to CPYOS's tables where a batch exceeds 4; enforce with the existing test assertion (`test_settings_api_keys.py:160`) once that test runs again. |
| `e9635421` | `entryCount` 88→89 for the TapZones row + comment that the count must move with the entry | **yes** — same as `c7996b9f` | yes — invariant + "say why the number moved" | **ABSORB_PARTIAL** | Same route as `c7996b9f`: make the count self-checking at compile time in CPYOS rather than hand-maintained. |
| `87fc4ab7` | New `SettingsListActivity` root hub + `SettingsSection.h`; `SettingsActivity` reduced to one group per screen; icons per group | **yes** — CPYOS uses 4 capsule tabs + category headers (`CpyTheme` shelf tabs), verified on device; the commit rewrites `SettingsActivity.cpp` (340 lines) and `main.cpp` routing | **partly** — the upstream complaint is real (a long flat list), and CPYOS's largest tab is still long | **PLAN_LATER** | UX decision, not a port: measure rows per CPYOS tab (base 98 + capability rows). If one tab exceeds ~20 rows, add a *section hub inside that tab* reusing `UiListActivity` + CPYOS capsule rows — never the upstream `SettingsListActivity`/`SettingsSection` files. |
| `ec32b959` | INX sharing of page-selection actions: adds `defersContentSelection()`/`hasContentSelection()`, makes `showMainTabContentSelection()` virtual, **deletes `InputModality` from `HalGPIO`**, folds the tab-focus repaint into the release/action frame | **yes** — CPYOS uses `InputModality` for `SelectionCursorPolicy` (`UITheme.cpp:147`) and hardware-key tab focus (`main.cpp:2651`); INX-only pieces have no target | yes for one hunk — CPYOS has the identical pre-fix code at `ActivityManager.cpp:489-500`: the first content tap while `mainTabFocus==Tabs` calls `requestUpdate()` purely to repaint a cursor CPY never draws (`Activity.cpp:34-37` → `showSelectionCursor()` false on CPY touch), i.e. a wasted whole-page e-ink scan | **ABSORB_PARTIAL** | Extract only the repaint guard: skip `requestUpdate()` when `!UITheme::getInstance().showSelectionCursor()` (or move the focus change into the next action frame). Do not remove `InputModality`, do not add `defersContentSelection`. |
| `8b4c0d6e` | INX header status unified with Lyra: new `HeaderStatusMetrics` + `BaseTheme::drawHeaderStatus()` overlay + `headerStatusHeight()`; `MainTabs::statusBarHeight` derived from it | **partial** — CPYOS's CPY home draws its own pill/battery band; the commit changes `MainTabs::statusBarHeight` globally and replaces `HeaderDateUtils::drawHeaderTopLine` when `hasMainTabs()` | yes for the second half — CPYOS's 1-bit subpages (reading stats, `HeaderDateUtils::drawHeaderWithDate`) still draw the legacy 12 px battery + date row, which does not follow the theme's status metrics | **PLAN_LATER** | Design a CPY variant of the shared overlay (sources: `BaseTheme::applyHeaderStatus`, CPY pill metrics) so reading-stats/achievements headers match the theme. Do not import Lyra/`HeaderStatusMetrics` constants or the `hasMainTabs()` branch. |
| `3f8345c5` | `ReaderToolbarUi`: `listProps_.sidePadding` 0 → `tokens.listSidePadding` (rounded-draff theme) | **yes** — CPYOS sets `sidePadding = 0` at `ReaderToolbarUi.cpp:747` deliberately: the CPY cursor bar is drawn in the page margin outside the row, and `rowInset` already matches the title axis | no — the upstream value re-indents CPY rows without a defect to fix | **REJECT** | Deliberate CPYOS decision (§23.6g): cursor is a margin bar, rows stay flush; side padding must remain 0. |
| `b2b1e15a` | Frontlight panel: 2×2 text-label tiles → one row of icon slots (moon / refresh / rotated T / pointer) with on-off circles | **yes** — replaces the CPYOS control-center label tile grid (`FrontlightPanelActivity`, verified on device) | **partly** — labels can overflow the 2-column tile in long translations; icons free space for future quick controls | **PLAN_LATER** | UX decision: measure `tr(STR_TOUCH_TOGGLE)+on/off` and the orientation names against the 2×2 tile width in all 35 translations. If any overflows (or CPYOS adds a 5th quick control), port only the icon-slot renderer into CPYOS's panel; keep the CPY tile metrics. |
| `8f4d035c` | Picco slide switch owns night mode / touch controls / light-on-wake: hides those rows when `slideSwitchOwns*()`; also hides clock rows when `!halClock.isAvailable()` | **partial** — the slide-switch half needs `SLIDE_SWITCH_*` + `SlideSwitch.h` hardware CPYOS does not have (readpico has no slide switch); the clock-availability gate is portable | yes for the clock half only — CPYOS already gates on availability for tilt/frontlight (`SettingsList.h:788-800`), so the clock rows are the one gap; the stored value must survive | **ABSORB_PARTIAL** | Add `if (!halClock.isAvailable())` erasure of the clock rows to `isSettingAvailableOnBoard()` (UI-only, keep persistence). Skip everything Picco/slide-switch; "a hardware switch owns its software row" is a principle already followed, not code to port. |
| `01f09e0e` | Header band/status/bottom clusters respect board insets and `cornerRadius`: extracts `BaseTheme::headerBand()` (bezel + `headerOffsetY` + `bleed` + status edge inset), `max(margin, cornerRadius)` for status-bar clusters, wider tab divider | **partial** — CPYOS inlines the TRBL inset in `drawHeader()`; `BoardConfig` already carries `cornerRadius`/`headerOffsetY` (`BoardConfig.h:965-969`, readpico = 0) | yes as structure — one band helper instead of per-caller math; zero behavior change on the flat readpico panel, and it is the base the safe-area design agent needs | **ABSORB_PARTIAL** (UI half) | Take `headerBand()` + `headerStatusHeight()` + the `max(margin, cornerRadius)` status-bar clusters + `props.leadingStyles`; leave `headerOffsetY` title math and any inset redesign to the safe-area design agent (coordinate, don't duplicate). Flat-panel output must stay byte-identical: verify with the 16-level Home frame diff. |
| `43694c90` | KOSync "server type": 3-state blind cycle → `OptionPopup` with all three names, popup input/render wired into the page | **no** — CPYOS's own `KOReaderSettingsActivity::activateIndex` case 10 (`:81-93`) still cycles CrossPoint→KOSync→Other | **yes** — cycling hides two of three options and is the exact anti-pattern the touch rules ban (`docs/contributing/touch-and-ui.md`, Gold Rule 13/14) | **ABSORB_NOW** | In CPYOS's page: `optionPopup.show(STR_SERVER_TYPE, {CROSSPOINT,KOSYNC,OTHER}, current, ...)`, plus `handleCustomInput()` forwarding and a `render()` that paints the popup (same shape as other CPYOS pages using `InlineConfirm`/`OptionPopup`). |
| `cac12269` | `ActivityManager` ctor/dtor moved out of the header (incomplete `Activity` type); renames `HomeActivity::onSelectBook` → `onRecentBookSelected` | **partial** — CPYOS still has both inline (`ActivityManager.h:100` dtor, ctor inline) and a legacy `HomeActivity` | yes for the move — an inline dtor instantiating `unique_ptr<Activity>` requires a complete type in every TU that includes the header; that is a latent build break with no upside | **ABSORB_PARTIAL** | Move ctor/dtor to `ActivityManager.cpp` + `#include "Activity.h"`; skip the `onSelectBook` rename (CPY home is `InxRecentActivity`; renaming the retired page adds churn). |
| `944b741e` | clang-format of one `Rect menuRect{...}` initializer in `HomeActivity.cpp` | no (pure formatting) | no, as a cherry-pick — CPYOS's file has diverged and the hunk would not land cleanly | **PLAN_LATER** | Fold into a single tree-wide clang-format 21 run on a machine that has it (AGENTS: local box has none); never hand-apply upstream formatting hunks. |
| `31480212` | clang-format of two ternaries + trailing blank lines in `TapZoneSettingsActivity.cpp` | no | no, as a cherry-pick (same reason) | **PLAN_LATER** | Same batch as `944b741e`. |
| `d52fc820` | Deletes `MappedInputManager::wasTapInRect()` ("unused") and its doc row | **yes** — CPYOS still has ~20 callers (`ImageViewerActivity`, `FullScreenMessageActivity`, `Game2048Activity`, `MinesweeperGameActivity`, …) | direction is right (one hit-test source) but the deletion is premature; migrating those 1-bit pages to FreeInkUI hit-testing is its own job | **PLAN_LATER** | Plan: migrate the four hand-rolled 1-bit surfaces to `fui` row/button hit-tests, then delete the helper. Until then keep it — it is the only shared geometry source for those pages (Gold Rule 14). |
| `4ef42ee1` | Guards debug-only prep (`next`/`body` in library paging, `HalMemory::getInternalHeap()` in the dictionary) with `#if defined(ENABLE_SERIAL_LOG) && LOG_LEVEL >= 2` | **no** — CPYOS has the same pattern already (`WifiSelectionActivity.cpp:758`) | yes — CPYOS's `LibraryListActivity.cpp:1398-1405` has the identical unused `next`/`body` in release builds; release-config cppcheck fails on it | **ABSORB_PARTIAL** | Apply the guard to CPYOS's own `buildRows()` debug block; run a release-config cppcheck pass to find siblings. The dictionary hunk has no CPYOS counterpart (no heap logging there). |
| `052d481a` | Test pinning DISABLED(stored 4) through the real load path + real tap classifier, with a stored-3 neighbour as the anti-vacuous control | **partial** — the file lives in upstream's `test/touch_page_turn` (absent here) and its premise (row 3 ≠ value 4) does not exist in CPYOS | yes as intent — "a stored 4 must survive the load and leave the tap branch quiet, a stored 3 must still turn" is a good end-to-end pin for CPYOS too | **ABSORB_PARTIAL** | Write the case in CPYOS's own gesture suite against `ReaderUtils::detectTouchPageTurn()` + `CrossPointSettings::loadSettingsDocument()`; drop the DynamicEnum/`enumValueCount` scaffolding. |
| `5eb498b0` | Regression tests that a mixed gesture pair survives migration (next=Inverted, prev=Disabled stays as saved) | **partial** — upstream's migration rewrites pairs, CPYOS's does not (`CrossPointSettings.cpp:578-589`) | yes as intent — it pins CPYOS's "only expand when both keys are absent / never overwrite a stored pair" guard | **ABSORB_PARTIAL** | Add to CPYOS's own suite: load a pair-era save with mixed values and assert both directions are unchanged and `touchReaderControls`/`tapZones` are untouched. |
| `f3ace194` | Trims redundant cover-loader tests (one case per cached-cover outcome) | **partial** — CPYOS's `test/book_cover_loader` covers the g8 thumbnails, the 10-slot PSRAM cache and path memo, so the upstream cases are not 1:1 | low — pure test hygiene, and over-trimming risks the g8/PSRAM coverage CPYOS added | **PLAN_LATER** | Re-derive the "one case per outcome" table against CPYOS's `BookCoverLoader` on the next touch of that test; do not apply the upstream deletions. |

## Counts

| verdict | count | items |
|---|---:|---|
| ABSORB_NOW | 1 | `43694c90` |
| ABSORB_PARTIAL | 12 | `5407ffcd` `b823d54b` `c7996b9f` `35dccb87` `e9635421` `ec32b959` `8f4d035c` `01f09e0e` `cac12269` `4ef42ee1` `052d481a` `5eb498b0` |
| PLAN_LATER | 7 | `87fc4ab7` `8b4c0d6e` `b2b1e15a` `944b741e` `31480212` `d52fc820` `f3ace194` |
| REJECT | 3 | `471243b2` `e41982a4` `3f8345c5` |
| **total** | **23** | |

Previous: 23 REJECT. Now: 3 REJECT (all three name a deliberate CPYOS decision — long-press menu on
touch boards, read-time `INVERTED_TAP` fold, flush reader-toolbar rows).

## Top 5 that must no longer be "rejected"

1. **`43694c90` — `ABSORB_NOW`.** CPYOS still blind-cycles the 3 KOSync server types
   (`KOReaderSettingsActivity.cpp:81-93`); a popup is a small, local, low-risk win.
2. **`4ef42ee1` — `ABSORB_PARTIAL`.** CPYOS carries the exact unused-variable block the commit fixes
   (`LibraryListActivity.cpp:1398-1405`), so its own release/cppcheck path is already exposed.
3. **`c7996b9f` + `35dccb87` + `e9635421` — `ABSORB_PARTIAL`.** Not a port: CPYOS's capacity invariant
   is *currently broken*: `test/settings_api/test_settings_api_keys.py` dies on
   `unexpected base-list capability guard: defined(CROSSPOINT_FONT_PACK_EMBEDDED)`, and a recount of
   `getBaseSettingsList()` gives 31 batches summing to **109** entries with 5-, 5- and **7**-item
   batches against `entryCount = 98 + guards`. Make the count compile-time-checked and split the
   oversize batches in CPYOS's own table.
4. **`ec32b959` — `ABSORB_PARTIAL`.** One hunk has real value on Read Pico: the first content tap while
   the tab bar has focus forces a whole-page repaint (`ActivityManager.cpp:489-500`) of a cursor CPY
   never draws — roughly one wasted ~450 ms scan per tab visit.
5. **`01f09e0e` — `ABSORB_PARTIAL`.** `headerBand()` / `headerStatusHeight()` / `max(margin, cornerRadius)`
   is the single-source header geometry the safe-area work needs; behavior-neutral on the flat readpico
   panel, so it can land before any inset redesign.

Runner-up worth scheduling with #5: `5407ffcd` (move the last straggler board filter into
`isSettingAvailableOnBoard()`) and `5eb498b0` (pin "a stored gesture pair is never rewritten").


---

# 网络/插件/Web + 构建/发布：20 项新标准重分类

# Reclass — NETWORK / PLUGIN / WEB / BUILD / RELEASE / DOCS (20 commits)

Reclassification of the KEEP_REJECTED rows in
`docs/plans/2026-11-08-upstream-kept-rejected-review.md` §7 (7 commits) and §8 (13 commits),
under the stricter principle: **a commit is rejected only when it is BOTH conflicting and
unnecessary.** Everything else gets a route.

Base: worktree `.worktrees/upstream-sync` @ `b39f3706`, upstream tip `a6fd569f`.
Evidence was taken from both trees (`repos/crossmux` = dev, `repos/cpyos-public` = release target);
wherever a file is cited it was checked in **`repos/cpyos-public`** unless stated otherwise.

**Definitions used below**

- *conflict* — the change cannot land as-is: the target path is absent, or the CPYOS counterpart has
  diverged (packed assets, inline CSS, own README/version/workflow set).
- *necessary/useful* — removes a real defect in CPYOS, or restores a capability CPYOS needs.
- No build/apply was run; `git diff`/`git ls-tree`/`grep` only.

## A. Network / plugins / Web UI (review §7)

| sha | what changes | conflict? | necessary/useful? | verdict | route |
|---|---|---|---|---|---|
| `a722472c` | 1 line: in `OpdsBookBrowserActivity::openPublicationDetail()`, reset the **member** `publication` with `publication = {}` instead of declaring a shadowing local inside `if (!publication.valid)`. | No target: CPYOS `src/activities/browser/OpdsBookBrowserActivity.cpp` has neither `openPublicationDetail` nor `OpdsPublication` (grep empty). | Only a latent-bug rule; no CPYOS defect today. | PLAN_LATER | When a publication/detail path is added (same prerequisite as the unadopted half of `f2120a3f`), assign `member = {}`; put "never shadow a member you then partially fill" on the OPDS checklist. |
| `55a77c2e` | `web.css`: `.web-ui select { appearance: none; padding-right: 36px; }` + inline SVG chevron. | Yes: no `src/network/html/assets/web.css`; CPYOS styles are inline per language page and packed into `CWP1`. | No: CPYOS's only `<select>` (`en/SettingsPage.html:359`) is unstyled/native; the `appearance:none` hits in `en/FilesPage.html:622-623` are **range sliders**, not selects. | REJECT | CPYOS decision: selects keep the native arrow; no custom chevron asset. |
| `e93f8d1f` | Keep the manifest's own `browse.lists` **and** a fetched `lists_url` index (`fixedListCount`), don't refetch on Back to the picker, keep fixed lists usable when the index fetch fails. | No (feature absent, nothing contradicted). | Yes for plugin catalogs backed by a remote folder/tag API — but the base (`e529cadb`) is missing. | PLAN_LATER | Adopt as step 3 of the list-index stack below. Nothing applies today. |
| `d5d0bea1` | Fetch `lists_url` with `lists_headers`, map `lists_items`/`lists_fields`, list `id` → `{list_id}`/`{list_id_enc}`, re-auth once on 401/403, **and bound the vector with `MAX_BROWSE_LISTS = 64`**. | Yes for the mapping half (whole function absent). | Mapping no (needs the stack); **the bounded reserve is a live hazard**: CPYOS `src/activities/plugins/PluginCatalogActivity.cpp:200` does `manifest.browseLists.reserve(browse["lists"].size())` over ≤48 KB of remote manifest JSON (`MAX_API_RESPONSE`), and `BrowseList` is 3 strings + 5 notice strings. | ABSORB_PARTIAL | (a) **now, CPYOS-side**: clamp that reserve and the loop to a `kMaxBrowseLists`-style constant (same value 64) — this is independent of the plugin-system commit and is review §9 item 2. (b) headers/mapping/`{list_id}`/re-auth later with the stack. |
| `861683cc` | `requires` capability array on static **and** fetched lists; hide a list whose requirements the firmware does not advertise; malformed/oversized (>32) array hides it. | Yes: needs `src/util/FirmwareIdentity.h`, `PLUGIN_API_VERSION`, `PLUGIN_CAPABILITIES` — CPYOS has none (`grep` over `src/` is empty). | Not today: nothing advertises capabilities, so filtering would be inert. | PLAN_LATER | Prerequisite model: `firmwareidentity::{FAMILY, PLUGIN_API_VERSION, PLUGIN_CAPABILITIES}` + `catalog.*` vocabulary + `matchesCapabilities`, then `requires` on lists (keep the "malformed → hide" rule). |
| `c9f64d0d` | New `/plugins` page, `web.css` consolidation, `Header.html.inc`, `plugin-host.js` served as an asset, `loadPlugins` error state, `cpPlugins` cookie, Save-disabled-until-edited. | Yes for the layout work: packed per-language HTML, inline `<style>`, no `web.css`/`Header.html.inc`; plugin cards already mount inline in `SettingsPage` (`#plugin-container`). | **Partly — two separable defects.** | ABSORB_PARTIAL | (a) `shared/PluginHost.js.inc:121-126` does `list = await (await fetch('/api/plugins')).json()` then `for (const p of list)`: a 503 `{"error":"out of memory"}` or invalid JSON throws (object is not iterable) and leaves the card area silently blank. Port upstream's `ok`/`Array.isArray` check + "Could not load plugins" state. (b) Serve PluginHost **once**: 8,837 B is inlined into 5 pages (`en/zh SettingsPage`, `en/zh FilesPage`, `RunnerPage`) ≈ 35 KB raw duplication (~20 KB minified); needs a new `WebPackFormat` asset id + schema bump + `/js/plugin-host.js` route + built-in fallback. (c) Reuse the "Save disabled until a field changes" rule only when those widgets are touched. Reject the relocation, CSS consolidation and class renames. |
| `18d611f2` | Per-case temp file in `DictionaryHtmlPagesTest`; `[[maybe_unused]]` for the copied `constexpr` block in `test_recursive_dictionary.py`. | Yes: both files absent (`test/chapter_html_slim_parser/` has `ChapterHtmlSlimParserTest`/`UpstreamParserTest`; no `test/test_recursive_dictionary.py`). | No: CPYOS parser tests already use pid-suffixed temp names (`ChapterHtmlSlimParserTest.cpp:96,342`, `UpstreamParserTest.cpp:27`), and the `[[maybe_unused]] constexpr` rewrite already exists (`scripts/tests/test_ui_font_fallback.py:292`). | REJECT (not applicable; the idea is already applied where it is needed) | — |

### Prerequisite model for the plugin list-index cluster (`e93f8d1f` + `d5d0bea1` + `861683cc`)

These three are a **stack**, not three independent ideas. Porting order:

1. `e529cadb`-equivalent compatibility vocabulary (`FirmwareIdentity.h`, `PLUGIN_API_VERSION`,
   `PLUGIN_CAPABILITIES`, `filtersCompatibility`) — absent from CPYOS.
2. Dynamic index (`lists_url`, `lists_headers`, `lists_items`/`lists_fields`, `{list_id}`,
   bounded by `MAX_BROWSE_LISTS`) — `d5d0bea1`.
3. Merge behaviour (`fixedListCount`, `listIndexLoaded`, fixed lists survive a failed fetch,
   no refetch on Back) — `e93f8d1f`.
4. `requires` filtering — `861683cc`.

Until step 1 exists, 2–4 cannot even be compiled; that is a *sequence* constraint, not a reason to
keep them permanently rejected.

## B. Build / release / CI / docs (review §8)

| sha | what changes | conflict? | necessary/useful? | verdict | route |
|---|---|---|---|---|---|
| `59912ea9` | Link **whatever** PR artifacts exist instead of requiring all five; emit "not available" only when nothing unexpired is found (+ Node fixture test `scripts/tests/test_pr_firmware_links.py`). | **No** — `repos/cpyos-public/.github/workflows/pr-firmware-links.yml` is byte-identical to upstream's parent (`git diff 59912ea9^ -- …` is empty), so the patch applies cleanly. | **Yes — live false negative.** The workflow expects `firmware.bin`/`-sticky`/`-x4pro`/`-x4c`/`-papermono` (`:64-68`) while `ci.yml:135,147-151` builds `default x4pro readpico` and uploads **only** `firmware.bin`. Every PR comment therefore says "Firmware is not available … did not produce …". | **ABSORB_NOW** | Apply the diff. Then align `builds` with CPYOS's real uploads (and consider also uploading the `readpico`/`x4pro` bins `ci.yml:135` already builds but discards). Port the Node fixture test with CPYOS names. |
| `35072257` | README → 2.0.0 (device list, clippings/Bluetooth bullets) + `platformio.ini version = 2.0.0`. | Yes (README rewritten; `0.x-cpyos` version scheme). | No. | REJECT | CPYOS owns README + version; upstream numbering is not a fact CPYOS needs. |
| `e7b47ee5` | `ci.yml` push branch renamed to the default branch (`master` → `develop`). | Partial — the file exists and is already `[main]`, but the rule is stale for CPYOS. | **Yes as a rule**: CPYOS's integration/publish branch is `cpyos` (AGENTS §31.2), and `ci.yml:5` only lists `main`, so direct pushes to `cpyos` run **no build CI**. | ABSORB_PARTIAL | One-line CPYOS edit: `branches: [main, cpyos]` (`concurrency.cancel-in-progress` already dedupes). Not a file merge. |
| `6ec32892` | `.github/release-notes/1.6.5.md`. | Yes (CPYOS uses `docs/releases/<v>.md` + `.ota.json`). | No. | REJECT | CPYOS release notes live in `docs/releases/`, one per tag. |
| `997b4331` | Trim Stable to four approved targets, README/docs target wording, `repair_stable_165` workflow + 192-line script + tests; **states two rules**: build mode ≠ publication eligibility, and a missing channel is `unsupported_channel` (never a silent fallback). | Yes (target table, README(.zh-CN), upstream boards; net-zero with `843c1806`). | File-level no — CPYOS publishes only its own targets and both rules are already enforced: `scripts/verify_release_artifacts.py:128` rejects `-dirty`; `scripts/nightly_targets.py` gives `readpico` `supportedChannels: ['nightly']`. | ABSORB_PARTIAL | Docs-only: two sentences in `docs/engineering/firmware-release.md` (eligibility is separate from the local build mode; unknown channel ⇒ `unsupported_channel`, never fallback). No workflow, no repair script, no README. |
| `414b3797` | Add `~RenderLock(){}` to the OPDS harness stub so the harness models lock lifetime. | Yes (`scripts/tests/test_opds_safety.py` absent). | The file: no. **The idea: yes** — CPYOS has ≥8 harness stubs, most without a destructor (`scripts/tests/test_reading_ui_regressions.py:814`, `test_language_save_feedback.py:39`, `test_airpage_display_retry.py:95`, …), while the real `RenderLock` unlocks in `~RenderLock()` and has a same-task recursion guard (AGENTS §49.5/§49.7). | PLAN_LATER | Add one shared harness prelude (`struct RenderLock { ctor(T&); ~RenderLock(); unlock(); }`) under `scripts/tests/` and reuse it; assert release-on-early-return in reader/OPDS harnesses. |
| `c17b80ab` | Frontlight guide lists Paper Mono. | Yes (`USER_GUIDE.md` is upstream's; CPYOS ships `docs/user-guide/`). | No — Read Pico has no frontlight and CPYOS has no Paper Mono target. | REJECT | Doc belongs to hardware CPYOS does not ship. |
| `a6db8f15` | README: de-link → Silkscreen link. | Yes (CPYOS README). | No. | REJECT | Branding/link in a CPYOS-owned file. |
| `91ed3ef8` | README markdown bolding fixes. | Yes. | No. | REJECT | Same file ownership. |
| `6d2cd3a1` | Split AGENTS.md into a thin map + new `.agents/rules/*.md`; move skills; add review skills. | Yes (CPYOS AGENTS.md = upstream map **plus** CPYOS §52–54; rules already live in `docs/engineering/`). | No — **superseded upstream**: `git ls-tree a6fd569f .agents/` shows `.agents/skills` only, i.e. the rules tree was later removed and the map kept. CPYOS already has that map + `.agents/skills/`. | REJECT | CPYOS decision: single source of record = AGENTS.md map → `docs/engineering/`; on-demand skills in `.agents/skills/`. Do not introduce a second rules tree. |
| `3607ea7b` | Refine those rules; expand the `firmware-handoff` skill to 122 lines. | Yes (same tree). | No — `firmware-handoff` does not exist at `a6fd569f` (dropped upstream); CPYOS never had it. | REJECT | Same decision as above. |
| `d1f5d1ec` | Shrink the same rules/skills (471+/1124−). | Yes. | No — intermediate state; CPYOS's shared skills already match `a6fd569f` within a few lines (control-flow 2, hal 6, heap 15, refactor 52). | REJECT | Optional content review of those small skill deltas when a skill is next edited; never a file merge. |
| `bb855ef9` | SCOPE.md: "as a downstream foundation" — accept narrowly scoped, product-neutral improvements to low-level infrastructure from downstream forks; downstream-specific features stay in the fork. | No (new section; SCOPE.md is CPYOS-owned). | Marginal but cheap, and CPYOS is itself a downstream fork that contributes primitives. | ABSORB_PARTIAL | Author 6–8 lines in `SCOPE.md` where *downstream* = forks of CPYOS. Do **not** merge upstream wording: CPYOS deliberately deleted the old "Working with upstream" block (`git diff a6fd569f -- SCOPE.md`). |

## Summary

| verdict | count | shas |
|---|---:|---|
| ABSORB_NOW | 1 | `59912ea9` |
| ABSORB_PARTIAL | 5 | `d5d0bea1`, `c9f64d0d`, `e7b47ee5`, `997b4331`, `bb855ef9` |
| PLAN_LATER | 4 | `a722472c`, `e93f8d1f`, `861683cc`, `414b3797` |
| REJECT | 10 | `55a77c2e`, `18d611f2`, `35072257`, `6ec32892`, `c17b80ab`, `a6db8f15`, `91ed3ef8`, `6d2cd3a1`, `3607ea7b`, `d1f5d1ec` |

Reject categories that survive the stricter test:

- **Not applicable, no CPYOS counterpart** (2): `55a77c2e`, `18d611f2`.
- **Branding / version / release artifacts owned by CPYOS** (4): `35072257`, `6ec32892`, `a6db8f15`, `91ed3ef8`.
- **Doc for hardware CPYOS does not ship** (1): `c17b80ab`.
- **Superseded upstream, CPYOS already has the end state** (3): `6d2cd3a1`, `3607ea7b`, `d1f5d1ec`.

## Top 5 that should no longer be rejected

1. **`59912ea9`** — the CPYOS file is byte-identical to upstream's parent, and the workflow's five-name
   expectation can never be met (`ci.yml` uploads only `firmware.bin`), so every PR currently gets the
   wrong "Firmware is not available" comment. Apply now.
2. **`c9f64d0d`** — extract `loadPlugins`'s missing `ok`/non-array handling (a 503 JSON silently blanks
   the plugin area) and serve `PluginHost.js` once instead of inlining 8,837 B into 5 pages.
3. **`d5d0bea1`** — the `MAX_BROWSE_LISTS` bound maps onto a real unbounded `reserve()` in
   `PluginCatalogActivity.cpp:200`; absorb the bound now, defer the mapping.
4. **`e7b47ee5`** — the underlying rule is missing in CPYOS: `ci.yml` runs on push to `main` only, while
   integration work is pushed to `cpyos`.
5. **`e93f8d1f`** — worthwhile robustness (fixed lists survive a failed index fetch, no refetch on Back),
   parked only because the list-index base is missing; schedule it in the plugin stack.

Runners-up: `861683cc` (needs the capability vocabulary), `414b3797` (stub-lifetime idea applies to
8 CPYOS harnesses), `bb855ef9` (SCOPE clause), `a722472c` (one-line rule when a detail page lands).

## Correction to the earlier audit slices

`05-network-plugins.md` / `06-build-docs.md` justified two `D_SKIP` verdicts with
"CPYOS does not have `pr-firmware-links.yml` / has only `cpyos-release.yml` + `pages.yml`".
That is **false for both trees**: `repos/cpyos-public/.github/workflows/` and the dev tree both contain
`pr-firmware-links.yml` (132 lines, the pre-fix version). Same for `ci.yml` (`e7b47ee5`), which exists
and only triggers on `main`. Those two rows were rejected on a false premise and are now
ABSORB_NOW / ABSORB_PARTIAL.

## Verification commands used

```bash
git -C <worktree> log -1 --format='%ad %s' --date=short <sha>
git -C <worktree> show --stat <sha>            # diff for a722472c, 55a77c2e, e93f8d1f, d5d0bea1, 861683cc, 59912ea9, 414b3797, 18d611f2, bb855ef9, 997b4331
git -C <worktree> diff 59912ea9^ -- .github/workflows/pr-firmware-links.yml   # empty == applies cleanly
git -C <worktree> ls-tree a6fd569f .agents/ .agents/skills/                    # no .agents/rules upstream
grep -n "reserve(browse\[\"lists\"\]" src/activities/plugins/PluginCatalogActivity.cpp   # :200
grep -rn "PLUGIN_CAPABILITIES|FirmwareIdentity" src                                         # none
grep -n "appearance" src/network/html/en/FilesPage.html                                     # sliders only
grep -n "branches:" -A1 .github/workflows/ci.yml; grep -n "name: firmware.bin" -A3 .github/workflows/ci.yml
grep -n "PluginHost.js.inc" src/network/html/*/SettingsPage.html src/network/html/*/FilesPage.html src/network/html/RunnerPage.html
```


---

# 剩余 4 项（封面中间态/陀螺仪/BLE）

# Remainder of the 70 KEEP_REJECTED items — stricter re-read

Scope: the four commits no other review re-examined under the strict rule
**reject only when a change is BOTH conflicting AND unnecessary; otherwise absorb-now / absorb-partial / plan-later**.
Read-only. HEAD `b39f3706`, upstream `a6fd569f`, SDK submodule at `d9a1304c` (patched).

Prior art corrected here: `bucket3-ui-a.md:40` says `Imu::hasGyroscope()` "does not exist at pin `96de1be`" — true at
that pin, **false at the current submodule** (`d9a1304c`), so that nugget moves from "future SDK bump" to "absorb now".
`bucket3-renderer-storage.md:30-31` is right that the *final state* is in tree; it does not state that the *residual need*
(suppressing repeat conversion attempts) is still unimplemented, which §1 below shows.

## Verdicts

| sha | what changes | conflict? | necessary/useful? | verdict | route |
|---|---|---|---|---|---|
| `186d9407` | Failed cover conversion → writes a **0-byte file as terminal marker**; forces `emptyIsTerminal=true` in all six `ensure*()` container branches; sizes caches by `sizeof(BmpHeader)` | **yes** — behavioural (conflates "no cover" with "transient failure"; bypasses `hasCoverOverride()` invalidation; CPYOS's Home probe passes `emptyIsTerminal=false` for full covers, so the marker it writes gets deleted and retried anyway) + textual (`Epub.cpp`, 7 call sites, 6 test stubs) | **no as written** — upstream reverted it 7 h later and CPYOS already sits on the reverted end state; the residual need (stop re-attempting a *failing* conversion) is real but narrow | **Reject as written** → residual routed to **plan-later** (RAM failure memo, no file sentinel) | §1 |
| `12e441f7` | Pure revert of `186d9407`, plus `sizeof(BmpHeader)` payload check | none — it *is* CPYOS's current code | **no** — its one lasting line was superseded by `3db7a96b` (`bfOffBits`), which CPYOS merged as `7352b33f` | **Reject / superseded, no action** | §2 |
| `8fc2b661` | Gate tilt on `hasGyroscope()`, `_available=false` when absent; reorders `sleep()` before the check; settings comment | **yes as written** (would delete Read Pico accel tilt + shake) | **partially** — the *capability-query pattern* is portable **now**, not after an SDK bump | **Absorb-partial (absorb now)** — take `!hasGyroscope()` detection, keep accel tilt, keep `_available = begin()` | §3 |
| `fecee610` | 6.3 k-line parallel BLE owner (`BlePageTurner` runtime/policy/port), own settings screen + JSON, SDK gitlink bump, `platformio.ini`, `MappedInputManager` remote injection | **yes, structural** — second owner on the same radio, second key mapper, second settings surface | **no as a module**; **three mechanisms are genuinely missing** in CPYOS (idle radio off, bounded start retries, radio down before the next screen allocates) | **Absorb-partial** (mechanisms only), reject module | §4 |

## 1. `186d9407` — no terminal sentinel; a RAM failure memo instead (plan-later)

Today's CPYOS path, read from the tree:

- `src/util/BookCoverLoader.cpp:19-45` `checkCachedCover()` returns `Terminal` only for `(a)` an unreadable file and
  `(b)` `fileSize()==0 && emptyIsTerminal`. Payload validation is `bfOffBits + rowBytes*height <= fileSize`
  (upstream `3db7a96b`) — correct for the `_g8` Gray8 thumbs (1,078-byte palette prefix).
- `src/util/BookCoverLoader.cpp:51-66` `ensureCachedCover()` **never writes** a marker: a failed `generate()` just
  returns `""`. `lib/Epub/Epub.cpp:1088-1096` does write the 0-byte "no cover" marker, but only for
  `coverImageHref.empty()` — i.e. the *legitimate* "this book has no cover" case, guarded by
  `emptyIsTerminal = !epub.hasCoverOverride()` at `BookCoverLoader.cpp:198`.
- `lib/Epub/Epub.cpp:45-62` `convertCoverFile()` deletes the output on failure, so a corrupt/oversized/undecodable
  cover leaves nothing behind → the state is `Missing` on the next probe.

So: **a failed conversion is re-attempted once per screen entry, not per render.**
`HomeActivity.cpp:169-193` re-probes and re-generates inside `loadRecentBooks()` on every Home entry, under a
`status_card::showWait` progress card; the per-attempt cost is already logged
(`ensureCachedCover`, `"thumbnail %s in %ums"`, 0.3–3 s on a slow card). `LibraryListActivity.cpp:2101/2111`
latches `GridCover::Unavailable` per epoch, `InxRecentActivity.cpp:268/406` latches `CoverCacheState::Unavailable`
per session, so those two are already bounded.

Why not the sentinel: a 0-byte file cannot distinguish "no cover" from "this decode failed once"; the recovery path today
is "delete the marker", which the user cannot discover, and `hasCoverOverride()` is the only invalidation. Upstream
reached the same conclusion (`12e441f7`, five hours later) and never restored it.

**Route (plan-later, ~40 lines, no file-format change):**
add a negative memo in `BookCoverLoader`'s anonymous namespace next to `g_thumbPathSlots`
(`BookCoverLoader.cpp:100-140` is the template: static fixed array + LRU stamp, no heap):

```cpp
struct FailureSlot { std::string coverPath; std::string bookPath; uint64_t bookBytes; bool overridePresent; uint32_t stamp; };
```

- Record on `generate()==false` **or** on a post-generate state that is still not `Ready` (`ensureCachedCover:62`),
  keyed by cover path + container `fileSize()` (there is **no mtime API** — `lib/hal/HalStorage.h:114-132` exposes
  `fileSize`/`fileSize64` only) + `hasCoverOverride()` at the time of failure.
- Short-circuit at the top of `ensureCachedCover()` when the key matches: return `""`, one `LOG_DBG`, **no** wait card
  and no SD reads.
- Invalidate on: `clearThumbFileCache()` (`BookCoverLoader.h`), library index rebuild / epoch bump
  (`LibraryListActivity.cpp:898`), any `cover.override` appearance (`hasCoverOverride()` flip → different key),
  and implicitly on every boot.
- Honest limit: a replaced book of identical byte size stays memoised until reboot. That is the same durability class as
  the existing session caches, and it removes only *repeated* work, so a stale entry is a missing thumbnail, never a
  wrong one.

Do **not** key on "0-byte file exists": that is the sentinel by another name.

## 2. `12e441f7` — nothing to take

It reverts §1 and keeps `fileSize >= sizeof(BmpHeader) + pixelBytes`. That formula is wrong for any palette larger
than the 1bpp one (`sizeof(BmpHeader)` is 62 by construction here), which is exactly what `3db7a96b` fixed with
`Bitmap::getPixelDataOffset()`; CPYOS has the fixed form in `checkCachedCover` and in `Bitmap.h`.
Independent value: **none**. Do not port its test/stub deletions either — CPYOS's
`test/book_cover_loader/BookCoverLoaderTest.cpp:318-375` already pins the four Home-fast-path cases
(header-only, truncated, complete, empty-marker).

## 3. `8fc2b661` — take the capability query, keep the feature

Current CPYOS detection (`lib/hal/HalTiltSensor.cpp:66-88`):

```cpp
#if FREEINK_DEVICE_READPICO
  _accelOnly = BoardConfig::ACTIVE.sensors.imuType == BoardConfig::ImuType::Sc7a20h;
#else
  _accelOnly = false;
#endif
```

That is a board/part-table check behind a board macro. Two concrete weaknesses: (a) any non-ReadPico target with an
accelerometer-only part gets `_accelOnly=false`, so `isAvailable()` stays true, the Tilt/Shake row stays in
`src/SettingsList.h:527`, and the gesture can never fire (gyro reads are zero); (b) the `#if` pins a shared HAL file to
one board.

The SDK now answers the capability question directly: `freeink::Imu::hasGyroscope()`
(`freeink-sdk/libs/hardware/Imu/include/Imu.h:32`, impl `.../src/Imu.cpp:438-442`:
`begun_ && imuType ∈ {Lsm6ds3, Qmi8658, Bmi270}`).

**Change (absorb now, ~6 lines):**

1. `HalTiltSensor.cpp:66-88`: replace the block with `_accelOnly = !_sdkImu.hasGyroscope();`, keeping
   `_available = _sdkImu.begin()` — i.e. **do not** take upstream's `_available = hasGyroscope()`.
2. Take upstream's ordering nit: `sleep()` before the capability decision, so an accel-only part still goes to standby
   when the gesture is off (today it already does, but the order makes it unconditional).
3. Keep the log self-proof (`"accelerometer only, gravity rate"` vs `"gyroscope rate"`) — per project discipline that
   string is how the compiled branch is verified on device (`GYR` line in `device_log/live.log`).
4. No `SettingsList.h` change: `isAvailable()` semantics stay "there is a usable motion source".

Optional hardening, separate decision: if `hasGyroscope()` is true but `read()` yields `gx=gy=gz=0` for a whole
session, fall back to the gravity path instead of retrying zero-rate samples. Not required to close this item.

**SDK dependency:** none — `hasGyroscope()` is in the pinned submodule. Note it is still table-derived, so a wrong board
table misreports exactly as today; the win is that the app no longer hardcodes a board macro, and the accel-only case
now works on any target.

## 4. `fecee610` — take three mechanisms, not the module

CPYOS already owns the radio: `src/BleInput.{h,cpp}` (start contexts, per-board gates incl. the NimBLE PSRAM probe,
`encodeKey`/`describeKey`), `src/BleRemotePolicy.h` + `test/ble_remote/ble_remote_policy_test.cpp`, `bleKeyMap` +
`BleButtonMapActivity` + `BluetoothSettingsActivity`, and the "no bonded remote → reader keeps the radio off" rule at
`BleInput.cpp:247-257` / `main.cpp:314-317`. It also already has the equivalent of upstream's remote-held-time guard
(`MappedInputManager.cpp:634` `if (bleActivityThisFrame) return 0;`). A second module would mean two owners; the module
stays rejected.

Three gaps that upstream solves and CPYOS does not:

**(a) Idle radio off — missing entirely.** `updateBluetoothLifecycle()` (`src/main.cpp:298-335`) stops only when the
*screen* stops wanting BLE or when **no remote is bonded**. A bonded remote that is switched off / left at home keeps
the radio up for the whole reading session at full CPU. Upstream: `kIdleOffMs = 30 s`
(`BlePageTurner/src/RadioPolicy.h:27`, verdict at `:135`; decision block `Runtime.cpp` "Nothing connected for long"), re-armed on reader
entry or an explicit connect, and the clock is reset by link state *and* by advertisement activity so a peer that is
being discovered right now is not killed at the boundary.
Route: add `constexpr bool radioIdleExpired(bool linked, bool busy, uint32_t idleMs, uint32_t limitMs = 30000)` to
`src/BleRemotePolicy.h` (pure, host-testable — extend `test/ble_remote/ble_remote_policy_test.cpp` with the 29 999 /
30 000 ms boundary); in `BleInput.cpp` track `BleHid.isConnected() || isConnecting() || isScanning()` and expose
`bleinput::tickIdle()` that calls `stop()` and sets a per-reader-visit `idleStopped` flag; call it from
`updateBluetoothLifecycle()`; re-arm on reader entry (`BluetoothSettingsActivity` is an explicit owner and stays out of
the policy).
**SDK dependency:** the coarse rule needs none. Upstream's exact rule resets on `port::connectionActivity()`
(`advertisements`), which has no equivalent at `d9a1304c` (`freeink-sdk/.../BleKeyboardHost.h:134-215` exposes
`isConnected`/`isConnecting`/`isScanning` only) — record that if the finer rule is ever wanted.

**(b) Unbounded start retry with a side effect.** `main.cpp:332` retries every 2 s forever; each refusal in Reader
context first runs `cache->releaseSdFontCaches()` (`BleInput.cpp:184-200`), so a starved reader drops its SD font
caches every 2 s. Upstream bounds it: `kRetryMs=5000`, `kRetryLimit=6`, counter reset per book visit and on success
(`Runtime.cpp:29-34`, retry block in the `Phase::Running` handler).
Route: per-reader-visit `retries` counter in `updateBluetoothLifecycle()`, released only on the first attempt(s) of that
visit, and a `kRetryLimit` after which the reader stops asking until the next visit.

**(c) Radio still up while the next screen allocates.** `updateBluetoothLifecycle()` runs at `main.cpp:1623`
(pre-activity) and `:2683` (post-`activityManager.loop()`), so a `Push`/`Replace` handled in that same iteration has
already run `onEnter()` (Home cover scan, library index) with the NimBLE heap still resident. Upstream calls
`bleturner::beforeScreenChange()` inside `ActivityManager`'s pending-action loop and `beforeSleep()` on the sleep path
(`goToSleep` → `ReplaceForSleep`).
Route: in `src/activities/ActivityManager.cpp:269` (`while (pendingAction.load() != PendingAction::None || deferredPush)`)
call `bleinput::stop()` when `pendingActivity && !pendingActivity->keepsBluetoothAlive()`
(`Activity.h:59-60` already defines the predicate), and call it from `goToSleep()`/`enterDeepSleep` before the sleep
screen allocates. `BleHid.end()` is already bounded and idempotent (`BleKeyboardHost.h:127-141`), but note
`ensureStarted()` (`BleInput.cpp:167-172`) does **not** consult `BleHid.isStopping()`, so a `begin()` refused while a
teardown is in flight is reported as `Failed`; add that guard in the same change.

**Not taken:** `BlePageTurner` runtime/`RadioPort`/`RadioPolicy` verdict machine, its JSON settings block, its
settings/pairing screens, per-remote learned button tables (`RawButtonEvent`/`popRawButton` already exist in the SDK,
and `bleKeyMap` is CPYOS's shipped mapping — migrating would be a product change), link notes, and the Home-start
profile (CPYOS starts only inside the reader; the X3 49 KB budget and the S3 "start at Home" numbers do not apply to
Read Pico's PSRAM-hosted stack).

## Coverage

`2026-11-08-upstream-kept-rejected-review.md` lists **70** KEEP_REJECTED items in eight themes.
Themes 1, 2, 4, 6, 7, 8 (66 items) were re-examined by the six area reviews; theme 3 (2 items: `186d9407`,
`12e441f7`) and the two "would delete an existing capability" traps in theme 5 (`8fc2b661`, `fecee610`) are the four
below.

**70/70 covered.** Outcome of this remainder: 0 absorb-now-as-written, 2 absorb-partial (`8fc2b661` capability query,
now unblocked; `fecee610` idle-off + bounded retry + stop-before-screen-change), 1 plan-later (RAM failure memo from
`186d9407`), 1 no-action (`12e441f7`), i.e. still 0 commits taken verbatim.

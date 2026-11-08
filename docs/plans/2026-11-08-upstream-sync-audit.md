# Upstream 285-commit audit (0x1abin/main → cpyos)

Fork point `83b0f81` (2026-10-12) → live `upstream/main` `a6fd569f` (2026-11-07).

Range: **285 commits** = 254 non-merge + 31 merges. All 254 non-merge commits were individually audited (8 area slices + 1 SDK deep-dive, each in its own detached worktree at `834e6207`).


## Snapshot

- Dry-run `git merge upstream/main`: **150 conflicted files** (132 UU, 7 AA, 3 UD, 8 DU), 254 new files added cleanly, 412 modified files merged cleanly, `freeink-sdk` gitlink auto-resolved to upstream `d9a1304c`.
- Patch-id equivalence (`git cherry cpyos upstream/main`): **0** — none of the upstream commits exist byte-identically; all earlier absorption was re-implementation.
- Classification: **A_ALREADY 45 · B_CLEAN 36 · C_CONFLICT_WORTH 77 · D_SKIP 88 · SDK_DEFER 8**.
- SDK gap: `96de1be → d9a1304c` = 202 commits, 337 files, +53.6k/−4.8k. Our patch set = 31 files/114 hunks (+2.1k/−0.3k); 27/31 files still apply clean, only 12/114 hunks fail. Upstream now has a better CFF stack (cff+psaux+pshinter+psnames; `psintrp.c` heap-allocates `glyphPath`).


## Bucket 1 — directly mergeable (B_CLEAN, 36)

By area: renderer-epub=3, ui-a=2, ui-b=3, fonts-i18n=3, network=5, build-docs=5, storage-misc-a=11, misc-b-sdk=4

### renderer-epub (3)

- `149dd532` fix: stop books with thousands of chapters aborting the index build
  - CLEAN | — | **B_CLEAN** | CPYOS `BookMetadataCache.cpp:325-346` still does `targets.resize(spineCount)` (one 16 B/entry deque for the whole spine). Upstream chunks 2,048 items at a time. Take: bounds the batch-buffer RAM on C3 for 2,000+ chapter books. No behaviour change; verify `fillUncompressedSizes` return still accumulates.
- `daa004da` fix(txt): resolve first-page chapter navigation
  - CLEAN | — | **B_CLEAN** | CPYOS `lib/Txt/Txt.cpp:430` still passes `visibleOffset` unchanged; page 0 starts at the generated body newline so `sourceForVisible(0)` maps to the wrong chapter. Take as-is (one-line + test).
- `fc4b740a` fix: copy a TXT cover with a checked read
  - CLEAN | — | **B_CLEAN** | CPYOS `lib/Txt/Txt.cpp:177-181` still loops `while (src.available()) { dst.write(buffer, src.read(...)) }` — a `-1` read becomes `SIZE_MAX` write, and a failing write leaves a truncated cover that `exists()` accepts forever. Take as-is.

### ui-a (2)

- `b90389ea` feat: hide dictionary entry when none configured
  - CLEAN | — | B_CLEAN | CPYOS `EpubReaderMenuActivity::buildMenuItems` pushes `DICTIONARY` unconditionally and `SETTINGS.dictionaryName[32]` exists. Wrap in `if (SETTINGS.dictionaryName[0] != '\0')`. Also mirror in the toolbar `buildMoreActions()` if that path adds a lookup row.
- `eced576b` fix: add plugin-manager to return-selection mapping
  - CLEAN | — | B_CLEAN | `cpyos` `ActivityManager::goHome` maps only `OpdsBookBrowser`; add `\ | \ | activityName == "PluginCatalog"`. `PluginCatalogActivity` exists in CPYOS. One-line, zero risk.

### ui-b (3)

- `0f4f82ec` feat(plugins): Make session recording more robust (#3869)
  - CLEAN | — | B_CLEAN | CPYOS already has `flushReaderSession()`, `completeAlreadySynced()`, `pluginevents` and the same ctor shape, so KOReader's WiFi flow can be reused with `pluginEventsOnly`. Compile check needed (`STR_SYNC_PROGRESS`, `PluginHttp.h` include set).
- `6251054b` fix: merge sleep image directory pools (#4025)
  - CLEAN | — | B_CLEAN | CPYOS still does `selectRandomSleepFile("/.sleep")` then `/sleep` (SleepActivity.cpp:657-658) and the same for the overlay pools (:896-897): files in the second dir are invisible whenever the first has one. Straight port; call it twice for the overlay pair.
- `d9764e2b` feat: add end_percent to plugin reading events (#4201)
  - CLEAN | — | B_CLEAN | CPYOS has `ChapterPosition`, `bookPercentFor()`, `isAtEndOfBook()`, `ScreenshotInfo` and the same emit sites (ReaderActivity.cpp:154, main.cpp:699), so the patch lands as-is. Compile check (`position.hasTotal`, `section->isPartial`).

### fonts-i18n (3)

- `0080d8fe` fix: highlight current selection in font downloader (#4101)
  - clean | — | **B_CLEAN** | Deletes the `item.state = fui::StateDisabled` on installed families (the row stayed tappable but looked unselectable). CPYOS has the identical 3 lines and `FontDownloadActivity` is still reachable from Settings (`SettingsActivity.cpp:1060`). Trivial, zero risk.
- `551d48e7` fix: Correcting capitalization in English settings to make it uniform (#4224)
  - clean | — | **B_CLEAN** | CPYOS still has `STR_WORD_SPACING: "Word spacing"` / `STR_CHARACTER_SPACING: "Character spacing"` (english.yaml:799-800) while neighbours are title case. Two strings, 30-second port.
- `99c2f09d` feat: Add vector font (.ttf/.otf) upload support in webserver (#3717)
  - clean | — | **B_CLEAN** | Adds `FontInstaller::isValidVectorFontFilename` (same traversal rules), sfnt magic check, PSRAM-gated `vectorFonts` flag in `/api/fonts`, and `FontsPage.html`. CPYOS still only accepts `.cpfont` on `/api/fonts/upload` (AGENTS §18) although the runtime already renders TTF/OTF — take it. `FontsPage.html` is flash-resident in CPYOS (§25), so the HTML hunk applies as-is.

### network (5)

- `5bd05983` fix: normalize web upload filenames to NFC
  - clean | — | **B_CLEAN** | CPYOS `reserveAvailableUploadFilename` (en/FilesPage.html:4098) and `fetchExistingUploadNames` (:4127) still compare `toLowerCase()` only, so NFD names from macOS are invisible on FAT. Not a security fix; portability bug for CJK/accented names. Mirror into zh-CN, repack.
- `c1a62f66` fix: prevent FilesPage modal from closing when selecting text
  - clean | — | **B_CLEAN** | 8 lines in en/FilesPage.html (`mousedown` records `clickTarget`, close only if `clickTarget === overlay`). CPYOS has the identical overlay loop at en/FilesPage.html:2077. **Mirror into zh-CN/FilesPage.html**, then regenerate web pack + `WebPackSchema.generated.h`.
- `cdac66ff` fix(webdav): GET sends one byte per file
  - clean | — | **B_CLEAN (critical)** | **This bug is live in CPYOS.** `WebDAVHandler.cpp:331 client.write(file)` and `HalFile : public Print` (HalStorage.h:114) ⇒ overload resolution picks `Print::write(uint8_t)`, sending `0x01` with the real `Content-Length` (client sees a truncated transfer, looks like flaky Wi-Fi). Fix = `Storage.readFileToStream(path, client)` (declared HalStorage.h:55) with the size captured before close. No CPYOS-specific adaptation needed.
- `daf0c177` feat: add overwrite flag to /upload
  - clean | — | **B_CLEAN** | CPYOS still refuses existing names (CrossPointWebServer.cpp:1092) and **ignores a failed final `close()`** (:1163, :1189) — a file whose size was never written can report success. Patch adds `?overwrite=1` via `<dest>.part` staging + `Storage.replaceFile` (CPYOS has it), refuses folder collisions, and turns a failed close into a write error. Safe by design (old file survives a failed upload).
- `f331030f` Allow plugins to override download directory via config
  - clean | — | **B_CLEAN** | Adds `downloadDir()`: `dest_dir` from the plugin **config** (not manifest) validated by `protectedpaths::isPluginPath`, used by `downloadBundle()`/`downloadBook()`. CPYOS has every prerequisite (`pluginhttp::Headers config` PluginCatalogActivity.h:125, `manifest.destDir` :219, `isPluginPath` ProtectedPaths.h:69). Port as-is (3 files).

### build-docs (5)

- `0fcdb3ec` fix: allow deleting hidden files in the web UI (#4031)
  - clean | — | **B_CLEAN** (guard first) | CPYOS `src/network/CrossPointWebServer.cpp:1500` still refuses to delete any name starting with `.` (`" (hidden/system file)"`), so the File Transfer page cannot delete `.wifi.json`-style leftovers or stray `/.fonts` entries. Upstream keeps `protectedpaths::isSensitivePath()` (wifi/opds/koreader json) as the only guard. **Caution:** CPYOS keeps settings, caches, jjwxc cookies/keys under `/.crosspoint/`; today those are already deletable as *contents* (only the last path segment is checked), but allowing dot-names makes deleting the directory itself possible. Recommend applying *and* extending `protectedpaths::isSensitivePath` to `/.crosspoint` (and `/.crosspoint/jjwxc`), plus the docs hunks.
- `6def3259` fix(weread): preserve navigation after single-page shelf holds
  - clean | — | **B_CLEAN** | Real 1-line behaviour bug still present in CPYOS: `WeReadActivity.cpp:1735` sets `shelfNavigationGesture_`/`shelfLastPageTurnAt_` *before* `if (count > itemsPerPage)`, so on a single-page shelf a hold-arrow marks the press "handled" and swallows the next one. Move both assignments inside the `count > itemsPerPage` block. Adds `scripts/tests/test_weread_shelf_input.py` (new file, compiles public header text — verify it still matches CPYOS's slightly older `WeReadActivity`).
- `a5340ea6` chore: compile out the wolfSSL debug trace, save 39Kb (#4213)
  - clean (7 lines, `scripts/patch_wolfssl.py`) | — | **B_CLEAN — HIGH VALUE** | **Top flash win in this slice.** CPYOS `scripts/patch_wolfssl.py` (used by `[base]` ⇒ every env) lacks the `#ifndef FREEINK_WOLFSSL_DEBUG / #undef DEBUG_WOLFSSL` block; Arduino-wolfSSL's `user_settings.h` always defines `DEBUG_WOLFSSL`, so all `WOLFSSL_MSG/ENTER/LEAVE` strings are linked. Safe here: no env defines `FREEINK_WOLFSSL_DEBUG` (grep: only `freeink-sdk/.../SecureClient.cpp:59` tests it) and CPYOS/release code uses `wolfSSL_get_error()` codes only. Upstream measured −38,896 B on gh_release; CPYOS app0 currently has ~121 KB headroom, so this buys ~1/3 of it back. Build-verify one env, then re-measure.
- `bbb45c39` fix: scope Git version to sources that use it (#3860)
  - conflict on doc only; `scripts/git_branch.py` applies clean | docs/contributing/getting-started.md | **B_CLEAN** | SCons `AddBuildMiddleware` re-applies `CROSSPOINT_VERSION` only to TUs containing the token. Public `scripts/git_branch.py:95` still does a global `env.Append(CPPDEFINES=...)`; CPYOS gates injection to dev envs (`default`, `sticky`, …), release envs pass `-DCROSSPOINT_VERSION` via build_flags, so no duplicate-define risk. Benefit is incremental cache reuse (CPYOS incremental ≈138 s); low priority, zero risk. Drop the doc hunk (upstream contributing doc path differs).
- `e142df46` fix(settings): explain language selection save failures
  - clean | — | **B_CLEAN** (with i18n cost) | Applies coherently over CPYOS's `LanguageSelectActivity` (pack-availability + `bootlang::remember` kept). Fixes a real UX bug: CPYOS's save-failure branch still marks the **failed** language as `Selected` (`rowItems[i].value = ... == langIndex`) and gives no explanation. **Cost:** adds `STR_LANGUAGE_SAVE_FAILED` (en+zh) ⇒ i18n key count changes ⇒ all 33 packs + data partition must be reflashed and old packs are rejected (§29.1 trap). Batch it with other i18n-key commits.

### storage-misc-a (11)

- `164f0995` fix: keep heap free while the plugin relay buffers a response
  - CLEAN | — | **B_CLEAN** | Clean apply. CPYOS already has `HalMemory::getDefaultHeap()` and the submodule has `SecureHttpClient::hasContentLength()/getContentLength()`, so the new `heapReserve` path works. Worth taking (web-server stall risk).
- `1f77b83d` fix: stop dropping log lines at the low-power clock
  - CLEAN | — | **B_CLEAN** | CPYOS `Logging.cpp` still has `if (logSerial) { logSerial.print(buf); }` — the exact line this fixes. High diagnostic value on C3/X4 at 10 MHz.
- `3a03be89` fix(KOSync): resolve upload XPaths for text outside `<p>`/`<li>`
  - CLEAN | — | **B_CLEAN** | Clean; CPYOS ships `ChapterXPathResolver` unchanged. Fixes silent "always first page" progress pushes for Calibre/MOBI-derived EPUBs.
- `79915f07` fix: wrap plugin event disclosures to fit screen
  - CLEAN | — | **B_CLEAN** | Adds `src/util/PluginPickerLayout.h` + host test (note: a 71 KB PNG fixture). Also the include that `00897520` expects.
- `813aaa12` fix(bidiutils): canonicalize reversed tuple in emission path
  - CLEAN | — | **B_CLEAN** (low) | Clean. Hebrew-only (theonym) heuristic; irrelevant to CPYOS's CJK focus but harmless.
- `9561a741` fix(sync): enforce C3 memory regression checks
  - CLEAN | — | **B_CLEAN** (tooling) | Only `.agents/skills/sync-upstream/**`; CPYOS has that skill. No firmware impact.
- `a6fd569f` fix(waveshare-epaper-397): always deep sleep, wake on any rotary key
  - CLEAN | — | **B_CLEAN** (hw) | Clean apply. CPYOS has `env:waveshare_epaper_397`, so it compiles; no CPYOS shipping target uses it. Zero risk, take verbatim.
- `aa76a31b` fix(kosync): advertise percentage progress support
  - CLEAN | — | **B_CLEAN** | One-liner: `?position_kinds=locator,percentage` on the progress GET. CPYOS lacks it.
- `bb4f8785` fix: map JPEG scale options to pixel divisors
  - CLEAN | — | **B_CLEAN** (defensive) | Today `JPEG_SCALE_HALF=2 / QUARTER=4 / EIGHTH=8` (JPEGDEC.h) so CPYOS's `scaleOption == 0 ? 1 : scaleOption` happens to be right, but a flag-value change silently yields header-only BMPs. Cheap robustness; take it.
- `c71c1562` test: run standalone UI regressions with CTest
  - CONFLICT | test/CMakeLists.txt | **B_CLEAN** (test infra) | 5 lines registering the python UI regressions; CPYOS's `test/CMakeLists.txt` diverged but the hunk is additive. Low priority (CPYOS has no cmake locally).
- `e0bcd26b` feat: filter plugin events by book metadata
  - CLEAN | — | **B_CLEAN** | CPYOS has no `require_meta` (grep 0 hits); `PluginEvents.cpp` is otherwise unmodified from base, so it applies verbatim. Docs-only extra.

### misc-b-sdk (4)

- `280895ff` fix: split dictzip inflate window into 8KB segments (#3826)
  - conf: .github/workflows/ci.yml, test/CMakeLists.txt | CI + test CMake only | B_CLEAN | **Highest-value clean port in this slice.** All `lib/InflateReader/*`, `lib/uzlib/src/*`, `src/util/DictZip.*` hunks applied with no conflict. CPYOS uses exactly this path (`src/util/DictZip.cpp`, `Dictionary.cpp`) and still needs one contiguous 32 KB ring — its own header comment already admits "measured on device at 32756 bytes against a 32768 requirement". Segmented ring removes that cliff. Take the code; drop the CI/CMake hunks and add the new host test to CPYOS's own test wiring if wanted. Must bump nothing else (uzlib ABI internal).
- `5d18bee7` perf: allocate BiDi workspace on first use (#4116)
  - clean | — | B_CLEAN | Applies cleanly; CPYOS's `lib/MiniBidi/BidiUtils.cpp` still has the two static `bidi_char` buffers. Take only if internal-RAM static savings matter: it converts ~2×`BIDI_MAX_LINE` static → heap on first RTL paragraph (so it *moves* cost to the heap, ~1-4 KB, on first Arabic/Hebrew page). Given CPYOS's PSRAM budget this is low value and slightly higher fragmentation risk; safe either way.
- `92216b41` refactor: remove unused zlib header helper (#4057)
  - clean | — | B_CLEAN | CPYOS declares/defines `InflateReader::skipZlibHeader()` (`lib/InflateReader/InflateReader.{h,cpp}`) with **zero callers** (`git grep` in cpyos). Pure dead-code delete; take as-is.
- `b05cb3e8` docs: fix KOReader document ID offset comment (#4191)
  - clean | — | B_CLEAN | Comment-only change in `lib/KOReaderSync/KOReaderDocumentId.h`; applies cleanly. Zero risk, take with any batch.


## Bucket 2 — conflicts but worth resolving (C_CONFLICT_WORTH, 77)

By area: renderer-epub=7, ui-a=7, ui-b=11, fonts-i18n=11, network=13, build-docs=5, storage-misc-a=12, misc-b-sdk=11

### renderer-epub (7)

- `1a5c8cd1` perf: Batch page-position reads
  - CONFLICT(1) | Section.cpp | **C_CONFLICT_WORTH** | CPYOS `Section::getPageForVisibleTextOffset` (`Section.cpp:1151`) still reads one `uint32_t` per page in a loop (up to `count` SD reads) after the in-RAM `build_->lut` fast path. Resolution: keep-ours for the LUT branch; replace the disk loop with the 32-entry batch (also picks up the `f.seek` failure check).
- `31dc683f` fix: broken focus reading with RTL text
  - CONFLICT(7) | ParsedText.cpp, TextBlock.{cpp,h}, Section.cpp, docs, stubs, CMakeLists | **C_CONFLICT_WORTH** | CPYOS still stores `focusSuffixXArr` (`TextBlock.cpp:30,295`, `TextBlock.h:78`) and has no `measureFocusRtlSplitAdvance`; RTL words in bionic/focus reading therefore resume the regular run at the wrong x. Self-contained (no dependency on 0305aaa6): rename suffixX→splitX + add the RTL branch in `extractLine`. Also bumps Section version in CPYOS terms.
- `56c677f6` fix: lend the framebuffer to the chapter build's image dimension probe
  - CONFLICT(3) | ChapterHtmlSlimParser.cpp, GfxRenderer.h, EpubReaderActivity.cpp | **C_CONFLICT_WORTH** | CPYOS has `GfxRenderer::FrameBufferLoan` (`GfxRenderer.h:828`) and `releaseFrameBufferForBuild()`, but the parser's probe (`ChapterHtmlSlimParser.cpp:1070-1073`) has **no loan retry**, and `frameBufferLoans`/`frameBufferLoanCount()` do not exist. Resolution: keep-ours; add (a) `frameBufferLoans` counter to GfxRenderer, (b) retry probe + extraction wrapped in `FrameBufferLoan` in CPYOS's image branch, (c) `pageBufferStale` gate in CPYOS `openOverlay()`/background-build tick.
- `72ca92fe` fix(dict): strip CJK blanket-like punctuations
  - CONFLICT(3) | CjkBreaks.{cpp,h}, test | **C_CONFLICT_WORTH** | CPYOS `Dictionary::cleanWord` (`src/util/Dictionary.cpp`) only strips ASCII + U+2000–206F curly quotes; `「词语」`/`（词语）` never match a headword. Depends on exporting `isNoBreakBefore/AfterCjkPunctuation` (currently file-static in `ParsedText.cpp:74,114`) — either land 9c3fbdd1 first or lift the two predicates into a shared header. Small, user-visible for CN/JP books.
- `9c3fbdd1` fix(ruby): handle ruby text narrower than its base text
  - CONFLICT(4) | ParsedText.cpp, TextBlock.cpp, 2× CMakeLists | **C_CONFLICT_WORTH** | CPYOS ruby is the pre-2.0.0 shape: `TextBlock.cpp:199-204` `RubyDrawInfo{int x; std::string text; baseDir;}` with centred-only draw (`:221,305`) — no `space-around` spread. Commit also extracts `CjkBreaks.{h,cpp}` (CPYOS keeps `cjkCharacterBreakByteOffsets` + `isNoBreak*CjkPunctuation` in `ParsedText.cpp` anonymous namespace). Recommendation: take the ruby-align part and the module extraction (it unblocks 72ca92fe); keep CPYOS's `cjkCharacterBreakByteOffsets` body (already 3e2af487's two-pass form) as the `CjkBreaks` implementation.
- `ad53eb64` fix(ruby): make ruby renderings closer to web browsers
  - CONFLICT(4) | ParsedText.cpp, TextBlock.cpp, Section.cpp, test | **C_CONFLICT_WORTH** | Newest ruby overhang model (left/right "stick-out" units, adjacency handling, `1/(2M)` reservation). CPYOS `ParsedText.cpp:902-1040` still has `isCjkIdeograph` + `leftOverlap/rightOverlap = delta/2` and reserves the **full** half-overhang at line start. Take together with 9c3fbdd1 (ad53eb64 assumes 9c3fbdd1's `CjkBreaks`/`spread` structures); without it furigana near line edges overflows the margin.
- `f8936ba6` perf: finish chapter pagination in idle time
  - CONFLICT(5) | Section.{cpp,h}, ChapterHtmlSlimParser.h, EpubReaderActivity.{cpp,h} | **C_CONFLICT_WORTH** | Two of three parts. (a) `deque` page LUT — **already** in CPYOS (`Section.h:54 std::deque<PageLutEntry> lut`). (b) `deque` anchor table — **missing**: CPYOS `ChapterHtmlSlimParser.h:146` is `std::vector<std::pair<std::string,uint16_t>> anchorData` (28 KB contiguous once a long chapter passes 512 anchors). (c) `yieldNow` hook — CPYOS already yields via `BUILD_PAGES_PER_CHUNK`/`RenderLock`. Recommendation: port (b) only.

### ui-a (7)

- `02d2a628` fix(home): hit-test home menu rows from theme geometry
  - CONFLICT | home/HomeActivity.cpp | C_CONFLICT_WORTH | CPYOS `HomeActivity::loop` still hit-tests with `metrics.menuRowHeight + menuSpacing` while `BaseTheme`/`CpyTheme::getMenuRowGeometry` exist (used by WeRead). Port the geometry-based hit test so legacy/theme home rows stay aligned; verify with `exp/refresh_soak.py` touch assertions.
- `50823ffa` fix: Center button hints using the actual text height
  - CONFLICT | themes/lyra/LyraTheme.cpp | C_CONFLICT_WORTH | CPYOS `BaseTheme::drawHintLabel(..., singleLineYOffset)` and `textYOffset = 7` are unchanged (pre-fix); BaseTheme.cpp hunk applies, Lyra edited differently (plus CPYOS `UpstreamStyleTheme` callsite). Small, low risk; needs one real-screen check of hint centering.
- `696fcc5c` fix: avoid restoring stale home cover pixels
  - CONFLICT | home/HomeActivity.cpp | C_CONFLICT_WORTH | CPYOS line 583 is exactly pre-fix `bool bufferRestored = coverBufferStored && restoreCoverBuffer();`. Take `coverRendered && coverBufferStored && …` (one-line, prevents a stale cover rect after a failed/blank render). Context conflict only.
- `6d649b66` fix: add sleep screen confirmation
  - CONFLICT | util/BmpViewerActivity.cpp | C_CONFLICT_WORTH | CPYOS `ImageViewerActivity::doSetSleepCover()` runs straight from the action button (no confirm). Port the intent using CPYOS's surface kit (`InlineConfirm`/`ConfirmationActivity`) so an accidental tap cannot overwrite `/sleep.bmp`; note CPYOS writes via `.part` + backup.
- `8e30a699` fix: stable tip positions in keyboard activity
  - CONFLICT | util/KeyboardEntryActivity.cpp | C_CONFLICT_WORTH | CPYOS has the same unstable math (`tipCount` depends on `!text.empty()`, `y = (underlineBottom + kbRect.y)/2 - …`). CPYOS's render() adds pinyin/IME strip and `hasMainTabs()/hasTouch()` guards, so the hunk must be hand-merged: reserve the Clear-Text row and cursor rows unconditionally, then centre in `[tipsTop, kbRect.y]`. Verify IME candidate strip still fits.
- `a0946097` perf: avoid list navigation callback allocations
  - CONFLICT | UiListActivity.cpp, home/HomeActivity.cpp | C_CONFLICT_WORTH | `cpyos` `UiListActivity::navigateButtons()` is exactly the pre-fix form (`[this, count, &n]`, `auto& n = activeNav()`). Port the lazy-`activeNav()`/`listCount()` version; also removes the stale-`count` behavioural bug. Skip the HomeActivity hunk (CPYOS home is `InxRecentActivity` dashboard; `cycleBand`/`getMenuItemCount` absent). Re-apply same idea to `LibraryListActivity::navigateButtons` (still captures `count`).
- `aad48b40` fix: Sub-chapter contents navigation
  - CONFLICT | Section.{cpp,h}, EpubReaderActivity.{cpp,h}, EpubReaderChapterSelectionActivity.{cpp,h} | C_CONFLICT_WORTH | Genuine gap: CPYOS has no `Section::getTocIndexForPage`; `EpubReaderChapterSelectionActivity` highlights only `getTocIndexForSpineIndex()` (first TOC item of a spine), so aggregated EPUBs (many TOC entries per XHTML, e.g. JJWXC 1-chapter-per-file is fine but omnibus books are not) show the wrong current chapter and "next chapter" jumps wrong. Port `Section::getTocIndexForPage` (read-only, no cache-format change; uses existing `openAnchorMap`/`BufferedFileReader` + `makeUniqueNoThrow`) and the chapter-selection call, then adapt the reader hunk to CPYOS's Contents panel (`ReaderToolbarUi`) instead of upstream's menu.

### ui-b (11)

- `0ecc7d78` fix: back tap open frontlight dropdown (#4276)
  - conflict | DictionaryDefinitionActivity.cpp, HeaderBackTapTarget.h, test | C_CONFLICT_WORTH | CPYOS's `HeaderBackTapTarget` has no padding, so a near-miss Back tap (12 px band) falls into the status-bar tap that opens the control center (ActivityManager.cpp:251). Port `containsUnpadded()` + padded `contains()`; CPYOS has no clippings long-press caller, so only the padded `contains` matters. Tiny patch.
- `2a8c1ec5` fix: show cache-clear warning inside the dialog (#4105)
  - conflict | ClearCacheActivity.cpp | C_CONFLICT_WORTH | CPYOS still paints a whole warning page + popup (ClearCacheActivity.cpp:42-51). `OptionPopup::showMessage` exists here, so the "warning inside the dialog, Cancel returns" shape is a small adaptation consistent with CPYOS's §51 surface kit. Low priority.
- `40a633e7` fix: reader menu selection (#4227)
  - conflict | USER_GUIDE, EpubReaderActivity.{cpp,h}, EpubReaderMenuActivity.{cpp,h} | C_CONFLICT_WORTH | CPYOS's `EpubReaderMenuActivity` has no `selectedAction` memory and no `onEnter`, so the classic list menu reopens at row 0 every time. Also hides the SYNC row when there is no credential/plugin subscriber. Adapt to CPYOS's ctor (which carries `isJjwxc`) and drop the `bleturner` condition.
- `51089beb` refactor: decouple sleep overlay from sleep mode (#3770)
  - conflict | english.yaml, CrossPointSettings.h, SettingsList.cpp, SleepActivity.cpp, BmpViewerActivity.cpp | C_CONFLICT_WORTH | Adds `COVER_TRANSPARENT` (cover under the alpha overlay) + gray-cover plane repaint. CPYOS has every prerequisite (`TRANSPARENT=7`, `renderTransparentOverlayPass`, `tryRenderTransparentOverlayBmp`, `scanForUsefulAlpha`) but its enum indexes differ and there is no `overlayCover` state. Worth taking as a feature (medium effort), not as a cherry-pick.
- `5d1142c1` feat(reader): add Image Grayscale toggle for EPUB image pages (#4160)
  - conflict | USER_GUIDE, DirectPixelWriter.h, GfxRenderer.h, english.yaml, SettingsList.cpp, CrossPointSettings.cpp, EpubReaderActivity.cpp | C_CONFLICT_WORTH | Adds a "Black & White" image mode using a 2×2 Bayer dither in the B/W pass and skipping the gray planes — exactly the kind of page-turn/refresh saving CPYOS cares about (its 16-level page path costs ~0.9 s). Needs porting into CPYOS's grayscale16/band pipeline (`imagesDitheredInBw` does not exist). Medium effort, clear product win.
- `6cff5326` fix(tapzone): own a Confirm that arrived already released
  - conflict | src/components/OptionPopup.h, test | C_CONFLICT_WORTH | Second half of the barrier: the render handoff can deliver a pass carrying only the *release* (wasPressed/isPressed both false). CPYOS's flag branch does swallow that case, but only while the flag is still armed; the deferred barrier makes it deterministic. Take with cdab81d8.
- `6fb907a6` fix(reader): route the chapter tap zone through the TXT-aware navigation
  - conflict | src/activities/reader/EpubReaderActivity.{cpp,h}, test/CMakeLists.txt | C_CONFLICT_WORTH | Real bug in CPYOS: `gotoNextZoneChapter()` (EpubReaderActivity.cpp:2861) steps `currentSpineIndex`, so TXT/MD books (named chapters are anchors inside one spine) get a dead tap. CPYOS already has the TXT-aware `gotoChapter` lambda inline in `handleOverlayInput` (≈6427). Port = extract `currentChapterIndex()`/`gotoChapterIndex()` and call it from `gotoNextZoneChapter` **and** the overlay lambda. Small, high value.
- `8cf400c6` fix(dictionary): support multiple idx entries with same string (#4110)
  - conflict | Dictionary.{cpp,h}, DictionaryDefinitionActivity.{cpp,h}, DictionaryWordSelectActivity.cpp, docs, test | C_CONFLICT_WORTH | **Real defect still present.** CPYOS's `Dictionary::lookup(..., DictLocation*)` returns only the first `.idx` hit (Dictionary.h:132 region, no `locateAll`), so a headword with several parts of speech shows one entry. Port `DictEntryList` + `locateAll` + strict `<` bisect semantics into CPYOS's qidx/sidx implementation. Highest-value C item in this slice.
- `cdab81d8` fix(tapzone): own a fresh Confirm after the popup opens
  - conflict | src/CrossPointSettings.cpp, src/components/OptionPopup.h, 2 tests | C_CONFLICT_WORTH | CPYOS arms `ignoreInitialConfirmRelease = true` in `finishShow()` (OptionPopup.h:298); upstream replaces it with a deferred `confirmBarrierPending` decided on the first input pass. CPYOS's variant swallows a genuine press whose press-edge pass is missed. Port the barrier only; the CrossPointSettings.cpp hunk is whitespace. Pairs with 6cff5326.
- `e59f87b6` fix: truncate the Wi-Fi SSID by width, not bytes (#4079)
  - conflict | WifiSelectionActivity.cpp | C_CONFLICT_WORTH | CPYOS still truncates the "To <SSID>" line with a 26-byte buffer + `utf8SafeTruncateBuffer` (WifiSelectionActivity.cpp≈2038). Replace with `renderer.truncatedText(...)` in the CPY connecting sheet (and update the host test that pins the 26-byte ellipsis).
- `f73d788a` fix: OPDS crashes on the book details screen (#4165)
  - conflict | OpdsBookBrowserActivity.cpp (DetailsActivity.{cpp,h} deleted in CPYOS) | C_CONFLICT_WORTH | The crash fix is `RenderLock` around `releaseEntries()`/`fetchFeed()` state mutation in `OpdsBookBrowserActivity`, which CPYOS still has unchanged. Drop the DetailsActivity hunks (upstream details screen was replaced by CPYOS's booksource detail) and take the locking. Medium risk, real crash.

### fonts-i18n (11)

- `3f387a98` fix: Text Settings do not render correctly in landscape mode (#4172)
  - conflict 5 files / 6 hunks | `UiTabListActivity.cpp`, `TextSettingsActivity.cpp`, `TextSettingsPreview.cpp`, `german.yaml`, `azerbaijani.yaml` (modify/delete) | **C_CONFLICT_WORTH** (medium) | CPYOS's rewritten page still lays out from raw screen metrics — `TextSettingsActivity.cpp:129-132`, `:456`, `:634` use `renderer.getScreenHeight()`/`afterHeader` with **no `getScreenSafeArea()` insets**, so the same landscape/clipping class of bug is plausible. Patch must be reworked onto CPYOS's page (extra tab-bar action, 1-bit/16-level preview) and needs the new `STR_HYPHENATION_NO_PREVIEW` / `…_EMBEDDED_STYLE_NO_PREVIEW` / `…_TEXT_AA_NO_PREVIEW` keys instead of `STR_NOT_IN_PREVIEW`.
- `42d58daa` chore: update translations (#4283)
  - conflict 11 files / ~12 hunks | `catalan/german/hebrew/italian/polish/russian/spanish/swedish/valencian/vietnamese.yaml`, `azerbaijani.yaml` (modify/delete) | **C_CONFLICT_WORTH** (low) | Community pass: +1806 lines. **Key sets diverged** (English 1789 vs 1360; per-language 520 vs 660) and 41 of the 148 Spanish keys it touches exist in CPYOS's English set (13 of those already translated) → merge **keys, not files**, per language (~28 missing strings each). Also worth porting verbatim: the `docs/i18n.md` "whitespace inside quotes is significant / translate whole sentences" rule and `test/test_translation_spacing.py` (CPYOS has `scripts/gen_i18n.py::parse_yaml_file`, so it drops in). Any key-count change ⇒ re-run `gen_i18n.py` and reflash the whole data partition.
- `4663d177` perf: share SD font interval tables across loaded fonts (#4145)
  - conflict 3 files / 5 hunks | `SdCardFont.cpp`, `SdCardFont.h`, `SdCardFontTest.cpp` | **C_CONFLICT_WORTH** (medium) | Content-matched, refcounted interval sharing across fonts (48 KB once instead of per UI size for LXGW WenKai). Context is post-`9610da70` (`kernBlockIndex`, `freeStyleLigatures`), so it **requires** that rewrite. CPYOS keeps intervals in PSRAM (`psramDeleteArray`), so the OOM motivation is weak; the independent win is `+ RenderLock lock;` in `EpubReaderActivity::applyReaderTextSettings()` (render task can load/unload variants concurrently) — worth taking **even without** this commit.
- `51e0baa2` fix: fix ttf font failure and memory cleanup (#4284)
  - conflict 1 file / 2 hunks (include + insertion point only) | `src/SdCardFontSystem.cpp` | **C_CONFLICT_WORTH** (high value, cheapest) | `TtfEpdFont.cpp/.h` + `FontCacheManager.cpp/.h` + test apply **clean**; only `SdCardFontSystem::begin()` conflicts (CPYOS added the `SystemFontPack.h` include and a legacy-NotoSans cleanup block). Contents: bounded retry of `initFace()` after `MemoryManager::ensureFree(16 KB)` (`initFailures`, no `ensureFree` thrash), **per-style prewarm** (`build/addCoverage(utf8, style)` + `FontCacheManager` honouring `styleMask`), and a `FontCacheManager::setReleaseCallback` so `releaseSdFontCaches()` also drops TTF variants. Directly relevant: CPYOS's reader is TTF-first and its bionic-reading bold uses real bold faces (§49.11), which today are prewarmed/never — resolve by adding `setReleaseCallback(…clearTtfVariants…)` next to `renderer_ = &renderer`.
- `70b3b1a3` perf: reduce font download heap (#4124)
  - conflict 4 files / 8 hunks | `SdCardFontManager.cpp/.h`, `FontDownloadActivity.cpp`, `test/CMakeLists.txt` | **C_CONFLICT_WORTH** (medium) | Two ideas: rescan the registry **before** TLS/JSON buffers exist, and stop keeping a `std::string` path per `.cpfont` (family stores its root; `SdCardFontFamilyInfo::filePath()` rebuilds, `name` only for non-canonical/vector files). CPYOS still stores `info.path = std::string(rootPath) + "/" + nameBuffer` per file, and its vector-font discovery makes the refactor wider. Real gain only on cards with many families.
- `7394e40f` feat: render redaction blocks without a special font (#3882)
  - conflict 6 files / 11 hunks | `docs/file-formats.md`, `lib/EpdFont/EpdFont.{cpp,h}`, `EpdFontFamily.h`, `Epub/Epub/Section.cpp`, `lib/GfxRenderer/GfxRenderer.cpp`, test | **C_CONFLICT_WORTH** (low) | Draws U+2588/U+25A0 as sized rectangles when the face lacks them (`getGlyphMetrics` + `syntheticGlyph`), and bumps `SECTION_FILE_VERSION` (CPYOS is at 86/87 → trivial). CPYOS has **no** solid-block handling (`git grep 0x2588/0x25A0 lib src` → only tables/presets). Its system pack covers both codepoints, so the gain is limited to SD `.cpfont`/embedded faces that omit them. Conflicts are real: CPYOS's `renderCharScaled` signature and 4-bit path differ → needs a hand-written 4bpp/BW solid fill.
- `83df537d` perf: skip the advance-table scan once the table is full (#3948)
  - conflict 2 files / 5 hunks | `lib/EpdFont/SdCardFont.cpp`, `SdCardFontTest.cpp` | **C_CONFLICT_WORTH** (low) | Half already present: CPYOS caps `MAX_UNIQUE_CODEPOINTS = ADVANCE_CACHE_LIMIT` (~3 KB scratch) with the same rationale comment and `hitCap` flag. Residual value: (a) return before allocating when every requested style's table is full, (b) 32-codepoint inline buffer so short table words never touch the heap, (c) lowest-N collection. Written against `styles_[si].present` (post-`b912e000`) → conflicts. Only worth it together with `b912e000`.
- `9610da70` perf: stop SD-font caches from splitting the heap while reading (#3831)
  - conflict 4 files / 14 hunks (`SdCardFont.cpp` alone) | `SdCardFont.{cpp,h}`, `GfxRenderer/GfxRenderer.cpp`, `SdCardFontTest.cpp` | **C_CONFLICT_WORTH** (large) | Removes the resident ~3 KB-per-style kern class tables (block index read from SD per page), adds advance-table capacity doubling, rewrites the kern path on top of `f1d0af1a`. Real internal-RAM value on a 94 KB device **if** users read with SD `.cpfont`. CPYOS's file already diverged (1992 vs 1855 lines) → 14 hunks, highest-risk item in the slice. Do **not** take before `f1d0af1a`, and only with device heap measurements.
- `b912e000` perf: store only present SD font styles (#4114)
  - conflict 1 file / 9 hunks | `lib/EpdFont/SdCardFont.cpp` | **C_CONFLICT_WORTH** (low) | Allocates style records by the file's style count (~800 B/font, 3.2 KB for 4 sizes of a 2-style Korean font) and rejects duplicate style ids. CPYOS still has `PerStyle styles_[MAX_STYLES] = {}` (4 fixed 404 B records) with `hasStyle()`; the refactor touches the whole file. Value is real but smaller than on X4 (CPYOS's reader usually uses the pack, not `.cpfont`). Gate behind `f1d0af1a`.
- `e78f09fd` fix: translate complete confirmation questions (#4097)
  - conflict 6 files / ~6 hunks | `english.yaml`, `FileBrowserActivity.cpp`, `LibraryListActivity.cpp`, `BmpViewerActivity.cpp` (modify/delete), `azerbaijani.yaml`, `test/CMakeLists.txt` | **C_CONFLICT_WORTH** (small) | Real defect still live in CPYOS: `tr(STR_DELETE) + "? "` at `FileBrowserActivity.cpp:569` and `LibraryListActivity.cpp:629` (+ debug hook `:2435`) glues English punctuation onto a translated label. Port **keys, not files**: add `STR_DELETE_ITEM_CONFIRM` (+ `STR_SET_SLEEP_COVER_CONFIRM` if a sleep-cover flow exists) and replace the 3 sites. `BmpViewerActivity` does not exist in CPYOS.
- `f1d0af1a` fix: keep SD-font kerning and ligatures in edge cases (#3838)
  - conflict 2 files / 3 hunks | `lib/EpdFont/SdCardFont.cpp` (kern top-up + `kernLigOk/tablesOk`), `test/sd_card_font/SdCardFontTest.cpp` | **C_CONFLICT_WORTH** | The bug-fixes themselves land clean; only the prewarm top-up hunk conflicts. Includes an **architecture-independent memory-safety fix**: a class id `> kernLeft/RightClassCount` indexes past the row buffer (`heap read past allocation`, fonts are user-supplied), plus `uint8_t` loop counters that never terminate at 255 classes. CPYOS still has `uint8_t numLeft` (SdCardFont.cpp:574), no `miniKernBuilt`, no clamp → **not present**. Take this one first; it is 3 small hunks. Note `9610da70` subsumes it.

### network (13)

- `26ccbe28` fix: say why an OPDS feed fetch failed
  - conflict (1) | src/network/HttpDownloader.cpp | **C_CONFLICT_WORTH** | Adds `HttpDownloader::lastStatus()`/`lastAnswerRedirected()`, `src/util/OpdsFeedError.h` (pure classifier: 401→credentials, 401-after-cross-origin-redirect→https hint, 5xx→server, no response→unreachable, 403 stays generic) and 4 EN strings. CPYOS `DownloadError` (HttpDownloader.h:25) already has extra `NO_WIFI`; add the statics beside it. Apply in CPYOS's own 288-line `OpdsBookBrowserActivity::fetchFeed` (no publication-detail path). 4 new i18n keys ⇒ i18n pack bump ⇒ data-partition reflash.
- `2ec01c96` perf: free about 5KB of heap while the web server runs
  - conflict (2) | src/network/CrossPointWebServer.{cpp,h} (+ platformio.ini hunk) | **C_CONFLICT_WORTH** | Three separable wins, all still absent in CPYOS: (a) `PluginJob pluginJobs[MAX_PLUGIN_JOBS]` static (445 B × 6 ≈ 2.7 KB, CrossPointWebServer.h:199) → allocate on first job submit, `std::span` accessor; (b) `NetworkUDP udp` (~1.4 KB, :83) → plain lwIP socket for discovery replies; (c) `-DWEBSOCKETS_SERVER_CLIENT_MAX=2` + idle-client reaper — CPYOS has none of that and a global `wsUploadInProgress` lock (:150) that a vanished uploader holds until TCP gives up, blocking every later upload. Land in this order; (a)+(b) are the internal-RAM win CPYOS cares about.
- `39e56ca1` fix(plugins): paginate static catalogs after compat filtering
  - conflict (1) | src/activities/plugins/PluginCatalogActivity.cpp | **C_CONFLICT_WORTH** | Real kernel that applies to CPYOS today: a catalog whose browse URL has no `{page}` (static JSON) **repeats page 1 forever** — CPYOS `fetchPage` re-requests the same URL and sets `hasMore = items.size() > pageSize` (PluginCatalogActivity.cpp:573-629). Port only `isStaticFeed` + `PageWindow(pageOffset, pageSize)` local windowing; **drop the compatibility branches** (CPYOS has no `filtersCompatibility`). Adapt to CPYOS's `catalogHasSpace` row budget.
- `552c932b` perf: stream plugin relay replies instead of buffering
  - conflict (5) | docs/webserver-endpoints.md, CrossPointWebServer.cpp, html/js/plugin-host.js, src/util/PluginHttp.{h,cpp} | **C_CONFLICT_WORTH** | CPYOS `handleRelay` (:2114) still buffers the whole reply into a `String` (32 KB cap) then re-sends it. Adds `pluginhttp::requestStream()` (onStart headers/status/length + onData chunks), sends `X-Relay-Status`/`X-Relay-Headers` then `sendContent`, caps at 64 KB, and `client().stop()` on an incomplete body. Note CPYOS's `pluginhttp::request` never had the `heapReserve` parameter this patch deletes — that hunk is a no-op here.
- `7990db2b` fix: count untitled rows on server catalog pages
  - conflict (4) | docs/sd-plugins.md, PluginCatalogActivity.cpp, PluginCatalogPaging.h, test/plugin_catalog_paging/* | **C_CONFLICT_WORTH** | Refinement of the paging bundle (`countsTowardPage`: server pages count every row it sent, static lists count only shown rows). Only meaningful after c3b16da5 + 39e56ca1; fold in, don't land alone.
- `91e9efc3` fix: make web Fonts and Settings pages work with many SD font families
  - conflict (2) | src/network/CrossPointWebServer.cpp, src/network/html/FontsPage.html | **C_CONFLICT_WORTH** | **CPYOS has the pre-fix code**: `handleGetSettings` silently drops an oversized `fontFamily` entry (:1652 "Skipping oversized setting JSON"), and `handleFontList` (:3006) builds one whole document for up to `MAX_SD_FAMILIES = 128` (SdCardFontRegistry.h:44). CPYOS now *encourages* installing many big CJK/emoji families (font-extras), so this is a live limit. Port the server side (heap fallback per entry + streamed one-family-per-chunk list); adapt the page hunk to CPYOS's en/zh FontsPage.
- `b4d9458f` fix: add hold-to-uninstall for plugins
  - conflict (2) | src/activities/plugins/PluginCatalogActivity.h, USER_GUIDE.md | **C_CONFLICT_WORTH** | CPYOS has `State::PLUGIN_PICKER`, `installedPlugins`, `PluginLocations::ROOTS`, but **no way to uninstall a plugin** and `PluginRef` has no `dir`. Port: `PluginRef.dir`, 1000 ms hold in the picker (`CatalogActivity` ctor flag `wantsTouchLongPress`), validated recursive `Storage.removeDir`, 3 i18n keys. Safety validation (name/dir must match a ROOT) is part of the patch — keep it.
- `c3b16da5` fix: plugin catalog paging with {page}+{limit}
  - conflict (2) | src/activities/plugins/PluginCatalogActivity.cpp, PluginCatalogPaging.h (new) | **C_CONFLICT_WORTH** | Introduces `PluginCatalogPaging.h` (`Paging::Static/SizedPages/IndexPages`, `PageWindow`, `limitFor`, `hasMore` lookahead). Prerequisite for 39e56ca1/7990db2b — land the three as one adapted bundle.
- `c9cb1f8d` perf: add ETag to plugin file responses
  - conflict (1) | src/network/CrossPointWebServer.cpp | **C_CONFLICT_WORTH** | CPYOS `handlePluginFile` (:2084) re-sends the whole plugin file every page load. Adds an MD5 **content** ETag (not size/mtime — no RTC), `Cache-Control: no-cache`, and `If-None-Match` → 304. ~25 lines; `If-None-Match` is already collected by CPYOS.
- `d6c1512e` Merge PR #418 weread x3 sync
  - conflict (1) | src/activities/apps/weread/webapi/WeReadActivity.cpp | **C_CONFLICT_WORTH** | CPYOS **already has** `retryPhase`, `renewalAttempted_`, `previousVid_`, `newConnections_/reusedRequests_` (WeReadClient.{h,cpp}). What is still missing: (a) `acceptChapterStatus()` — a chapter/reader **403 is a purchase restriction, not an expired session**, while CPYOS `WeReadClient.cpp:2975` returns `SessionExpired` (forces re-auth mid-book); (b) `AbortCallback` threaded through `requestOnce`. Port those two concepts + `scripts/tests/test_weread_connection.py`.
- `db9dd391` feat: method and body for /api/fetch
  - conflict (2) | docs/webserver-endpoints.md, src/network/CrossPointWebServer.cpp | **C_CONFLICT_WORTH** | CPYOS `handleFetch` (:2358) is GET/resumable only. Adds method GET/POST/PUT/PATCH/DELETE, 4 KB request-body cap, 512-byte error-body capture, and refuses `offset`/`maxBytes` for non-resumable requests (no replay of a side-effecting verb). Feature for plugin authors; the caps are the part that matters for device memory.
- `e8299b5f` fix: keep the web server responsive and within memory while plugins load
  - conflict (4) | src/SettingsList.{h,cpp} (cpp absent), CrossPointWebServer.cpp, html/js/plugin-host.js | **C_CONFLICT_WORTH** | Two independent, cheap wins: (a) override `_currentClientWrite`/`_currentClientWrite_P` to **close a client that stalls mid-reply** — the base `WebServer` ignores short writes and a stalled peer blocks the single-threaded server for minutes (CPYOS already subclasses WebServer at CrossPointWebServer.cpp:77, so ~20 lines); (b) `ChunkedReply` (1 KB `Print` buffer) so streamed JSON doesn't do hundreds of tiny socket writes. **Skip** the route-table refactor: CPYOS has extra routes (`/api/relay`, `/api/crypto`, `/api/fetch`, `/api/book-key`, `/api/plugin-fs`, `/api/wifi*`) that would have to be re-added and re-audited.
- `eaf5c73a` fix(weread): preserve annotations as EPUB footnotes
  - conflict (7) | src/activities/reader/EpubReaderActivity.{cpp,h}, src/util/BookCacheUtils.cpp, docs/engineering/cache-management.md, test/content_opf_parser/*, test/inx_navigation/test_tap_zone_chapter.py (deleted here) | **C_CONFLICT_WORTH (partial)** | 34 files. The WeRead-highlight half is **declined by product decision** (AGENTS §30: 「微信读书划线不做」). Generic part still worth having: `book.bin` non-linear spine `linear` flag (auxiliary resources excluded from chapter nav **and** progress weights), `Epub::findSpineForProgress` + `ProgressMapper` (CPYOS ProgressMapper.cpp:914 still does the naive linear scan that skips non-linear bytes), EPUB3 footnote navigation in the reader. Must renumber CPYOS's own book/section versions and re-verify the JJWXC protected-EPUB open path (`CPRO open`) after the version bump.

### build-docs (5)

- `28971493` docs: clarify non-Latin UI fallback isn't CJK-specific
  - conflict | docs/sd-card-fonts.md | **C_CONFLICT_WORTH (docs only)** | Reword + a callout saying a non-Latin `.cpfont` needs the **UI sizes (8/10/12 pt)** as well as the reader sizes, otherwise titles/rows stay blank. That fact explains a class of CPYOS reports too (the built-in pack is CJK+Latin only, readers must supply a font for Greek/Cyrillic/Armenian). Hand-merge the wording into CPYOS's `docs/sd-card-fonts.md` (conflict is only because CPYOS rewrote the CJK section for the system font pack).
- `9ea86717` perf: skip unused Arduino SDK libraries (#3857)
  - clean textually (26+/5-), **semantic conflict** | platformio.ini | **C_CONFLICT_WORTH** | `CONFIG_ARDUINO_SELECTIVE_*` = EEPROM/Ethernet/FFat/HTTPUpdate/LittleFS/Matter/NetBIOS/OpenThread/PPP/SD/SD_MMC/SPIFFS/Ticker/WiFiProv/Zigbee + `esp-zigbee-lib`/`esp-zboss-lib` removal are safe (public has **no** `#include <SD.h>/<Update.h>/<SPIFFS.h>/<Ticker.h>` anywhere in src/lib). **Do NOT take** `CONFIG_BT_ENABLED=n`, `..._BLE=n`, `..._BluetoothSerial=n`, `..._SimpleBLE=n`, nor the two deleted `CONFIG_BT_NIMBLE_*` lines in `[firmware_tuned_c3]`: readpico extends `s3_ble_controller` and `c3_ble` sets `CONFIG_BT_ENABLED=y` (BLE HID host, `FREEINK_CAP_BLE_HID_HOST=1`, `src/platform/BtLibraryInUseShim.h` relies on a missing `_bleLibraryInUse` in controller-only cores). Take the subset, then build `readpico` + `default` + `readpico_nosource`.
- `9f4e387d` docs: explain IPA in dictionary pronunciations (#4034)
  - conflict | docs/dictionary.md | **C_CONFLICT_WORTH (docs only, user-visible)** | Two lines: dictionaries draw with the reading font, IPA glyphs are missing from the built-in fonts and render as `�`; install an SD font covering IPA. This is **true for CPYOS** (`CPYOSHanSansSC-Regular-8105.ttf` ships CJK+Latin+punct only, no IPA) and is a repeated support question. Hand-merge into `docs/dictionary.md`.
- `f2a1beda` fix: rebuild core when component lists change (#4214)
  - conflict | .github/workflows/ci.yml, .github/workflows/release_candidate.yml, scripts/patch_pioarduino_cache.py | **C_CONFLICT_WORTH — HIGH VALUE** | CPYOS `scripts/patch_pioarduino_cache.py:97` still computes `cache_request = requested + memory_fingerprint`, i.e. **a changed `custom_component_add/remove` does not invalidate the "core already rebuilt" record** — exactly the AGENTS §4.3/§6 failure mode where a component-list edit silently reuses the old core. Resolution: keep CPYOS's `--prepare-platform` block, add `COMPONENT_OPTIONS`/`build_cache_request()` verbatim, and call it with `env.GetProjectOption("custom_component_remove")` (+ `custom_component_add`). Add `--self-test` cases. Workflows: CPYOS's cache keys live in `cpyos-release.yml` — add `'scripts/patch_pioarduino_cache.py'` to `hashFiles()` there (upstream's `ci.yml`/`release_candidate.yml` do not exist here).
- `fe042860` fix(txt): reject incomplete BMP cover copies
  - conflict | lib/Txt/Txt.cpp, test/CMakeLists.txt | **C_CONFLICT_WORTH** | **Real bug, still present:** CPYOS `lib/Txt/Txt.cpp:176-181` uses `while (src.available()) { dst.write(buffer, bytesRead); }` with no size check, so a full/short card leaves a truncated `cover.bmp` that `exists()` accepts forever. Resolution: replace that block with upstream's bounded `while ((bytesRead = src.read(...)) > 0)` + `copiedBytes != sourceSize` check. CPYOS `HalFile` already exposes `int read(void*,size_t)` and `uint64_t fileSize64()`, so no API gap. Test file is new (`scripts/tests/test_txt_cover_copy.py`) — add the `test/CMakeLists.txt` target by hand (CPYOS's list differs).

### storage-misc-a (12)

- `00897520` feat: accept server paths in JSON plugin catalogs
  - CONFLICT | docs/sd-plugins.md, PluginCatalogActivity.cpp, FirmwareIdentity.h, UrlUtils.{cpp,h}, test | **C_CONFLICT_WORTH** | Needs `pluginhttp::escapeUrl()` (9ac185c8) and `util/PluginPickerLayout.h` (79915f07) — apply in that order or it will not compile.
- `09fedb97` fix: refresh Library after plugin book installs
  - CONFLICT | lib/LibraryIndex/LibraryBuilder.cpp, src/network/CrossPointWebServer.cpp | **C_CONFLICT_WORTH** | CPYOS still has the private `isBookName()` (no `isLibraryBookFile(std::string_view)`), so the plugin-install path cannot probe "is this a library book" without re-walking. Manual merge; no cache-format change (`library.bin` unaffected).
- `3db7a96b` fix: validate cached cover size against the BMP pixel data offset
  - CONFLICT | src/util/BookCoverLoader.cpp, tests, stubs/Bitmap.h | **C_CONFLICT_WORTH** | **Final state of the cover-validation chain** (`bb4f8785 → 186d9407 → 12e441f7 → 3db7a96b`): `bfOffBits + rowBytes*height <= fileSize`. CPYOS's `inspectCachedCover()` still returns Ready on header-parse alone, so a 62-byte stub cover is accepted forever. Needs `Bitmap::getPixelDataOffset()` added; resolve against CPYOS's `_g8` Gray8 thumbs + `ThumbSlot` cache (no file-format change; stale stubs are deleted → regenerate).
- `69c8d867` fix(dict): show all matching `.syn` definitions
  - CONFLICT | lib/Txt/TxtToHtml.*, DictionaryDefinitionActivity.*, DetailsActivity.cpp, Dictionary.{h,cpp}, HtmlToPlainText.*, tests | **C_CONFLICT_WORTH** (medium) | CPYOS only has the singular `Dictionary::locateSynonym()` + its own `.sidx` sidecar fork; merge is manual (plural `locateSynonyms()` + append-mode `readDefinition()` + bold headword labels). Real UX gain for Japanese/multi-headword `.syn`, no format change.
- `7365c1ac` fix: keep cumulative spine sizes in book.bin instead of RAM
  - CONFLICT | lib/Epub/Epub/BookMetadataCache.{cpp,h}, tests/stubs | **C_CONFLICT_WORTH** | CPYOS holds an OOM-safe `unique_ptr<uint32_t[]> cumulativeSizes` (from `ddadc36f`); upstream replaces it with a 32-entry window + 1 SD refill. **No `book.bin` format change** (it re-reads the already-stored `cumulativeSize`), but the window must keep CPYOS's nothrow/failed-read fallback or progress/percent jumps regress. Saves ~4 B/spine.
- `83f9cc05` fix(time): Use RTC for plugin time
  - CONFLICT | src/main.cpp | **C_CONFLICT_WORTH** (low) | CPYOS `HalClock::localTime()` exists, so the hook compiles; `trustedtime::adopt()` is new (floor/plausibility window). Only matters on boards with a battery RTC (Metalio/readpico PMU). `main.cpp` conflict is CPYOS's STUCK detector/debug commands — place the hook right before `trustedtime::init()` (CPYOS line ~1323).
- `9ac185c8` fix: URL-encode plugin template variables
  - CONFLICT | docs/*, PluginCatalogActivity.cpp, FirmwareIdentity.h, PluginHttp.{h,cpp} | **C_CONFLICT_WORTH** | Real bug: CPYOS still calls `http.begin(url)` and has no `pluginhttp::escapeUrl()`/`{id_enc}`/`catalog.encoded_item_fields`. Small conflicts (capability string + `begin()` line). **Prerequisite for 00897520.**
- `9dd39984` fix: check cached cover payload on Home fast paths
  - CONFLICT | src/activities/home/HomeActivity.cpp, src/util/BookCoverLoader.{cpp,h} | **C_CONFLICT_WORTH** | CPYOS has the probe (`BookCoverLoader::cachedThumbnail()`), but `HomeActivity::loadRecentCovers()` still gates on bare `Storage.exists(coverPath)` (line ~169) and `inspectCachedCover` is still file-private. Export a `checkCachedCover()`-equivalent and use it; also cover `InxRecentActivity` (CPYOS's default home).
- `b6605a51` fix: decode inline dict type
  - CONFLICT | docs/dictionary.md, EpubReaderActivity.cpp, Dictionary.{h,cpp}, test | **C_CONFLICT_WORTH** (medium) | CPYOS's `readIfoFacts()` is the old 2 KB `strstr` version — no `inlineTypes`. Merge is manual (CPYOS added `.sidx`/syn bookkeeping around it).
- `e54c0087` fix: Metalio E-Ink 4 powers off fully when it sleeps
  - CONFLICT | freeink-sdk, lib/hal/HalPowerManager.cpp, src/main.cpp | **C_CONFLICT_WORTH** | CPYOS *does* build `env:metalio_eink4`. Take the three app-side hunks (`isMetalioEInk4()` in `coldBootImpliesPowerButton`, single `metalio::powerOff()`, skip `verifyPowerButtonWakeup`); **do not** bump the gitlink — re-express the SDK half as a `patches/freeink-sdk/` hunk, otherwise CPYOS's 31-file patch is destroyed.
- `f47a218b` feat(kosync): add server profiles and extended metadata
  - CONFLICT | src/activities/settings/KOReaderSettingsActivity.cpp | **C_CONFLICT_WORTH** (medium-low) | CPYOS has no `SyncMetadata`/series/ISBN-ASIN and no profiles; `ContentOpfParser.cpp` applies clean but CPYOS must keep its `embeddedFonts` manifest collection. Touches `SettingsList.h` + `english.yaml` (i18n key count ⇒ data-partition rebuild). No `BookMetadataCache` change ⇒ no `book.bin` format bump.
- `f889b828` fix: dictionary back overflow bug
  - CONFLICT | docs/dictionary.md, src/activities/reader/ClipSelectionActivity.cpp, test/test_clipping_selection_text.py | **C_CONFLICT_WORTH** | `ClipSelectionActivity` does not exist in CPYOS (AnnotationStore instead) ⇒ take only the `Dictionary.{h,cpp}` part (XDXF `sametypesequence=x` + plain-text-fallback heap headroom). CPYOS has `DEFINITION_HEAP_HEADROOM_BYTES` and `definitionsAreHtml()` but no XDXF.

### misc-b-sdk (11)

- `116195b4` fix: ignore system folders in Library (#4184)
  - conf: lib/LibraryIndex/LibraryBuilder.{cpp,h}, src/network/CrossPointWebServer.cpp, test/library_builder/LibraryBuilderTest.cpp | as listed | C_CONFLICT_WORTH | **Real gap, cheap fix.** CPYOS `LibraryBuilder.cpp:202` is still `isHiddenOrSidecar(name) { return name[0]=='.'; }`, so `System Volume Information` / `XTCache` folders get indexed. Do **not** add upstream's `lib/FsHelpers/HiddenItems.h` — CPYOS already has `FsHelpers::isProtectedPathComponent()` (`lib/FsHelpers/FsHelpers.cpp:218`, same two names) and `CrossPointWebServer`/`WebDAVHandler` already carry the HIDDEN_ITEMS arrays. Resolution: make `isHiddenOrSidecar` take `std::string_view` and call `FsHelpers::isProtectedPathComponent`, plus the same check inside `isBookName`/path validation.
- `148e6e2b` fix: XTCH books missing from library, XTC books show filename and unknown author (#4023)
  - conf: lib/LibraryIndex/LibraryBuilder.cpp, test/library_builder/LibraryBuilderTest.cpp | both | C_CONFLICT_WORTH | **Real gap.** CPYOS `LibraryBuilder::isBookName` (`:191`) tests `.xtc` only — `.xtch` books never index even though `FsHelpers::hasXtcExtension()` (xtc+xtch) exists (`lib/FsHelpers/FsHelpers.cpp:175`). And CPYOS's metadata pass only has `isEpubName ? Epub : Fb2::probeMetadata` (`:354-363`) — XTC falls into the Fb2 branch, so title/author never come from the XTC header. Resolution: use `hasXtcExtension` in `isBookName` and add the `Xtc` branch (`xtc.load()/getTitle()/getAuthor()`) before the Fb2 fallback; keep CPYOS's `titleFromBook`/`authorFromBook` bookkeeping.
- `20516ddc` test(tapzone): drive the Confirm entry states through finishShow
  - conf: test/inx_navigation/test_touch_network.py | that file | C_CONFLICT_WORTH | Test-only, but it rewrites exactly the `OptionPopup` Confirm-entry case CPYOS lists as a **known pre-existing failure** ("popup 段在 AGENTS §55.5 之前就失败 … assert(p.selectedIndex==0) SIGABRT"). The premise holds in CPYOS (`ignoreInitialConfirmRelease` still exists at `src/components/OptionPopup.h:165`, and 1fd5ea3d's first-pass confirm is absorbed). Worth porting to unblock the suite; resolve by hand against CPYOS's stub header.
- `4042e454` fix: open the hotspot captive portal on phones (#4211)
  - conf: src/network/CrossPointWebServer.cpp, test/CMakeLists.txt | 2 | C_CONFLICT_WORTH | CPYOS still has the old pre-handler redirect (`src/network/CrossPointWebServer.cpp:663`, `handleNotFound` only) so iPhone/Android probes (`/hotspot-detect.html`, `captive.apple.com`) that WebDAV swallows never open the portal. `src/util/UrlUtils.{h,cpp}` hunks apply cleanly (CPYOS has no `isForeignHost`). Core's WebServer already ships `addMiddleware` (`middleware/`, `Middlewares.h`). Resolution: add `UrlUtils::isForeignHost` + the AP-mode middleware and `Host` in `collectHeaders(…,8)`; keep CPYOS's `/api/…` handling; drop the docs/test-CMake hunks.
- `4c9ec287` fix(font): render .notdef glyph for replacement characters in TTF fonts (#4256)
  - conf: freeink-sdk, docs/file-formats.md, lib/Epub/Epub/Section.cpp (TtfEpdFont.cpp applied clean) | as listed | C_CONFLICT_WORTH | **App-side one-liner still missing**: CPYOS `lib/EpdFont/TtfEpdFont.cpp:328` is `if (gid == 0) return nullptr;` — a codepoint absent from a vector face (very common with CPYOS's 8,105-glyph pack and the Ext-B names in AGENTS §45.3) resolves to gid 0 and is *dropped*, so the word loses the glyph **and its advance** instead of showing tofu. Fix: `if (gid == 0 && cp != REPLACEMENT_GLYPH) return nullptr;`. Resolution notes: bump CPYOS's own `SECTION_FILE_VERSION` (86/87, not upstream's 73) so cached line breaks rebuild; keep CPYOS's docs wording. ★ Upstream calls this the companion of a SDK PR (freeink-sdk#165) — verify with the font owner that `rasterizeGlyph26_6(0, …)` produces sane metrics on 96de1be before relying on it (worst case: ink box + advance identical to upstream, no crash).
- `60d3c8bb` fix: Scale custom covers up & let you set them on touch boards (#4185)
  - conf: freeink-sdk, USER_GUIDE.md, src/activities/boot_sleep/SleepActivity.cpp, src/activities/util/BmpViewerActivity.cpp (deleted), test/CMakeLists.txt | as listed | C_CONFLICT_WORTH | New `lib/BmpToBmpConverter/{.h,.cpp}` (178 lines: 1/2-bpp BMP → BMP with layout fit + Atkinson dither, sequential SD reads) and `Bitmap::readRowGray/isTopDown/is1Bit` additions apply cleanly; CPYOS has **no** BMP upscaling at all (`git grep BmpToBmpConverter` = 0) so a small custom sleep cover is shown unscaled / rejected. SleepActivity is CPYOS-owned (night sleep, duck, AirPage) → port the `selectSleepImage`/cover-source plumbing by hand, keep CPYOS's custom-image settings instead of upstream's. Also depends on `GifCommon::calculateLayout` (already present in CPYOS).
- `6f6bac9a` fix(storage): recover retained JSON staging files
  - conf: lib/Serialization/PersistableStore.cpp, test/persistable_store/{CMakeLists.txt,check_write.py} | as listed | C_CONFLICT_WORTH | Upstream recovers a retained `<path>.tmp` on load. CPYOS replaced remove-then-rename with rename-to-`.bak`, and `readDocFromFile` already falls back to `.bak` (`PersistableStore.cpp:64-79`) — so the *primary* failure mode upstream protects against is already covered, and adding `.tmp` promotion on top risks resurrecting a stale staging file over a good `.bak`. **Recommendation: do not port**; if kept, add it as a *third* fallback after `.bak`, not before (and mirror it in `JsonSettingsIO::saveJsonDocumentToFile`).
- `77725069` fix(storage): preserve saved JSON when snapshot allocation fails
  - conf: test/CMakeLists.txt | test/CMakeLists.txt | C_CONFLICT_WORTH | Only the real change is 5 lines: early `if (doc.overflowed()) return false;` in `PersistableStoreBase::writeDocToFile`. CPYOS's `lib/Serialization/PersistableStore.cpp:10` lacks it, so an overflowed document silently writes a truncated temp file (CPYOS already rejects short writes, but only *after* serializing). Take the guard; drop the CMake/test hunk.
- `c54ee33f` fix(storage): reject partial analytics JSON writes
  - conf: lib/Serialization/PersistableStore.h, test/persistable_store/check_write.py (deleted in CPYOS) | as listed | C_CONFLICT_WORTH | Split: (a) `PersistableStore.h` change is comment-only **and CPYOS already has the stronger check** (`written != measureJson(doc)` at `PersistableStore.cpp:38` plus a `.bak` two-rename scheme) — skip; (b) `src/JsonSettingsIO.cpp` still has only `if (written == 0)` (`:57`) → **port ` |  | written != measureJson(doc)`** there. Net: 1-line change plus message-text update.
- `d663dcc6` feat: Show every sleep image once in randomized order before repeats (#3841)
  - conf: src/CrossPointState.{cpp,h} | 2 | C_CONFLICT_WORTH | CPYOS still has the 16-entry recents ring (`SLEEP_RECENT_COUNT`/`recentSleepImages`/`isRecentSleep`), which cannot guarantee "every wallpaper once" once a card holds >16 images; upstream replaces it with an LCG (Hull-Dobell, cycle-walking) + 5-entry lookback. Value is cosmetic but real for large AirPage/custom-image sets. Cost: `state.json` schema change (needs a `fromJson` migration from the ring fields — CPYOS also has a legacy `loadFromBinaryFile`) and a hand merge into CPYOS's rewritten `SleepActivity`. Do after 60d3c8bb, not before.
- `f084a4dd` fix(covers): reject metadata offsets and repair empty XTC thumbs
  - conf: src/activities/home/HomeActivity.cpp, test/book_cover_loader/BookCoverLoaderTest.cpp (Bitmap.cpp applied clean) | subset of above | C_CONFLICT_WORTH | **Salvage only `lib/GfxRenderer/Bitmap.cpp`** (`paletteOffset` / `bfOffBits` bounds check before reading the palette): CPYOS `Bitmap::parseHeaders()` at `lib/GfxRenderer/Bitmap.cpp:225` has no such validation, so a bogus `bfOffBits` reads palette bytes from the wrong offset. The "repair empty XTC thumbs" half is already in CPYOS (`BookCoverLoader::ensureCachedCover(..., emptyIsTerminal=false)` for non-EPUB); `HomeActivity::loadGridCover` does not exist in CPYOS. Resolution: take the Bitmap hunk + port the malformed-thumbnail test into `test/book_cover_loader`.


## Bucket 3 — conflicts / not worth merging (D_SKIP, 88)

By area: renderer-epub=5, ui-a=14, ui-b=16, fonts-i18n=13, network=10, build-docs=13, storage-misc-a=10, misc-b-sdk=7

### renderer-epub (5)

- `0305aaa6` feat: Improve epub rendering
  - CONFLICT(27) | Page.{cpp,h}, TextBlock.{cpp,h}, ParsedText.{cpp,h}, CssParser.{cpp,h}, CssStyle.h, ChapterHtmlSlimParser.{cpp,h}, Section.cpp, GfxRenderer.{cpp,h}, SdCardFont*, FontCacheManager, Utf8.h, EpubReaderActivity, ClipSelection*, test/* | **D_SKIP** | The 2.0.0 renderer rewrite: per-word font ids + `hasWordFonts` serialization, `TAG_PageDropCap`(4)/`TAG_PageBorderBox`(5), `drawScaledCodepoint` drop caps, table column pre-scan, CSS source-order cascade, `white-space`, widow/orphan control, section v56–v59. CPYOS's `Page.h` has only tags 1–3 and no per-word fonts — i.e. **none** of it is present, but adopting it rewrites the layout core, collides with CPYOS's v86/87 Section format, 16-level AA text path, vertical text, PageBottomAlign, and ruby geometry. Structural; keep excluded (AGENTS §40.4).
- `2c09b8ce` refactor: remove unused image existence helper
  - CONFLICT(1) | ImageBlock.cpp | **D_SKIP** | Pure dead-code removal; `ImageBlock::imageExists()` (`ImageBlock.{h,cpp}`) has zero callers in CPYOS. Nothing to gain from a merge; delete by hand if desired.
- `327e7219` fix: recognize AES-256-CBC manifests
  - CLEAN | — | **D_SKIP** | Explicitly excluded (AGENTS §40.2). CPYOS's SDK `freeink-sdk/libs/book/ContentProtection/src/ProtectedBook.cpp:295` matches only `aes128-cbc`; with this probe change a book would be flagged "protected" and then fail to decrypt. Only take together with an SDK bump.
- `cb2a4c78` fix: correct hyphenation with dropcaps
  - CONFLICT(4) | ParsedText.{cpp,h}, Section.cpp, docs, test | **D_SKIP** | Primary fix needs a bounded `maxLines` ("leave words beyond the emitted lines unsplit") that CPYOS does not have (no `maxLines` anywhere in `ParsedText`; no dropcaps/bounded preview layout). Residual nugget: upstream also starts `lineWidth` at `calculateRubyExtraStartOffset(...)` in `computeHyphenatedLineBreaks` — CPYOS `ParsedText.cpp` hyphenated loop still starts at 0, so ruby overhang is unaccounted. Fold that one line into the ruby port rather than merging this commit.
- `fdf5f76c` fix: add non-CJK GUI fallback font support
  - CONFLICT(2) | GfxRenderer.cpp, SdCardFontSystem.cpp | **D_SKIP** | Diff is 90% comment renames + deleting `utf8IsCjkCodepoint` (still unused in CPYOS: `lib/Utf8/Utf8.h:80`, zero callers) + widening `kFallbackProbes` from 10 to 14 scripts. CPYOS's block is `#if !FREEINK_DEVICE_READPICO` (`SdCardFontSystem.cpp:365`) so it never runs on the target, and CPYOS resolves faces via system font pack + supplemental fonts (AGENTS §43/§45). Optional manual: delete the dead helper, extend the probe list.

### ui-a (14)

- `01f09e0e` fix: Respect content area insets and corner radius in UI
  - CONFLICT | freeink-sdk, LibraryListActivity, WifiSelection, CoverGridHomeUi, BaseTheme.{cpp,h} | D_SKIP | Built on `UITheme::getContentArea()` / `BaseTheme::headerBand()` + `BoardConfig::ACTIVE.cornerRadius` bezel insets; neither API exists in CPYOS (`getContentArea` only as a Sokoban local helper) and the touched list/home surfaces were rewritten for the CPY theme. Would drag a freeink-sdk gitlink bump.
- `0a907772` fix: load the X3 battery capacity into its BQ27220 fuel gauge
  - CLEAN | — | D_SKIP | X3-only (CPYOS builds `x4pro`/`x4c`/`readpico`/`eego_a4`…, no X3 env) and requires `BatteryMonitor::loadDesignCapacity()`, which does not exist in CPYOS's pinned/patched freeink-sdk (`grep` finds BQ27220 registers only). Applying builds nothing and would need a gitlink bump (excluded).
- `261726f6` feat: BOOX Picco device
  - CONFLICT | 15 code files + partitions_picco.csv, platformio | D_SKIP | Hardware port (Picco env, `SlideSwitch.h`, HalPowerManager external-power path). CPYOS ships no Picco target and its power/PMU code is Read-Pico specific; importing risks the §34 power fixes.
- `31480212` style: clang-format 21 cleanups in tap-zone settings activity
  - CONFLICT | TapZoneSettingsActivity.cpp | D_SKIP | Formatting of upstream code CPYOS does not share.
- `35dccb87` fix(settings): split the tap-zone batch and reserve its capacity
  - CONFLICT | SettingsList.h | D_SKIP | Upstream `getBaseSettingsList` batch/reserve bookkeeping; CPYOS's table is structured differently (93 entries, single insert blocks).
- `43694c90` fix: show popup for kosync server type selector
  - CONFLICT | freeink-sdk, KOReaderSettingsActivity.{cpp,h} | D_SKIP | CPYOS removed the KOReader *server-type* feature entirely (`git grep KOReaderServerType` → 0 hits); its KOReaderSettingsActivity row 3 is document matching. Nothing to fix.
- `87fc4ab7` feat: Settings grouping
  - CONFLICT | SettingsActivity.{cpp,h}, SettingsList.h, ActivityManager, listIcons | D_SKIP | Introduces `SettingsSection` + `SettingsListActivity` (sub-activity per group). CPYOS has its own grouping: header rows inside `SettingsActivity::rowItems_` with `-1` actionValue (`settingsIndexAtRow`, `rowSettingIndex_`), plus `SettingsSearchActivity`. Superseded; adopting would re-break CPYOS's row mapping (§26).
- `8fc2b661` Check for gyroscope presence in tilt sensor init
  - CONFLICT | HalTiltSensor.cpp, SettingsList.h | D_SKIP | Upstream gates tilt on `hasGyroscope()`. CPYOS deliberately derives tilt from the accelerometer (gravity derivative) for Read Pico's SC7A20H (§21) — this commit would silently delete the feature.
- `944b741e` style(home): clang-format the menu hit-test rect initializer
  - CONFLICT | home/HomeActivity.cpp | D_SKIP | Pure re-format of 02d2a628's hunk; meaningless without that commit and CPYOS is not clang-format-clean anyway (§4.3 note).
- `c201af43` feat(dict): multi-token lookup using clipping
  - CONFLICT | ClipSelectionActivity, DictionaryWordSelectActivity, EpubReaderActivity, docs | D_SKIP | Depends on the excluded upstream clippings subsystem (`d7fcc7ce`); CPYOS uses `AnnotationStore` and its own selection UI. Porting would re-introduce a second selection model. Revisit only as a CPYOS-native feature.
- `c7996b9f` fix(settings): reserve capacity for the page bottom-align entry
  - CONFLICT | SettingsList.h | D_SKIP | Adjusts upstream's `entryCount` linear table (87→88). CPYOS's table uses `entryCount = 93` with different composition and bottom-align is not a list entry. Meaningless here.
- `d52fc820` refactor: remove unused rectangle touch helper
  - CONFLICT | MappedInputManager.{cpp,h}, docs | D_SKIP | CPYOS still uses `wasTapInRect` in ~20 places (games: 2048, avatar, chess, gomoku, minesweeper, …). Removing it is upstream-only cleanup.
- `e529cadb` feat: global plugin system toggle + plugin hub bootstrap
  - CONFLICT | 12 code files incl. PluginCatalogActivity, SettingsActivity, CrossPointWebServer, PluginLocations | D_SKIP | New feature, not a fix: adds `pluginsEnabled`/`pluginHubPromptHidden`, `scanPluginsOnDisk()`, install quarantine, web-server endpoints, `PluginHubInstallActivity`. CPYOS's plugin subsystem already diverged (`classifyDeviceManifest`, `isNewerVersion`, own catalog; no hub). Defer as a product decision — depends on the upstream plugin hub + web endpoints.
- `ec32b959` fix(inx): share actions and consolidate page selection
  - CONFLICT | 28 files (AppsMenu, InxRecent, RecentBooks, About, ReadingStats, OptionPopup, UiContentSelection…) | D_SKIP | Superseded surface-by-surface: `AppsMenuActivity` is deleted in CPYOS (AppCatalog + My tools drawer), `InxRecentActivity`/`ReadingStatsActivity`/`AboutActivity` are CPYOS rewrites, and CPYOS has no `UiContentSelection` (its list navigation lives in `UiListActivity`). Only docs value (`docs/engineering/inx-theme-compatibility.md`); nothing to merge.

### ui-b (16)

- `0727f9f0` feat: Bluetooth quick toggle (#4187)
  - conflict | USER_GUIDE, GfxRenderer.{cpp,h}, MappedInputManager, ActivityManager, BleSettingsActivity, FrontlightPanelActivity, listIcons | D_SKIP | Requires upstream's `BlePageTurner`/`bleturner` module (not present in CPYOS; CPYOS BLE is `BleInput`/`BleKeyMapping`) and builds on b2b1e15a's icon row. Generic residue worth a look only for `wasLightPanelGesture()` un-gating (top-edge swipe without a frontlight) — CPYOS's MappedInputManager:536 keeps the `Frontlight.present()` gate.
- `3f8345c5` fix: padding in Tollbar menu with roundedraff theme (#4228)
  - conflict | ReaderToolbarUi.cpp | D_SKIP | `sidePadding = 0 → tokens.listSidePadding` for the upstream "roundedslides" theme. CPYOS's toolbar is a full rewrite (left thin-line cursor, its own insets).
- `471243b2` fix: Hide long-press menu setting on Home-key and Picco boards (#4019)
  - CLEAN | — | D_SKIP | Applies textually but is the wrong product call for CPYOS: `confirmLongPressThreshold()` would return 0 on **all** touch boards while CPYOS still shows `STR_LONG_PRESS_MENU` (filtered only for `hasHomeKey()`, SettingsActivity.cpp:472) and uses it (`EpubReaderActivity.cpp:2554,3203`). Apply only together with 5407ffcd, i.e. never.
- `4ef42ee1` fix: skip debug-only diagnostic work in release builds (#4250)
  - conflict | LibraryListActivity.cpp, DictionaryDefinitionActivity.cpp | D_SKIP | Guards two `LOG_DBG` blocks that compute `HalMemory::getInternalHeap()`. CPYOS removed those diagnostics (no `getInternalHeap` calls in either file).
- `5407ffcd` Consolidate touch board settings filtering logic
  - conflict | freeink-sdk, SettingsList.h, SettingsActivity.cpp | D_SKIP | CPYOS keeps long-press menu on readpico deliberately (capacitive keys + touch, no home key). Its own touch filters live in `SettingsList.h:809`. Not a bug for CPYOS.
- `6b863e48` fix(touch): hide dict button when no dict is available (#4245)
  - conflict | ClipSelectionActivity.{cpp,h}, DictionaryWordSelectActivity.cpp, BaseTheme.{cpp,h}, test | D_SKIP | The action row belongs to upstream clippings (`ClipSelectionActivity`, `drawSelectionActions`), which CPYOS does not have. CPYOS's equivalent is `InlineConfirm`/annotation flow with its own labels.
- `88ea9bcf` fix: keep 1.1/1.2 line spacing for .cpfont fonts (#4013)
  - conflict | reader | D_SKIP | Deliberately excluded (CPYOS chose 1.3/1.6 spacing, AGENTS §40.2).
- `8b4c0d6e` fix: unify INX header status placement
  - conflict | Activity.cpp, MainTab.h, InxRecentActivity, UITheme, UiHighDpiProfile, BaseTheme.{cpp,h}, InxTheme.{cpp,h}, LyraTheme.h, tests | D_SKIP | INX-theme chrome redesign. CPYOS retired INX as the active theme (OptionPopup.h: "INX is retired") and draws its own headers in CpyTheme; adopting would fight the CPY chrome.
- `8f4d035c` fix: Move Picco switch to hardware setting not software switch (#4234)
  - conflict | HalFrontlight, SettingsList.{cpp,h}, menus, FrontlightPanel, main.cpp, SlideSwitch.h | D_SKIP | Picco board hardware; CPYOS does not target it and has no slide switch.
- `ad24aea8` fix: delete clipping popup blinks (#4093)
  - conflict (file deleted in CPYOS) | EpubReaderClippingListActivity.cpp | D_SKIP | CPYOS does not adopt upstream clippings; it has `AnnotationStore`/`InlineConfirm`. No such activity.
- `b2b1e15a` fix: use icons instead of text labels in FrontlightPanel (#4176)
  - conflict | FrontlightPanelActivity.{cpp,h}, USER_GUIDE | D_SKIP | CPYOS's control center is a deliberate labelled-tile sheet ("Pure 1-bit: no dithered fills"), reworked in its own idiom. Visual preference, not a defect.
- `b823d54b` fix(settings): let the page-turn DynamicEnum own its stored value
  - conflict | 6 files | D_SKIP | Keyed on upstream `touchControlsVersion`, which CPYOS never writes. CPYOS migrates via `pageTurnGesture/previousPageGesture` + `tapZoneMode` (CrossPointSettings.cpp:569-610) and handles INVERTED_TAP in `detectTouchPageTurn`, so the underlying defect does not exist here.
- `cac12269` fix: defines incomplete-type owner lifecycles out of line (#2643)
  - conflict | ActivityManager.{cpp,h}, HomeActivity.{cpp,h} | D_SKIP | Compile-hygiene (out-of-line ctor/dtor) + `onSelectBook`→`onRecentBookSelected` rename. CPYOS still has the inline ctor (ActivityManager.h) and builds on all its envs; no behaviour change.
- `e41982a4` fix(settings): keep mixed gesture pairs intact during touch migration
  - conflict | CrossPointSettings.cpp | D_SKIP | Same scheme mismatch as b823d54b. CPYOS's migration only expands when **both** gesture keys are absent, so pair-era saves are already left intact; no INVERTED_TAP side swap is needed because `detectTouchPageTurn` reads the gesture directly.
- `e9635421` fix(settings): reserve capacity for the new tap-zone row
  - conflict | src/SettingsList.h | D_SKIP | Upstream `entryCount 88→89`. CPYOS uses its own count (`SettingsList.h:238` = 93, asserted at :721) and already shrinks the table to exact size. Nothing to take, but keep CPYOS's own invariant in mind when adding base rows.
- `fb2a8a8f` fix: Adds support for new Picco & Sticky displays (#4146)
  - conflict | freeink-sdk, ActivityManager, SleepActivity, LibraryList, menus, UITheme, BaseTheme | D_SKIP | Upstream-only hardware plus a freeink-sdk gitlink bump. CPYOS ships readpico/C3/X3/X4 and its own SDK patch series; nothing here is a CPYOS bug.

### fonts-i18n (13)

- `072b6fc7` feat: line spacing as a percentage of font size (#4174)
  - conflict 19 files / 31 hunks | `ReaderRenderSpec.h`, `Section.cpp`, `ChapterHtmlSlimParser.{cpp,h}`, `GfxRenderer.cpp`, `CrossPointSettings.*`, `SettingsList.*`, `EpubReaderActivity.cpp`, `main.cpp`, 4 yaml, 2 tests | **D_SKIP** | Redesigns line spacing into 7 percentage steps (100–175 %) with line height derived from font size rather than `advanceY`, and drops the per-family multipliers. CPYOS deliberately keeps 4 levels with family-specific factors (0.90/0.95/1.1/1.2 for built-ins, 0.95/1.0/1.3/1.6 for SD/vector fonts) — §40.2 already rejected upstream's retune. Would rebuild every paginated section. Only revisit if users ask for continuous spacing control; the "line height from font size" idea is the only transferable piece.
- `1899de8f` fonts: include Cyrillic in the built-in subsets
  - clean | — | **D_SKIP** | Script-only (`build-cn-builtin-fonts.sh`, 2 lines) with **no runtime effect** until the CJK headers are regenerated — and per AGENTS §27 regenerating them reverts CPYOS's 6879-char 12 pt / 803-char 14–16 pt layout. Cyrillic is already in the system pack (§57.1) and the world-scripts extra.
- `2648933a` perf: reuse loaded UI font for CSS variants (#4117)
  - conflict 2 files / 3 hunks | `SdCardFontManager.cpp`, `src/SdCardFontSystem.cpp` | **D_SKIP** | Affects `SdCardFontSystem::setupUiFallbacks()`, which **returns early when the system font pack is ready** (AGENTS §43.4) — dead on readpico — and C3 has no PSRAM so it never loads SD families. Zero benefit for shipped targets.
- `3f0f4fad` fix(fonts): ship Cyrillic and refresh pagination with font data
  - conflict 19 files / 31 hunks | 6 `notosans_cjk_*.h` (≈110 k lines), `build-cn-builtin-fonts.sh`, `build-font-ids.py` + `publish-cn-builtin-fonts.py` (modify/delete), `fontIds.h`, `main.cpp`, `CrossPointSettings.cpp`, `UiHighDpiProfile.h`, 2 docs, 3 tests | **D_SKIP** | Regenerates the built-in CJK faces (Cyrillic + №) **and** replaces the fixed `UiHighDpiProfile::reader12FontId` with a content-hashed ID so pagination rebuilds when font data changes. CPYOS already invalidates pagination by `SECTION_FILE_VERSION` 86/87 (§43.2) and its reader faces are pack IDs; and regenerating the headers reverts CPYOS's own character split (§27). Nothing left worth the risk.
- `40b313a3` feat: complete Azerbaijani translation and add Ə/ə glyph support (#3842)
  - clean | — | **D_SKIP** | 38 of 40 files are regenerated `builtinFonts/notosans_*` + `notoserif_*` headers for Ə/ə; CPYOS **deleted `azerbaijani.yaml`** and its UI/reader faces come from the vector system pack (only `notosans_8` on C3, `notosans_12` offline). Clean apply is misleading: it would change compiled font bytes (and shift content-hashed IDs) with no visible gain.
- `7242d9ae` build(fonts): generate existing font IDs with Python
  - conflict 1 file / 2 hunks | `lib/EpdFont/scripts/build-font-ids.sh` | **D_SKIP** | Tooling for upstream's ID set (serif/sans/ubuntu/chess). CPYOS's `fontIds.h` is a different model (pack IDs, `CJK_UI_*`, `CJK_READER_SIZE_FONT_ID`, `NOTOSERIF_TITLE_FONT_ID`), so the Python generator would emit a header CPYOS cannot use. Take only if we ever rewrite the generator ourselves.
- `965722cf` feat: offer Libron in Manage Fonts downloads (#3971)
  - clean | — | **D_SKIP** | 11 lines in `lib/EpdFont/scripts/sd-fonts.yaml`. It is a catalog entry for the legacy generated `fonts.json`; CPYOS's user downloads are FEXT1 packs (`font-extras/packs.json` → `manifest-v1.txt`). Harmless to take, but it buys nothing until a manifest is regenerated and republished.
- `98da1782` fix(font): use the compacted style API in the advance-table scan (#4134)
  - conflict 1 file / 2 hunks | `lib/EpdFont/SdCardFont.cpp` | **D_SKIP** | It repairs the exact bug that taking `83df537d` without `b912e000` creates (`si` is a style id, not a slot → reads the wrong style or past the array). CPYOS's `si`-indexed loop is correct for its own `styles_[si]` array. **Never cherry-pick this ahead of `b912e000`.**
- `c6a2e311` fix: build develop again after #3948 and #4114 (#4132)
  - conflict 1 file / 1 hunk | `test/sd_card_font/SdCardFontTest.cpp` | **D_SKIP** | Test-only follow-up to `83df537d`/`b912e000`; contains the regression test `AdvancesBuildForAStyleStoredInAnotherSlot` for the `98da1782` bug. Nothing to port unless those two land.
- `ce781eb8` fix(ui): prewarm visible CJK glyphs before list painting
  - conflict 3 files / 3 hunks | `freeink-sdk` (submodule), `src/activities/UiListActivity.cpp`, `test/font_decompressor/FontDecompressorTest.cpp` (modify/delete) | **D_SKIP** | Adds `ScanMode::UiCjk` + a scan-before-paint pass in `UiListActivity::render`. Blocked structurally: it bumps the `freeink-sdk` gitlink, which CPYOS pins at patched `96de1be` (AGENTS §15), and CPYOS's list render was rewritten (16-level canvas, band updates, `paintOverlayBeforePresent`). Idea only; would need reimplementation against `CROSSPOINT_SYSTEM_FONT_PACK`.
- `cf50ce73` fix: use Ubuntu for the small UI font (#4026)
  - conflict 4 files / ~4 hunks | `builtinFonts/all.h`, `notosans_8_regular.h` (modify/delete), `src/fontIds.h`, `src/main.cpp` | **D_SKIP** | CPYOS already ships `ubuntu_10/12` and uses them for `UI_10`/`UI_12`; on high-DPI `SMALL_FONT_ID` is `notosans_12`. The 8 pt face only survives on C3 as an emergency, where the CJK 8 pt subset is the real fallback — swapping it has no user-visible effect and would churn `fontIds.h`.
- `d7fcc7ce` feat: add ability to clip/highlight text for EPUBs (#3589)
  - conflict 29 files / many | `Epub/Section.cpp`, `TextBlock.*`, `CrossPointSettings.*`, `SettingsList.h`, `EpubReaderActivity*`, `OptionPopup.h`, `BaseTheme.*`, `english.yaml`, `german.yaml`, … | **D_SKIP** | Superseded: CPYOS ships `lib/Epub/Epub/AnnotationStore.{h,cpp}` (highlight/annotate/export) with its own UI (toast + inline confirm). Upstream's version rests on per-word source ranges, which was explicitly rejected for CPYOS (§40.2). Nothing to port.
- `fc2de152` feat: move NotoSerif to optional SD font (#4022)
  - conflict 28 files (18× modify/delete) | `builtinFonts/all.h` + 18 `notoserif_*.h`, `build-font-ids.sh`, `sd-fonts.yaml`, `CrossPointSettings.cpp`, `ReaderFontSizes.h`, `SettingsList.h`, `EpubReaderActivity.cpp`, `TextSettingsActivity.cpp`, `main.cpp`, 3 yaml | **D_SKIP** | The goal is already true in CPYOS: `builtinFonts/all.h` includes only `notosans_8`, `notosans_cjk_8/10/12`, `ubuntu_10/12`, `chinese_chess_16`; the 18 `notoserif_*` headers are **dead files** (grep finds no includer) and `NOTOSERIF_*` ids are aliases of `notosans_12`. Deleting them is optional repo/build-time hygiene — 0 flash saved.

### network (10)

- `057e16a4` perf: minify web UI, load EPUB optimizer on demand (23 KB)
  - conflict (7) | .agents/rules/generated-files-cache.md, scripts/build_html.py, CrossPointWebServer.cpp, en/FilesPage.html, en/SettingsPage.html, shared/PluginHost.js.inc, js/*.js (new), scripts/vendor/r{jsmin,cssmin}.py (new) | **D_SKIP** | CPYOS already resolves `<!--#include-->` and minifies in `scripts/web_assets.py` (single compression shared by embedded headers and the CWP1 pack). The on-demand optimizer split saves first-paint latency, not pack bytes (pack headroom is only 4.5 KB anyway). Revisit only if the pack must grow.
- `55a77c2e` fix: web UI dropdown chevron inset
  - conflict (1, modify/delete) | src/network/html/assets/web.css | **D_SKIP** | CSS-only; target file does not exist in CPYOS (styles are inline per page).
- `5789260c` style: refresh device web UI
  - conflict (6) | scripts/build_html.py, src/network/CrossPointWebServer.cpp, src/network/html/{en/FilesPage,en/FontsPage,en/SettingsPage,shared/Footer}.html(.inc), assets/web.css (new) | **D_SKIP** | CPYOS replaced this UI (localized `en/`+`zh-CN/`, CWP1 pack, own restyle). **Extract only one hunk**: `handleGetSettings` should emit `"type":"toggle"` for 2-state OFF/ON enums (CPYOS CrossPointWebServer.cpp:1600 always sends `"enum"`, while en/SettingsPage.html:343 already renders `type==='toggle'`). ~10 lines, immediate UX win.
- `58d9c776` refactor: dialogs, consistent prompts, polish js
  - conflict (5) | src/network/html/{en/FilesPage,en/FontsPage,en/SettingsPage}.html, shared/Footer.html.inc (new), assets/web.css (new) | **D_SKIP** | Pure client-side refactor for upstream's single-page UI. CPYOS FilesPage already has per-modal cleanup + `modal-overlay` handlers; upstream's `Footer.html.inc`/`web.css` split does not exist in CPYOS.
- `861683cc` feat(plugin): filter catalog lists by capabilities
  - conflict (4) | docs/sd-plugins.md, PluginCatalogActivity.{cpp,h}, FirmwareIdentity.h | **D_SKIP** | Needs `firmwareidentity::PLUGIN_CAPABILITIES` + `requires` mapping (all from `e529cadb`). Nothing to attach it to in CPYOS.
- `919ec426` docs(weread): correct reading-duration claims
  - clean | — | **D_SKIP** | docs only (2 files).
- `a722472c` Reset publication object using uniform initialization
  - conflict (1) | src/activities/browser/OpdsBookBrowserActivity.cpp | **D_SKIP** | Target `OpdsPublication`/`openPublicationDetail` does not exist in CPYOS (no publication-detail path). Depends on the unadopted part of f2120a3f.
- `c9f64d0d` feat: move plugin settings to a Plugins web page, consolidate styles
  - conflict (12) | USER_GUIDE.md, docs/webserver*.md, az.yaml (absent), CrossPointWebServer.{cpp,h}, html/{HomePage,RunnerPage,en/FilesPage,en/FontsPage,en/SettingsPage}, html/assets/web.css, html/js/plugin-host.js, html/shared/Header.html.inc | **D_SKIP** | Pure page relocation + CSS consolidation for upstream's layout. CPYOS mounts plugins inline (`#plugin-container` in SettingsPage) and has no external `web.css`/`Header.html.inc`.
- `d5d0bea1` feat: headers and mapping for plugin list index
  - conflict (5) | docs/*, PluginCatalogActivity.{cpp,h}, src/util/FirmwareIdentity.h (absent in CPYOS) | **D_SKIP** | Same blocker as above. CPYOS has **no** `FirmwareIdentity.h`, `PLUGIN_API_VERSION` or `PLUGIN_CAPABILITIES` anywhere.
- `e93f8d1f` feat: combine fixed and dynamic plugin lists
  - conflict (3) | docs/sd-plugins.md, PluginCatalogActivity.{cpp,h} | **D_SKIP** | Requires `manifest.browseListsUrl` / `loadBrowseListIndex()` / `listIndexLoaded` — **absent from CPYOS** (introduced by `e529cadb`, which is outside this slice and not in CPYOS). Revisit only if the plugin-system commit is adopted.

### build-docs (13)

- `35072257` chore: update README to release 2.0.0 (#4084)
  - conflict | README.md, platformio.ini | **D_SKIP** | Sets `version = 2.0.0` and README badges. Conflicts with CPYOS branding + `0.x-cpyos` version scheme.
- `414b3797` test: model render lock lifetime in OPDS harness
  - modify/delete conflict (file absent) | scripts/tests/test_opds_safety.py | **D_SKIP** | CPYOS has no `scripts/tests/test_opds_safety.py`, and CPYOS's real `RenderLock` now has a recursion guard + destructor (§49.5), so upstream's stub model is stale anyway.
- `59912ea9` fix(ci): link available PR firmware artifacts
  - clean | — | **D_SKIP** | Fixes upstream `.github/workflows/pr-firmware-links.yml`, which CPYOS does not have (only `cpyos-release.yml` + `pages.yml`). Nothing to port.
- `5a51bf32` fix: restore simulator HAL compatibility
  - conflict | platformio.ini, test/CMakeLists.txt | **D_SKIP** | Only bumps the `crosspoint-simulator` git ref (`ad24d40`→`5a199a0`). CPYOS pins `58b6694d` (`platformio.ini:373`) and has no `test/CMakeLists.txt` simulator ref. **Dependency risk instead:** later slices (webbrowser/AA/etc.) may assume a newer simulator ref; check CPYOS's pin covers the HAL API used by the merge.
- `6ec32892` docs: prepare CrossMux 1.6.5 release notes
  - clean | — | **D_SKIP** | Adds `.github/release-notes/1.6.5.md`, upstream release process. CPYOS keeps `docs/releases/<v>.md`.
- `7dc84ed6` docs: record Waveshare 3.97 hardware validation and TG28 PMIC
  - clean | — | **D_SKIP** | `docs/engineering/waveshare-epaper-397.md` (board CPYOS does not build/ship). Nothing portable; PMIC notes are Waveshare-specific.
- `91ed3ef8` chore: Fix README.md missing bolds
  - conflict | README.md | **D_SKIP** | Markdown bolding in upstream README.
- `997b4331` fix(release): restrict Stable to four approved targets
  - 6 conflicts | README(.zh-CN), docs/engineering/{firmware-release,read-pico}.md, scripts/nightly_targets.py, scripts/tests/test_nightly_release.py | **D_SKIP** | Adds `repair_stable_165.py` + a one-off workflow whose only purpose was the upstream 1.6.5 release, and trims `nightly_targets.TARGETS` to upstream-approved boards. CPYOS publishes only its own targets; none of those files exist here. Superseded by 843c1806 anyway.
- `a66d76f4` refactor: unify firmware builds and release artifacts
  - 10 conflicts | README(.zh-CN), docs/engineering/{device-variants,build-system,firmware-release,...}, platformio.ini, scripts/{git_branch,nightly_targets,package_nightly_target,verify_nightly_release}.py, .github/workflows/{ci,nightly,hardware-ci}.yml | **D_SKIP** | Introduces `[mode_dev]/[mode_stable]/[mode_nightly]` and deletes the `-DCROSSPOINT_VERSION` build flags in favour of `git_branch.py`; deletes sticky/x4pro/x4c RC envs CPYOS never had and rewrites `nightly_targets.py`/`package_nightly_target.py`/`build_nightly_index.py` — all superseded by CPYOS's `build_cpyos_release.py` + `package_nightly_target.py` + `verify_release_artifacts.py`. Useful *ideas* only: `scripts/tests/test_build_modes.py` (asserts version/log-level per mode — CPYOS already asserts `project_name`/`version` in `verify_release_artifacts.py`). Do not cherry-pick.
- `a6db8f15` Change de-link project link to Silkscreen
  - conflict | README.md | **D_SKIP** | Branding/link in upstream README.
- `c17b80ab` docs: list Paper Mono in frontlight guide
  - clean | — | **D_SKIP** | `USER_GUIDE.md` (upstream guide). CPYOS ships its own user guide (`docs/user-guide`, generated EPUB) and has no Paper Mono target.
- `c8175b1f` test: fix and bound Read Pico simulator wake check
  - conflict | docs/engineering/upstream-sync-2026-11-03.md, platformio.ini | **D_SKIP** | `timeout-minutes: 5` on an upstream `hardware-ci.yml` job plus another simulator ref bump (`e112668`→`e5e716a`). CPYOS has neither that workflow nor that pin.
- `e7b47ee5` chore(workflows): run check on push to develop
  - conflict | .github/workflows/ci.yml | **D_SKIP** | CPYOS has no upstream `ci.yml`; it has its own workflow set (§31.4).

### storage-misc-a (10)

- `12e441f7` fix: drop the failed-cover empty sentinel
  - CONFLICT | BookCoverLoader.cpp, tests, stubs/Bitmap.h | **D_SKIP** (superseded) | Intermediate state (`sizeof(BmpHeader)+pixels`); final form is `3db7a96b`. Adopt the final file state instead.
- `186d9407` fix: stop retrying a failed cover conversion
  - CONFLICT | BookCoverLoader.cpp, tests/stubs | **D_SKIP** (superseded) | Its 0-byte "terminal sentinel" design was reverted by `12e441f7`. Do not take in isolation.
- `3607ea7b` chore: further refine agents guidelines
  - CONFLICT | AGENTS.md, .agents/** | **D_SKIP** | Docs/agent guidance only, same resolution as 6d2cd3a1.
- `65e3f9d0` feat: Add custom HTTP headers for OPDS and KOReader Sync
  - CONFLICT | AsciiText.h, CssParser.cpp, HttpHeader.*, KOReaderCredentialStore.*, OpdsServerStore.*, SettingsList.h, Opds/KOReader settings, clippings/ClippingSync.cpp, CrossPointWebServer.cpp, HttpDownloader.* | **D_SKIP** (defer) | Needs a new `lib/HttpHeader/` (absent in CPYOS) and touches `src/clippings/` (deliberately not adopted). Settings JSON would gain an `opds.json` `headers` key (migration note if revived). Defer; re-derive as a standalone feature.
- `6d2cd3a1` chore: refactor agents guidelines
  - CONFLICT | AGENTS.md, .agents/**, docs/contributing/** | **D_SKIP** | Replaces upstream `AGENTS.md` with `.agents/rules/*`. CPYOS keeps its own `AGENTS.md`; resolve with `--ours` and take only `.agents/**`.
- `809c1dab` feat(hal): map LilyGO T5S3 expander button to down input
  - CONFLICT | lib/hal/HalGPIO.cpp, EpubReaderActivity.cpp | **D_SKIP** | No LilyGO env in CPYOS `platformio.ini`; HalGPIO conflict is CPYOS's readpico/Metalio expansion. Take only if the board is ever added (hunks are inside `#if FREEINK_DEVICE_LILYGO_T5S3`).
- `b0aadf2a` docs: shorten JPEG scale and cover payload comments
  - CONFLICT | lib/JpegToBmpConverter/JpegToBmpConverter.cpp, src/util/BookCoverLoader.cpp | **D_SKIP** (doc) | Comment-only; the code it annotates is `bb4f8785`, which CPYOS lacks. Take `bb4f8785` instead.
- `bb855ef9` docs: clarify scope for reusable downstream primitives
  - CONFLICT | SCOPE.md | **D_SKIP** (doc) | 14 lines of policy text in upstream's `SCOPE.md`; CPYOS has its own scope doc.
- `d1f5d1ec` chore: yet more refinment of agents guidelines
  - CONFLICT | AGENTS.md, .agents/** | **D_SKIP** | Same.
- `fecee610` feat: Bluetooth page turner for X3/X4
  - CONFLICT | 16 files incl. freeink-sdk, platformio.ini, ActivityManager, EpubReader* | **D_SKIP** | Deliberately excluded (CPYOS already has `BleInput` + `BleKeyMapping`; two radio owners). Merge only if `--ours`-ing everything, which then makes the commit an empty shell — resolve as "keep CPYOS".

### misc-b-sdk (7)

- `052d481a` test(tapzone): pin DISABLED to a stored 4 that stops the tap turn
  - conf: test/touch_page_turn/test_page_turn_settings.py (deleted in CPYOS) | that file | D_SKIP | `test/touch_page_turn/` does not exist in CPYOS (whole directory absent). Recreating it is a separate decision; the setting itself is covered by CPYOS's `src/TapZones.h` + `TapZoneSettingsActivity`.
- `136c2655` chore: add license notices around the codebase (#4064)
  - conf: .gitattributes, .github/workflows/release.yml (+release_candidate) deleted, src/network/html/js/epub-optimizer.js deleted | as listed | D_SKIP | 36 files / ~5.9k lines of `licenses/*.txt` + `borrowed-code.txt` + `build_release_license.py` + per-file notices. CPYOS already has `LICENSES/` (AGPL-3.0/Apache-2.0/MIT/OFL/Crosspoint-Simulator/Metalio), `THIRD_PARTY_NOTICES.md`, `LICENSE`, and its own `scripts/tests/test_license_scope.py`; CPYOS's CI workflows are `cpyos-release.yml`/`pages.yml` and `epub-optimizer.js` no longer exists. Merging would fight the fork's licence scheme. Only lift individual entries from `licenses/borrowed-code.txt` if a CPYOS-owned file is missing provenance.
- `18d611f2` test: fix two host-test failures on macOS and under parallel ctest (#4130)
  - conf: DictionaryHtmlPagesTest.cpp + test_recursive_dictionary.py (both absent in CPYOS) | as listed | D_SKIP | Test-only, and both files are absent in CPYOS (recursive-dictionary tests were not taken with 6376516c).
- `5eb498b0` test(settings): add mixed gesture pair migration regressions
  - conf: test/touch_page_turn/… (deleted) | that file | D_SKIP | Same missing test directory as 052d481a; test-only.
- `d5b6c74d` fix: keep the BLE radio off in a book when no remote is chosen (#4074)
  - conf: 7× lib/BlePageTurner/* (modify/delete) | whole lib/BlePageTurner | D_SKIP | `lib/BlePageTurner` does not exist in CPYOS (deliberately removed upstream BLE module, §CPYOS context; CPYOS ships its own `src/BleInput`/`BleKeyMapping`). Re-express only if CPYOS's own BLE policy has the same "no remote chosen ⇒ keep radio off" gap — that is a CPYOS design question, not a port.
- `f0dfe5fc` fix: reduce protected-book and clipping sync memory usage (#4081)
  - conf: freeink-sdk, KOReaderSyncActivity.cpp, src/clippings/ClippingSync.{cpp,h} (deleted) | as listed | D_SKIP | Body of the commit is C3 framebuffer-loan inside `clippingSync::run()`, and CPYOS has no `src/clippings/` (its own `AnnotationStore`; "KOReader clipping 同步未做"). **One-line salvage worth taking**: `lib/Epub/ContentProtection.cpp` — log `book->lastError()` when `openFromScan` fails. CPYOS's protected-book path is critical for JJWXC and this failure currently logs only "cannot open protected content".
- `f3ace194` test: trim redundant cover cache cases
  - conf: test/book_cover_loader/BookCoverLoaderTest.cpp | test/book_cover_loader/BookCoverLoaderTest.cpp | D_SKIP | Test-only churn. CPYOS's loader (10-slot PSRAM thumb cache + `thumbPathFor` memo) already reshaped this test file; nothing behavioural to gain.


## Already equivalent in CPYOS (A_ALREADY, 45) — no content action

By area: renderer-epub=10, ui-a=13, ui-b=6, fonts-i18n=5, network=1, build-docs=4, storage-misc-a=4, misc-b-sdk=2

### renderer-epub (10)

- `089e7ae5` fix: flush pending word before a block opens
  - CONFLICT(4) | docs/file-formats.md, Section.cpp, ChapterHtmlSlimParser.cpp, test | **A_ALREADY** | CPYOS `ChapterHtmlSlimParser.cpp:1456-1462` contains the identical flush in the HEADER_TAGS branch (same comment), plus the `<br>` case at 1474. Absorbed per AGENTS §40.1.
- `1bd323dd` fix: preserve explicit CSS font inheritance
  - CONFLICT(6) | CssParser.{cpp,h}, CssStyle.h, Section.cpp, ChapterHtmlSlimParser.cpp, tests | **A_ALREADY** | CPYOS has `CssFontStyle::Inherit`/`CssFontWeight::Inherit` (`CssStyle.h:51,54`), parses `inherit` (`CssParser.cpp:448,457`) and resolves it against parent state in the parser (`ChapterHtmlSlimParser.cpp:860,869`). Absorbed per AGENTS §40.1.
- `3e2af487` perf: scan CJK break offsets without staging
  - CONFLICT(2) | ParsedText.cpp, test | **A_ALREADY** | CPYOS `ParsedText.cpp:194-230` already has the two-pass `boundaryCount` implementation (no `CodepointBoundary` staging vector). Absorbed per AGENTS §40.1.
- `4617eb90` fix(renderer): synchronize fading-fix refresh policy
  - CLEAN | — | **A_ALREADY** | CPYOS `lib/GfxRenderer/GfxRenderer.h:91` is already `std::atomic<bool> fadingFix;` (and `setFadingFix` at :352).
- `75c1c834` fix: preserve CSS styles under low memory
  - CONFLICT(3) | docs/file-formats.md, Section.cpp, CssParser.cpp | **A_ALREADY** | CPYOS's `CssParser::resolveStyle` has **no** `MIN_FREE_HEAP_FOR_CSS` gate at all (`git grep getFreeHeap` → nothing in `lib/Epub/Epub/css`), i.e. it already behaves like the fixed version. Nothing to remove.
- `8b2ba5e9` fix: corrects spacing in justified EPUB text
  - CONFLICT(5) | ParsedText.cpp, Section.cpp, TokenBoundary.h, docs, test | **A_ALREADY** | CPYOS `lib/Epub/Epub/TokenBoundary.h:20` has `isJustifiableGap(continues, isStretchableSpace)` with the upstream semantics. Absorbed per AGENTS §40.1.
- `9a7e6b35` fix(epub): preserve geometry and cache identity for bottom alignment
  - CONFLICT(9) | ReaderRenderSpec.h, Section.cpp, ChapterHtmlSlimParser.{cpp,h}, EpubReaderActivity.cpp, docs, tests | **A_ALREADY** | CPYOS has `lib/Epub/Epub/PageBottomAlign.h`, `ReaderRenderSpec::verticalBottomAlign`, and includes it in the section geometry key (`Section.cpp:95,162,183,232-242`) — absorbed per AGENTS §40.1.
- `a790b144` fix: take glyph-width rounding into account for space calculations
  - CONFLICT(3) | GfxRenderer.cpp, Section.cpp, docs | **A_ALREADY** | CPYOS `GfxRenderer.cpp:4003-4040` already snaps each glyph step (`fp4::toPixel(prevAdvanceFP)` per word, "Differential rounding") and goes further with `getAdvanceOrLoad` for glyphs missing from the resident advance cache.
- `d5693c32` fix: preserve paragraph continuity across soft flushes
  - CONFLICT(4) | ParsedText.cpp, Section.cpp, ChapterHtmlSlimParser.{cpp,h} | **A_ALREADY** | CPYOS solves the same bug differently: `ParsedText::firstLinePending` (`ParsedText.cpp:751,880`) prevents re-indent after a flush, soft flush (`ChapterHtmlSlimParser.cpp:1897-1917`) adds **no** marginTop/bottom, and the 7-arg `layoutAndExtractLines` callback carries `paragraphFinal` (used by the JJWXC bubble logic, AGENTS §33.2). Upstream's `makePages(includeLastLine=false)`/`wordsExtractedInBlock` shape is not needed.
- `ddadc36f` fix: free the spine href index after the TOC pass
  - CONFLICT(2) | BookMetadataCache.{cpp,h} | **A_ALREADY** | CPYOS uses a non-throwing `std::unique_ptr<SpineHrefIndexEntry[]>` (`BookMetadataCache.h:83`) built in `beginTocPass` (`:139-170`, with OOM fallback + log) and reset in `endTocPass` (`:191`). Stricter than upstream's deque.

### ui-a (13)

- `0855a14f` feat: "Footnotes and links" option in long-press menu
  - CONFLICT | CrossPointSettings.{cpp,h}, SettingsList.h, EpubReaderActivity.cpp | A_ALREADY | CPYOS's `longPressMenuFunction` enum already offers Ignore/Sleep/Page turn/Force refresh/**Footnotes** (`STR_FOOTNOTES`) and the reader handles it (`case …FOOTNOTES`).
- `14b4c939` Show touch toggle state in frontlight panel label
  - CONFLICT | util/FrontlightPanelActivity.cpp | A_ALREADY | CPYOS renders the row as a real toggle (`listItems[3].toggle = true; toggleChecked = touchOn`), which already shows state; upstream's label hack exists only because their tile has no toggle glyph. Do not apply (would duplicate the state).
- `15503ac3` feat: JPEG file viewer
  - CONFLICT | JpegToFramebufferConverter, PixelCache, FsHelpers, ActivityManager, FileBrowser, BmpViewerActivity | A_ALREADY | CPYOS has `FsHelpers::hasImageExtension` feeding `ActivityManager::goToReader` → `ImageViewerActivity`, JPEG decode (`jpegFileToBmpStreamWithSize`) and a sleep-cover path. Upstream's `BmpViewerActivity` rename/split is superseded by CPYOS's `ImageViewerActivity`; any leftover is only crop-mode polish (track under 6d649b66 item, not as a cherry-pick).
- `2cf3857d` refactor: remove unused checkbox icon helper
  - CONFLICT | BaseTheme.{cpp,h} | A_ALREADY | `git grep checkboxIcon` → 0 hits in CPYOS; the helper is already gone (CPYOS toggles are capsules/`setCheckboxRow`).
- `2f6a52c5` feat: eego a4 support
  - CONFLICT | 18 files | A_ALREADY | CPYOS has `[eego_a4_hardware]`, `FREEINK_DEVICE_EEGO_A4` branches in GfxRenderer/MappedInputManager/BootActivity/EpubReaderActivity, `FirmwareBoardTag` "eego_a4", and the two subtle fixes: `HalFrontlight::begin()` calls `manager.begin()` before `present()`, and persistence uses `isSettingAvailableForPersistence()` (CPYOS's cleaner `forPersistence` equivalent) so `STR_RESTORE_LIGHT_ON_WAKE` is not dropped at load. Nothing to port.
- `34327c7e` fix(tapzone): keep stored gesture across reloads and share one grid
  - CONFLICT | CrossPointSettings.{cpp,h}, SettingsList.h, ReaderUtils.h, TapZoneSettingsActivity, tests | A_ALREADY | CPYOS persists `tapZoneMode`, `tapZonesCustomized`, `tapZones[9]`, `tapZonesLong[9]` and shares one `tapzones::Grid` between editor preview and `ReaderUtils::detectTouchPageTurn`. Its host test (`src/…/tap_zone_geometry`, `test/touch_page_turn`) is the CPYOS equivalent of upstream's 888-line test.
- `8418166c` feat(reader): add page bottom alignment option
  - CONFLICT | ReaderRenderSpec, Section, ChapterHtmlSlimParser, EpubReaderActivity, TextSettingsActivity, tests | A_ALREADY | CPYOS has `lib/Epub/Epub/PageBottomAlign.h`, `ReaderRenderSpec::verticalBottomAlign`, persisted in the section cache (`Section.cpp` 95/162/183) and forced off in vertical text. No-op.
- `a5b9e25c` fix(tapzone): zero-init popup options; sync touch-network test stub
  - CONFLICT | TapZoneSettingsActivity.h | A_ALREADY | CPYOS declares its own popup option storage; nothing to zero-init.
- `a68548d9` feat: Metalio E-Ink 4 device
  - CONFLICT | 17 files incl. platformio, HalGPIO, SettingsList, main.cpp | A_ALREADY | `cpyos` already has `[metalio_eink4_hardware]`/`[env:metalio_eink4]`, `FREEINK_DEVICE_METALIO_EINK4` branches in HalGPIO/HalPowerManager/FirmwareBoardTag, `src/HapticFeedback.h` + `STR_HAPTIC_FEEDBACK`, and the metalio charger fix (`7f08403a`). Upstream's `lib/hal/HalHaptics.h` is a different generation — do not import.
- `ba39565c` feat: Swipe to delete
  - CONFLICT | UiListActivity, MappedInputManager, listIcons, LibraryListActivity… | A_ALREADY | CPYOS has its own swipe-to-delete: `UiListActivity::swipeReveal`, `ACTION_SWIPE_DELETE`, `ListRevealAction`, `icon_trash_2_32`, plus inline confirm.
- `c77628a2` feat(reader): configurable 3x3 tap zones with mini edge zones
  - CONFLICT | 12 files | A_ALREADY | CPYOS has `TapZones.h`, `TapZoneSettingsActivity` (legacy/custom pills, short/long layer, preview band, OptionPopup picker, `tapZoneMode`/`tapZones[9]`/`tapZonesLong[9]`).
- `e852510f` fix: prevent font-size crash in the Text panel
  - CONFLICT | CrossPointSettings.cpp, SettingsList.h, EpubReaderActivity.cpp | A_ALREADY | CPYOS `applyReaderTextSettings()` already resets `section` inside a `RenderLock` **before** `SETTINGS.saveToFile()` with the same comment; the `SettingsList.cpp` split is CPYOS's inline-header layout.
- `f828f8ae` tap zones: use the shared OptionPopup for zone action picking
  - CONFLICT | TapZoneSettingsActivity.{cpp,h} | A_ALREADY | CPYOS's picker already uses the shared `OptionPopup` (`popup_`, `rebuildPopupOptions()`).

### ui-b (6)

- `13056043` perf: stop keeping the settings list in heap after boot and save 10KB (#4085)
  - n/a (hint) | — | A_ALREADY | CPYOS rebuilds the table at exact final size (`SettingsList.h:715-725`, 144 B/entry note).
- `1fd5ea3d` fix(popup): let a fresh logical Confirm through the entry barrier
  - n/a (hint) | — | A_ALREADY | CPYOS OptionPopup keeps the `wasPressed`-in-same-pass escape with the upstream comment; only the *deferred* barrier (cdab81d8/6cff5326) is still missing.
- `a585ccbb` fix: preserve UTF-8 when masking passwords (#4106)
  - n/a (hint) | — | A_ALREADY | CPYOS's reveal window uses `utf8Prev` (KeyboardEntryActivity.cpp:729).
- `ad971c8a` fix: keep the tilt setting when the IMU probe fails (#4078)
  - n/a (hint) | — | A_ALREADY | CPYOS keeps the row in the base list and only filters the UI copy (`SettingsList.h:844` erases `STR_MOTION_PAGE_TURN`), exactly upstream's end state.
- `f7716534` perf: drop fixed bookmark cache reserve (#4121)
  - n/a (hint) | — | A_ALREADY | No fixed bookmark reserve remains in CPYOS.
- `facaa87e` Load WiFi settings from file before sleep mode
  - n/a (hint) | — | A_ALREADY | `WIFI_STORE.loadFromFile()` before sleep-plugin delivery in main.cpp:710.

### fonts-i18n (5)

- `14423d4a` fix: add currency, letterlike, minus to latin-ext (#3801)
  - clean | — | **A_ALREADY** (1-line delta) | CPYOS's `fontconvert_sdcard.py` was rewritten and its `symbols`/`reading` presets already cover 0x20A0–20CF and 0x2200–22FF (minus). Residual: CPYOS's **`latin-ext`** preset is still the old list and lacks 0x20A0–20CF / 0x2100–214F / 0x2212 → fold those three ranges in if/when fonts are re-converted by that path (the FEXT1 packs use their own generator).
- `19985520` perf: drop 5 KB reserve in SD font registry (#4094)
  - conflict 4 files / ~5 hunks | `SdCardFontRegistry.cpp`, `BookMetadataCache.cpp`, `Memory.h`, `EpubReaderBookmarksActivity.cpp` | **A_ALREADY** | Verified in cpyos: `families_.reserve(MAX_SD_FAMILIES)` is gone, `discover()` trims capacity via a move+swap idiom (`std::vector(std::make_move_iterator…)…swap`, since `shrink_to_fit` is a no-op with `-fno-exceptions`), `tocFile`/`spineHrefIndex` no longer calls `shrink_to_fit`, and `BookmarkFile::load(epubPath, bookmarks);` has no follow-up shrink. Only the named `Memory.h::shrinkToFit` helper differs (equivalent).
- `4ad213c0` fix(i18n): localize the tap zone editor popup title
  - conflict 3 files / 4 hunks | `chinese.yaml`, `english.yaml`, `TapZoneSettingsActivity.cpp` | **A_ALREADY** | CPYOS's `TapZoneSettingsActivity::openPopup()` already builds the title from `tr(STR_TAP_ZONE) + number + tr(STR_TAP_ZONE_SHORT/LONG)` — no hardcoded `"Zone %d · %s"`, and it does not even use the removed `STR_TAP_ZONE_JUMP_PERCENT`.
- `5880246d` fix: restore font browser navigation and retry (#4253)
  - conflict 1 file / 3 hunks | `src/activities/settings/FontDownloadActivity.cpp` | **A_ALREADY** (mostly) | CPYOS's `FontDownloadActivity` already `final : public UiListActivity`, routes list input through the base protocol + `UiAppHost::routeTouch`, and retries on Confirm in `ERROR` (`retryDownloadOperation()`, `finishAutomaticFlow(...)`). Residual only: `COMPLETE`/`ERROR` branches use `wasPressed(Back/Confirm)` where CPYOS's input policy wants `wasReleased` — a 4-line tidy-up, not a port.
- `c1d99e56` fix: draw corrupt .cpfont glyph entries as missing (#4091)
  - conflict 7 files / ~8 hunks | `SdCardFont.cpp/.h`, `english.yaml`, `EpubReaderActivity.cpp/.h`, `SdCardFontTest.cpp`, `test/sd_card_font/stubs/HalStorage.h` | **A_ALREADY** (one gap) | Verified: CPYOS has `fileSize_`, `glyphEntryValid()` (bounds `data length`/bitmap offset), calls it in `prewarmStyle`/`fetchAdvances`/`faultGlyph`, and logs a capped number of entries. **Gap:** the user-facing side is absent — no `STR_FONT_DAMAGED` key and no `showFontDamagedMessage` toast, so a damaged `.cpfont` only appears in the serial log. Optional small port if we want the reader to say why text is missing.

### network (1)

- `f2120a3f` feat: Adds OPDS 2.0 support
  - n/a | — | **A_ALREADY** | Capability present but CPYOS wrote its own: `lib/OpdsParser/OpdsJsonParser.{h,cpp}` (streaming, bounded) + format sniffing in `OpdsFeed.cpp:24-49` (`xml_`/`json_`). Upstream's LCP/`DetailsActivity`/`OpdsTokenStore` UI half is deliberately not adopted (AGENTS §40.2). Nothing to port.

### build-docs (4)

- `30ca03df` perf: disable unused Expat input context (#4118)
  - clean (only test-CMake hunk left) | — | **A_ALREADY** | `XML_CONTEXT_BYTES=0` is already in CPYOS `platformio.ini`; the pick only produced a 1-line change in `test/koreader_xpath_resolver/CMakeLists.txt` (`1024`→`0`). Optional: align that test define, no runtime effect.
- `6c17b0ee` fix: sync the SdFat FAT cache before directory data (#4150)
  - empty | — | **A_ALREADY** | `scripts/patch_sdfat.py` + `sdfat_patches/0003-sync-fat-before-data-cache.patch` and the PATCHES triple already in CPYOS (§40.1). Confirmed empty pick.
- `843c1806` chore: remove completed 1.6.5 repair tooling
  - empty (all 3 files absent) | — | **A_ALREADY** | Net effect of 997b4331+843c1806 is zero for CPYOS. Confirmed no staged change.
- `ce7045c7` Update Plugin Hub repository references
  - conflict (doc-only paths) | USER_GUIDE.md, docs/sd-plugins.md | **D_SKIP / A_ALREADY** | Repo moved `jadehawk/PluginHub.crosspoint-plugin` → `crosspoint-reader/crosspoint-plugin-hub`. CPYOS has **zero** `jadehawk`/`PluginHub.crosspoint-plugin` references (`git grep`), so there is no dead URL to fix in code; only CPYOS's own docs would need it if they mention plugins at all.

### storage-misc-a (4)

- `5ab0f290` fix: hold the power mode mutex while switching to low-power
  - CONFLICT | lib/hal/HalPowerManager.cpp | **A_ALREADY** | CPYOS `HalPowerManager::setPowerSaving()` already does `xSemaphoreTake(modeMutex)` around the whole check+`setCpuFrequencyMhz` (single `targetFrequency`, plus `debugModeMutexHolderName()`). Drop the hunk.
- `87f9c34c` perf(zip): scan the central directory in one read per entry
  - CONFLICT | lib/ZipFile/ZipFile.{cpp,h} | **A_ALREADY** | CPYOS `ZipFile.cpp` already has `lastCentralDirPos`/`lastCentralDirPosValid` with the rewind fallback — the same optimization, written independently. Nothing to take beyond upstream's host tests.
- `9dfa92ba` fix(layout): allow breaking line after hyphens, dashes
  - CONFLICT | lib/Epub/Epub/Section.cpp | **A_ALREADY** (+version trap) | CPYOS already breaks after `foo—`: its word/non-word segmenter emits `continues=true,noSpaceBefore=true` via `endsWithBreakableHyphen()`/`TokenBoundary::allowsBreakAfterExplicitHyphen()` (`ParsedText.cpp:288,682,697`), and `allowsBreak(true,true)==true`. **Never take its `Section.cpp` hunk**: it would set `SECTION_FILE_VERSION` 55→56 while CPYOS's layout is at 87/86, so freshly written sections would be re-read under the wrong layout.
- `eecb909d` fix: release statistics vector storage before networking
  - CLEAN | — | **A_ALREADY** | CPYOS `ReadingStatsStore.cpp:1162-1166` already uses `std::vector<T>().swap(...)` with the same `-fno-exceptions` comment.

### misc-b-sdk (2)

- `6376516c` feat: recursive dictionary lookup (#4063)
  - conf: ~25 files (incl. src/clippings, ClipSelectionActivity deleted in CPYOS) | many | A_ALREADY | Absorbed: `src/util/Dictionary.h:84-90` documents the recursive re-read API, `DictionaryDefinitionActivity.h:36` has the recursion arming comment. Re-applying would re-add upstream `src/clippings/` (CPYOS replaced it with `AnnotationStore`). Do not apply.
- `b64d797c` feat: support static GIF images in EPUBs (#2299)
  - conf: many (lib/Epub, docs, platformio.ini) | see run | A_ALREADY | Already absorbed: `lib/GifSupport/{GifCommon.h,GifImageLayout.h,GifLimitedWriter.h}`, `lib/GifToBmpConverter/`, `lib/Epub/Epub/converters/GifToFramebufferConverter.cpp`, `scripts/patch_animatedgif.py` all present. `GifCommon.h` is byte-identical upstream; the other two files differ only by CPYOS's later edits. Do not re-apply.


## SDK-gated commits (SDK_DEFER, 8)

By area: misc-b-sdk=8

### misc-b-sdk (8)

- `10709120` fix: eego a4 battery levels use ADC voltage on some units
  - n/a (gitlink only) | — | SDK_DEFER | Gitlink-only, board not shipped (EEGO A4).
- `37138937` bump submodule pointer. eego and picco fixes
  - n/a (gitlink only) | — | SDK_DEFER | Gitlink-only ("eego and picco fixes"), both boards unshipped. Fold into the SDK delta.
- `49b84fdc` Add support for Picco display variant
  - n/a (gitlink only) | — | SDK_DEFER | Gitlink-only, board not shipped (Picco). Same as above.
- `6b5566c8` fix: route X3 power detection through the SDK
  - conf: freeink-sdk, docs/engineering/device-variants.md, test/CMakeLists.txt, test/test_hal_power_sources.py (deleted) | as listed | SDK_DEFER | App side deletes ~52 lines of X3 `BQ27220` I2C probing from `lib/hal/HalGPIO.cpp` (`X3GPIO::readBQ27220CurrentMA`, `isUsbConnected()` branch) — it applies cleanly **only because the SDK supplies the replacement**. 96de1be has `BQ27220` in `BoardConfig.h` but no `isUsbConnected`/power-source API in `libs/hardware/PowerManager`, so this cannot compile today. Matters for CPYOS because `default` (C3) is the X3/X4 target. Sequence after the SDK port; `docs/engineering/device-variants.md` is a CPYOS-owned file — merge by hand.
- `93a14afd` fix: update SDK to resolve X3 anti-aliasing freeze (#3960)
  - n/a (gitlink only) | — | SDK_DEFER | Gitlink-only. Claims: fixes an **X3 anti-aliasing freeze**. Reviewer should confirm the SDK commit is included in whatever pin CPYOS lands; CPYOS targets C3/X3 via `default`, so a freeze fix is worth tracking by name even though nothing app-side exists here.
- `98b8ac04` Touch calibration for EEGO A4
  - n/a (gitlink only) | — | SDK_DEFER | Gitlink-only, board not shipped (EEGO A4). Nothing to port; only relevant as part of the SDK delta.
- `9ba67ea5` fix: connect before adding plugin request headers (#4167)
  - conf: freeink-sdk only (CrossPointWebServer.cpp + PluginHttp.cpp applied clean) | freeink-sdk | SDK_DEFER | App hunks are clean but **require `freeink::SecureHttpClient::connect(shouldAbort)`**, which 96de1be does not expose (only `begin()`); the fix exists to stop multi-KB plugin cookies from consuming the heap the TLS handshake needs. Revisit with the SDK port: CPYOS's `src/util/PluginHttp.cpp` and `/api/fetch` have the same ordering today (CPYOS does have an equivalent concern — its own browser/JJWXC codebases show handshake-vs-heap fights at `largest≈7 KB`).
- `ad08729e` fix: Add WiFi TX power limit configuration (#3993)
  - conf: freeink-sdk only (src/main.cpp applied clean) | freeink-sdk | SDK_DEFER | App hunk = ~12 lines in `setup()`: register `ARDUINO_EVENT_WIFI_{STA,AP}_START` handlers calling `esp_wifi_set_max_tx_power(BoardConfig::ACTIVE.wifiMaxTxPower)`. **Blocked**: 96de1be's `BoardConfig.h` has no `wifiMaxTxPower` field (grep = 0). Port after the SDK bump, or backport the field into the CPYOS SDK patch if TX cap is wanted (regulatory/SAR argument, low functional value for ReadPico).


---

# Merge execution plan (how to reach behind_by = 0 without breaking CPYOS)

`behind_by` counts ancestry, not content. It reaches 0 the moment a commit with
`upstream/main` as a parent is pushed. The work below decides what we actually *take*.

## Decision 0 — the freeink-sdk gitlink (blocking choice)

A plain `git merge upstream/main` auto-resolves the gitlink to upstream `d9a1304c`, but the
build applies our 31-file patch set to `96de1be`. On `d9a1304c`, 12 of 114 hunks fail and the
facade/adapter (`FreeInkDisplay.cpp`, `EpdiyLcdDriver.*`) need a semantic rewrite; the CFF patch
must be dropped because upstream now ships a better CFF stack (cff+psaux+pshinter+psnames and a
heap-allocated `CF2_GlyphPath`).

Until re-ported, ~10 firmware files call patch-only SDK APIs (`epdiyLcdUpdateArea`,
`epdiyLcdCleanArea`, `epdiyLcdScanInProgress`, `displayWindow`, `idlePowerTick`,
`waitForScanIdle`, `commitGrayscale16Area(s)`, async helpers, push stats). **Taking d9a1304c
without the re-port does not link.** Feature-level losses: water-ripple page turn, band/regional
refresh, CKV anti-ghosting, rail idle poweroff, PMU short-press wake, SD 40/20/10 fallback,
DMA-aligned SD buffer.

Two-phase strategy (recommended):
1. **Zeroing merge — keep `96de1be`**: in the merge resolution, restore our gitlink
   (`git checkout --ours freeink-sdk` / `git update-index --cacheinfo 160000,96de1be…,freeink-sdk`).
   This is a deliberate "ours" resolution, not an accident.
2. **SDK fork + re-port (separate project)**: take `d9a1304c`, re-port the 12 failing hunks +
   facade/adapter, drop `cff-otf.patch`, commit to a `cpyos` branch of a freeink-sdk fork,
   switch the submodule URL + gitlink. ~3.5–4.5 dev-days + 1 device day, plus ~0.5 day infra.
   This permanently removes the "36 dirty files / `submodule update` destroys patches" hazard.
   Do **not** run `git submodule update --init` in the current tree before that.

## Decision 1 — what "merge" means for rejected upstream changes

For D_SKIP items we resolve conflicts by keeping CPYOS's implementation. The merge commit then
contains upstream as an ancestor (behind = 0) while the tree keeps our behavior. That is normal
for a downsteam fork, but it must be documented per item so nobody later assumes the upstream
behavior is present. The report above is that record.

## Phase 1 — mechanical merge (1.5–2.5 dev-days)

1. Fresh worktree/branch from `cpyos` (never the dirty public worktree):
   `git worktree add -b work/upstream-sync <path> cpyos`
2. `git merge --no-ff upstream/main` → expect **150 conflicted files** (list saved at
   `merge-conflicts.txt`).
3. Resolve by policy:
   - **Ours (keep CPYOS)**: every file in D_SKIP areas — renderer rewrite files
     (`Page*`, `TextBlock*`, `ParsedText*`, `CssParser*`, `CssStyle.h`,
     `ChapterHtmlSlimParser*`, `Section.*`), CPY settings/settings-list, CPY theme/components,
     `main.cpp`, `ActivityManager.*`, site/CI/release scripts, `partitions*`, branding/docs,
     `freeink-sdk` gitlink (see Decision 0).
   - **Theirs (take upstream)**: new files added by upstream (254) that belong to accepted areas
     (e.g., `lib/BmpToBmpConverter`, `HttpHeader`, `CjkBreaks`, plugin paging helpers).
   - **Semantic merge**: files where both sides added API (`FreeInkDisplay.h`/`PanelDriver.h` at
     SDK level; `GfxRenderer`, `HalDisplay`, `SdCardFontSystem`, `UiListActivity`,
     `OptionPopup.h`, `Dictionary*`, `CrossPointWebServer.cpp`).
   - **Keys only, never whole files**: `lib/I18n/translations/*.yaml` and
     `src/SettingsList.h` (CPYOS's table/key set diverged; taking upstream's files would drop
     CPYOS rows).
   - **Review, don't blindly accept**: the 412 auto-merged files; spot-check
     `src/SettingsList.h`, `src/CrossPointSettings.*`, `src/main.cpp`, `lib/I18n`, `platformio.ini`.
4. **Never copy upstream's `SECTION_FILE_VERSION` values.** CPYOS is 86/87; upstream is 56/59.
   In particular `9dfa92ba`'s `Section.cpp` hunk would set 56 and cause silent reuse of
   wrong-layout section caches — do not take it. Bump CPYOS versions only for real format changes.
5. Build gates before any push:
   - `readpico` and `readpico_nosource` (the latter must keep `nm | grep -c JjwxcActivity = 0`)
   - C3 `default`
   - host suites (renderer, fonts, dictionary, webdav, webbrowser, jjwxc private, i18n pack, …)
   - device: `exp/refresh_soak.py` 36/36, font/dict/web/OTA smoke
6. Push `cpyos` (fast-forward merge commit). Behind becomes 0. If upstream moved during the
   work, fetch + merge again; the delta is small.

## Phase 2 — absorb what matters (the real value, ~2–3 weeks total)

Do **not** try to land 77 C items inside the merge commit. Zero the count first, then port in
reviewable commits, each with tests.

### Tier 1 — live bugs / security in current CPYOS (do first)
| commit | issue |
|---|---|
| cdac66ff | WebDAV GET sends 1 byte per file (`client.write(HalFile)` → `Print::write(uint8_t)`) |
| 4c9ec287 | TTF drops gid 0 (`.notdef` missing instead of tofu) |
| 8cf400c6 | dictionary loses duplicate `.idx` headwords |
| f73d788a | OPDS feed state mutated without `RenderLock` |
| 6fb907a6 | TXT/MD tap-zone chapter navigation steps the spine |
| 148e6e2b | `.xtch` books never indexed; XTC shows filename/unknown author |
| 116195b4 | library indexes system folders |
| 91e9efc3 | web Fonts/Settings pages break with many SD families (CPYOS still pre-fix) |
| f1d0af1a | SD-font kern out-of-range read / 255-class loop wrap |
| 51e0baa2 | TTF init failure + no per-style prewarm/release |
| 3db7a96b | final cover-payload validation (`bfOffBits` bounds; CPYOS still accepts stubs) |
| 1f77b83d | log lines dropped at low-power clock |
| fe042860 | TXT BMP cover unchecked write (B, trivial) |
| 2ec01c96 | ~5 KB internal RAM + WS reaper for stuck uploads |
| e59f87b6 | Wi-Fi SSID truncated by bytes |
| aad48b40 | sub-chapter contents navigation missing |
| 40a633e7 | reader menu selection memory |
| 5d1142c1 | image grayscale toggle for EPUB image pages |
| f2a1beda | core rebuild cache key blind to component add/remove (AGENTS §4.3/§6 P0) |
| 9ac185c8 | plugin template variables not URL-encoded |
| 09fedb97 | library not refreshed after plugin book install |
| 39e56ca1 | static plugin catalog repeats page 1 forever |
| db9dd391 | `/api/fetch` method/body support |
| 552c932b | plugin relay buffers whole replies |
| 26ccbe28 | OPDS fetch failures not explained |

### Tier 2 — memory/performance, ordered chains
- Font chain (must be applied in order): `f1d0af1a` → `9610da70` → `b912e000` → `83df537d` →
  `98da1782`/`c6a2e311` → `4663d177`; then `70b3b1a3`, `51e0baa2`.
  Do not cherry-pick out of order: intermediate states read the wrong style.
- Pagination/rendering: `1a5c8cd1` (batch page-position reads), `f8936ba6` (idle pagination;
  the `deque` part already exists), `56c677f6` (frame-buffer loan for image probe),
  `9c3fbdd1`+`ad53eb64` (ruby), `31dc683f` (RTL focus), `72ca92fe` (CJK punctuation in dict),
  `8b2ba5e9` already present.
- Web/plugins: `e8299b5f`, `552c932b`, `c9cb1f8d` (ETag), `c3b16da5`+`7990db2b` (paging),
  `b4d9458f` (plugin uninstall), `b5cb…`-class fixes.
- Storage: `116195b4`, `7365c1ac` (book.bin cumulative sizes), `77725069`/`c54ee33f`
  (1-line guards), `148e6e2b`, `3db7a96b`.
- Dictionary: `8cf400c6`, `69c8d867`, `b6605a51`, `280895ff` (already B, dictzip 8 KB window),
  `72ca92fe`.
- Sleep/UI: `6251054b` (B), `51089beb` (transparent overlay), `2a8c1ec5` (warning inside dialog),
  `d663dcc6` (sleep image shuffle), `0ecc7d78` (back-tap padding), `5d1142c1`.

### Tier 3 — blocked on the SDK fork
`ad08729e` (`wifiMaxTxPower`), `9ba67ea5` (`SecureHttpClient::connect()`),
`6b5566c8` (X3 power via SDK), `60d3c8bb` (cover upscale + BmpToBmp + gitlink),
`e54c0087` (Metalio sleep gitlink half), `ce781eb8`, `f0dfe5fc`. These compile only on `d9a1304c`.

## Out of scope by product decision (do not resurrect)
- `0305aaa6` upstream 2.0.0 renderer rewrite (source ranges, drop caps, CSS cascade, tables).
- `327e7219` AES-256 manifests (SDK only supports aes128-cbc).
- `8fc2b661` gyro-only tilt (Read Pico has no gyro; CPYOS uses accelerometer).
- `88ea9bcf` 1.1/1.2 line spacing (CPYOS chose 1.3/1.6).
- `d7fcc7ce` upstream clippings/source ranges (CPYOS has AnnotationStore).
- `fecee610` upstream BLE page-turner module (CPYOS has its own BLE input).
- Upstream boards we do not ship (Metalio/Eego/Picco/Sticky/BOOX/Waveshare/LilyGO) unless a
  shared-HAL fix is extracted.
- Branding/docs/release/CI rewrites.

## Notes on method and confidence
- 254 non-merge commits audited individually; 31 merge commits are containers whose net content
  is represented by commits inside the range (verified against first parents; sync merges bring
  the other lineage, whose commits predate the fork point or are in range).
- Every row's "apply" result comes from a real `git cherry-pick -n` in a clean detached worktree
  at `834e6207`; "ALREADY" claims are backed by a symbol/file grep in the cpyos tree.
- Standalone cherry-pick results are a lower bound: a conflict there can disappear in the full
  merge, and a clean pick can still be wrong if it depends on an excluded subsystem.
- Per-file conflict list: `merge-conflicts.txt`; raw per-commit tables: `findings/0*.md`;
  machine-readable: `classified.json`, `list-<CLASS>.tsv`.

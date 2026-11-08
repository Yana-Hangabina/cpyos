# 维持拒绝项详细审阅（桶 3 KEEP_REJECTED）

来源：`2026-11-08-upstream-bucket3-reaudit.md` 的逐项表。每项都重新读过上游 diff 与 CPYOS 当前实现；
本文件把所有 **KEEP_REJECTED** 项按区域整理，保留“为什么拒绝”和“什么条件下可以重新考虑”。
复审结论：**70 项维持拒绝**，其中多数有明确的“翻转条件”（新硬件、SDK 能力、产品方向变化、上游修复方式改变）。

> 说明：这些项**未合入**；如果未来要重新评估，先核对每行的翻转条件，再从上游 diff 重新论证，不要只看结论。


## 渲染/EPUB + 存储/封面/JSON（12）

### `fdf5f76c` — non-CJK GUI fallback font support

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Every hunk is either unreachable on Read Pico/C3 or already implemented better. Change only if CPYOS drops the system font pack (then the probe gate becomes live and 14 probes beat 10). Dead helper can be deleted by hand any time.

### `0305aaa6` — improve epub rendering (2.0.0 layout)

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Structural rewrite, not a merge. `Page.h` here has tags 1-3 only and no `hasWordFonts`; adopting means re-deriving CPYOS's whole paging layer with a device-verified plan. Changes only if CPYOS decides to rewrite layout deliberately (then re-derive per feature: drop caps → separate plan).

### `2c09b8ce` — remove unused image-existence helper

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：`ImageBlock::imageExists()` (`blocks/ImageBlock.cpp:47`) has zero callers; deleting it is a 3-line edit with no merge value. Hand-delete if the file is touched.

### `b0aadf2a` — shorten JPEG/cover comments

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Doc commit for state CPYOS already has.

### `809c1dab` — LilyGO T5S3 expander button → down

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Hardware CPYOS does not ship. Revisit only if a T5S3 target is added.

### `186d9407` — stop retrying a failed cover conversion

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Superseded in-tree; taking it would re-introduce the 0-byte sentinel.

### `12e441f7` — drop the failed-cover empty sentinel

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Same as above. Verify against `7352b33f` if in doubt.

### `6d2cd3a1` — refactor agents guidelines

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Guidance-only; taking it would replace CPYOS project knowledge with generic upstream rules. Individual review skills could be cherry-picked by hand; no product effect.

### `3607ea7b` — further refine agents guidelines

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Same.

### `d1f5d1ec` — yet more refinement of agents guidelines

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Same.

### `bb855ef9` — clarify scope for reusable downstream primitives

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Doc for upstream's contribution policy; CPYOS keeps its own scope doc.

### `fecee610` — Bluetooth page turner for X3/X4 (one module owns BLE)

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：A parallel implementation on the same radio; merging means either deleting CPYOS's BLE stack or running two owners. Optional nuggets if BLE drain ever becomes a complaint: idle radio auto-off (`kIdleOffMs = 30 s`) and the measured X3/S3 heap budgets — cherry-pick as small constants, not as a module.


## UI-A（主页/书架/键盘/布局/导航）（12）

### `43694c90` — show popup for kosync server-type selector

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Nothing to fix. Revisit only if CPYOS adds the kosync/other server-type setting — then use `OptionPopup` (already the CPYOS pattern for `SettingInfo::Enum`) instead of the upstream `optionPopup` member.

### `c201af43` — dict: multi-token lookup using clippings

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Dependency is not portable. If phrase lookup is wanted, build it **CPYOS-native**: seed `DictionaryDefinitionActivity` from the `AnnotationStore` range selected by long-press/drag (one lookup over `paragraph → text range`), and keep the existing two-step "select then confirm" UX. That is a new feature, not this commit.

### `8fc2b661` — check for gyroscope presence in tilt sensor init

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Both the API and the semantics are wrong here. CPYOS already solved the underlying concern (accel-only part) explicitly. The only adoptable nugget for a future SDK bump is replacing the `#if FREEINK_DEVICE_READPICO` + `imuType == Sc7a20h` probe with `!hasGyroscope()`, which is cosmetic.

### `01f09e0e` — respect content-area insets and corner radius

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：No board CPYOS ships has a rounded panel and no target page is missing the safe-area inset. Change if a rounded-corner device is added: then port `BaseTheme::headerBand()` + the status-bar `max(margin, corner)` clearance (and the tab-divider/tab-pct helpers), not the Library/Wifi/Home hunks.

### `944b741e` — clang-format the menu hit-test rect initializer

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Formatter churn on code CPYOS replaced (`HomeActivity` → `InxRecentActivity`/My). A future clang-format-21 pass covers it.

### `87fc4ab7` — settings grouping (hub + per-group sub-activities)

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Same feature, different design, and CPYOS's variant is already verified on device. Change only as a deliberate UX decision (hub vs. headers); then re-derive, do not merge.

### `ec32b959` — share actions and consolidate page selection

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Cannot apply without resurrecting INX. If the CPY main-tab bar ever grows a "focus the page first" model, the two real UX fixes are worth re-reading: don't swallow the first Confirm, and fold the focus repaint into the release frame (`mainTabTouchFocusPending`). Not now.

### `d52fc820` — remove unused rectangle touch helper

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Upstream-only cleanup. Would only become applicable if CPYOS migrated every game/viewer to the FreeInkApp hit-rect model.

### `c7996b9f` — reserve capacity for the page bottom-align entry

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Same guarantee already enforced structurally and by a stronger test. **Actionable note:** AGENTS §40.3 records that CPYOS's own `test_settings_api_keys.py` currently fails on "a 5-item insertion batch" — that is this bug class landing on CPYOS's table. Fix it CPYOS-side (recut the batches / bump `entryCount`), do not take upstream's numbers.

### `261726f6` — BOOX Picco device

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Hardware CPYOS does not ship and cannot test. Revisit only if a Picco target is added; the only possibly reusable idea for Read Pico is `wakeOnHeldButton`, which its PMU hand-off already covers differently.

### `31480212` — clang-format 21 cleanups in tap-zone settings

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Formatter-only; a clang-format-21 pass on CPYOS (when installed) covers it.

### `35dccb87` — split the tap-zone batch and reserve its capacity

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Bookkeeping for upstream's numbers. If CPYOS's own `entryCount` assertion is currently red (§40.3), split CPYOS's batches / recount there instead.


## UI-B（阅读器菜单/点按区/弹窗/睡眠/批注/BLE/板卡）（14）

### `e9635421` — reserve capacity for the tap-zone row

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：CPYOS has its own count (`SettingsList.h:239` = 93 + caps) and an API test that asserts it (`:723`). Nothing to port; the invariant (bump count in the same commit as a base row) already holds.

### `cac12269` — incomplete-type owner lifecycles out of line

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：`ActivityManager.h:95-101` still inlines ctor/dtor and every env builds; the `Activity` fwd-decl + `unique_ptr` member is only ill-formed in a TU that destroys the manager without `Activity.h`, and none exists. Route if it ever breaks: move both to `.cpp`, include `Activity.h` there (drop the `onSelectBook` rename — CPYOS's HomeActivity is unrelated).

### `b823d54b` — page-turn DynamicEnum owns its value

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：The real fix (getter/setter branch **before** `valuePtr`) is already in CPYOS (`SettingsActivity.cpp:928` before `:534`; `settingValueText` at `:1348` before `:1338`). CPYOS lists 5 labels including DISABLED, so stored 4 is not out of range; it never writes `touchControlsVersion`.

### `e41982a4` — keep mixed gesture pairs intact

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：CPYOS expands the legacy mode only when **both** gesture keys are absent (`CrossPointSettings.cpp:568-577`) and never rewrites `previousPageGesture`; INVERTED_TAP is honoured at read time in `detectTouchPageTurn`, so the zone swap the commit adds is not needed.

### `ad24aea8` — delete clipping popup blinks

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：No `ClipSelection/EpubReaderClipping*` in `src/activities/reader`; CPYOS uses AnnotationStore + InlineConfirm.

### `471243b2` — hide long-press menu on Home-key/Picco boards

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：`if (hasTouch()) return 0` would disable long-press Confirm on **readpico** (touch), where `confirmLongPressThreshold()` (`EpubReaderActivity.cpp:3183`) drives bookmark/dict/menu and the row is deliberately visible. No shipped CPYOS board sets `hasHomeKey` (X3/X4/readpico all false), so the stale-value defect is unreachable. Flip only if a home-key board is added — and then use `hasHomeKey()`, not `hasTouch()`.

### `5407ffcd` — consolidate touch settings filtering

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Moves the filter from `hasHomeKey` to `hasTouch`, i.e. removes a working readpico feature; CPYOS's filter (`SettingsActivity.cpp:472`) is the correct one for its board set. Bundled gitlink bump is unwanted.

### `88ea9bcf` — 1.1/1.2 line spacing for `.cpfont`

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Reverts upstream's own 1.3/1.6 to 1.1/1.2; CPYOS chose 1.3/1.6 deliberately (`getReaderLineCompression`, `CrossPointSettings.cpp:958`) and CJK bodies read better airy. Flip if users report wide/extra-wide as too loose on an SD `.cpfont`.

### `fb2a8a8f` — new Picco & Sticky displays

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：4 of 10 hunks need the new SDK/board fields. ★ Its `Frontlight.present()` gate is a **trap**: CPYOS uses `FrontlightPanelActivity` as the control center on touch boards **without** a frontlight (`MappedInputManager.cpp:535` says so), so the gate would remove the control center from readpico. `BwFrameKeeper` no-ops where absolute gray is supported (both CPYOS boards).

### `8b4c0d6e` — unify INX header status placement

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：`src/components/themes/` holds only `cpy` and `upstream`; the `inx` theme is deleted (`OptionPopup.h:32` "INX is retired") and `UI_THEME` has a single value. Dead code path.

### `b2b1e15a` — icons instead of text labels in FrontlightPanel

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：CPYOS's sheet is its own labelled-tile grid (deliberate, §27/V-series); no defect being fixed, and the icon row is the base b2b1e15a's successor feature builds on.

### `3f8345c5` — toolbar padding with roundedslides theme

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：CPYOS ships only `UI_THEME::CPY`; under CPY layout the row is never inverted and the cursor is a thin bar outside the inset, so `sidePadding = 0` (`ReaderToolbarUi.cpp:747`) is intentional and correct. Applies only if a selection-shape theme is revived.

### `8f4d035c` — Picco switch as hardware setting

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Picco-only board; `src/util/SlideSwitch.h` does not exist. The generic bits (`HalFrontlight::begin` brightness-before-warmth; `withWebHidden`) are moot: CPYOS boards have no frontlight driver path, and there is no setting-list-driven web API to hide the raw clock/timezone indexes from.

### `4ef42ee1` — skip debug-only diagnostics in release builds

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：The two guarded blocks compute `HalMemory::getInternalHeap()`; **neither file contains it** (0 matches). CPYOS's remaining `LOG_DBG("LIB"/"DICT", …)` blocks only format existing fields, and all envs build warning-free.


## 字体/i18n（13）

### `40b313a3` — Azerbaijani translation + Ə/ə glyph support

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Only the 2-glyph idea is valid, and its correct route is the same one §55.3 used for `·/«/»`: add U+018F/U+0259 to `CPYOSHanSansSC` and rebuild the `.def`, *not* 38 regenerated headers. Flip: if az (or any Latin-Ext-B locale) is added → add those two codepoints to the pack first, then take the yaml only.

### `d7fcc7ce` — clip/highlight EPUB text

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：CPYOS has `lib/Epub/Epub/AnnotationStore.{h,cpp}` with its own UI; upstream's version is anchored on `ParsedText` source ranges that §40.2 rejected for size (≈8 B/word, doubled for CJK tokens). Nothing to port.

### `965722cf` — offer Libron in Manage Fonts

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：The device reads its catalog from `CrossMuxEndpoints::FONT_MANIFEST_FORMAT` (crossmux host). A local `sd-fonts.yaml` entry reaches no user unless CPYOS starts publishing that JSON itself; CPYOS's own extra-font channel is FEXT1 (`font-extras/packs.json` → Pages). Flip: only if CPYOS hosts its own `fonts.json` — then this is a 11-line ADOPT_NOW.

### `1899de8f` — Cyrillic in built-in subsets

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：The stated bug does not exist here: the reader face comes from the system pack (`main.cpp:355`, `CROSSPOINT_SYSTEM_FONT_PACK`), and the pack has А/Ё/№ (cmap probe). Taking the 2 lines would look like a fix while changing nothing. Flip: only as part of a deliberate, calibrated header regeneration.

### `ce781eb8` — prewarm visible CJK glyphs before list painting

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：The stall it fixes is `FontDecompressor` group decompress on built-in compressed faces. On both shipped targets `typography::apply()` rebinds every UI role to the vector pack (`SystemFontPack`), so that path is unreachable except the 8 pt emergency fallback. Idea only. Flip: a measured first-list-paint stall on device with the pack absent.

### `fc2de152` — move NotoSerif to optional SD font

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Deleting the headers without upstream's settings rework **breaks id generation**; the ids have no registered face anyway (see side finding). Hygiene only, negative flash delta, non-zero risk. Flip: only if we drop the `NOTOSERIF` reader-family option first.

### `7242d9ae` — generate font IDs with Python

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：CPYOS's `fontIds.h` carries macros upstream's porter does not emit (`CJK_UI_18_FONT_ID`, `CJK_READER_SIZE_FONT_ID(pt)`, the `CJK_READER_SIZE` static_asserts). Adopting it would silently drop them; a Python port is only worth doing as our own rewrite.

### `3f0f4fad` — ship Cyrillic + refresh pagination with font data

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：(a) is a no-op here (pack has А/Ё/№). (b) is real but wrong-tooled: the live reader face is the pack, and CPYOS already invalidates with `SECTION_FILE_VERSION` 86/87 per font change (§43.2/§45.2), which is the same guarantee without hashing an uncompiled header. Flip: if pack font files ever change silently and a stale-pagination bug is observed.

### `cf50ce73` — Ubuntu for the small UI font

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：`main.cpp:407-411`: `SMALL` = `notosans_12` on high-DPI, `notosans_8` only otherwise; `UI_10/UI_12` are already Ubuntu. On the pack both are rebound to pack size faces, so this is invisible to users.

### `2648933a` — reuse loaded UI font for CSS variants

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：That path does not exist in CPYOS (grep for `variantFontId`/`loadFamilyVariant` → 0 hits); `loadFamilyExtraSize` already reuses an already-loaded size (`SdCardFontManager.cpp:104-108`), which is the whole local benefit. Zero effect, and it rewrites the manager we patched. Flip: if a CSS-size variant path is introduced.

### `98da1782` — compacted style API in advance-table scan

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：`buildAdvanceTablePacked()` already uses `resolveStyleMask()` + `hasStyle(si)` + `styleData(si)` (`SdCardFont.cpp:1934-1975`); there is no `styles_[si]` per-style indexing to repair. Cherry-picking it ahead of `b912e000` is actively wrong.

### `c6a2e311` — build fix + regression test after #3948/#4114

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：It adds `AdvancesBuildForAStyleStoredInAnotherSlot` for the `98da1782` bug — the bug is absent locally, so the test would assert behaviour CPYOS already has under a different implementation.

### `072b6fc7` — line spacing as % of font size

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：`CrossPointSettings::getReaderLineCompression()` (`:935-980`) is a deliberate 4-level, per-family design (0.90–1.05 sans, 0.95–1.2 serif, 0.95–1.6 SD/vector); §40.2 already rejected upstream's retune. Only the "line height from font size" idea transfers, and it needs device comparison. Flip: continuous-spacing requests + a device pass on vertical text.


## 网络/插件 + 杂项（10）

### `a722472c` — Reset publication object using uniform initialization

- **代价/风险**：**KEEP_REJECTED**
- **拒绝理由与翻转条件**：`OpdsPublication` / `openPublicationDetail()` do not exist in this tree (`grep` 0 hits; `src/activities/browser/OpdsBookBrowserActivity.*` has no publication detail path). The hunk *is* a real shadowing bug (`OpdsPublication publication;` re-declared inside the `if`), but there is nothing to fix. Flip: only if the OPDS item-detail page is ported.

### `55a77c2e` — fix: web UI dropdown chevron inset (#4090)

- **代价/风险**：**KEEP_REJECTED**
- **拒绝理由与翻转条件**：Target file absent. CPYOS keeps native `appearance` on `select` (`en/SettingsPage.html:95-110`: `padding:6px 10px; min-width:160px`, no `appearance:none`), so the flush-chevron artefact this patches cannot occur. Flip: a user reports the arrow touching the border — then add the `appearance:none` + 36 px padding rule to the page's inline CSS.

### `e93f8d1f` — feat: combine fixed and dynamic plugin lists (#4198)

- **代价/风险**：**KEEP_REJECTED**
- **拒绝理由与翻转条件**：Needs `listIndexLoaded` / `fixedListCount` on top of `loadBrowseListIndex()`, none of which exist: CPYOS has only manifest-declared `browse.lists` + `wantsListPicker()` (`PluginCatalogActivity.cpp:200,457`), and `grep lists_url` = 0 hits in `src/` and `docs/`. Flip: after the dynamic list-index feature is ported (it is not in this slice).

### `d5d0bea1` — feat: headers and mapping for plugin list index (#4197)

- **代价/风险**：**KEEP_REJECTED**
- **拒绝理由与翻转条件**：The diff *edits* `loadBrowseListIndex(int&)`, which does not exist in CPYOS. Separate finding worth acting on independently: its `MAX_BROWSE_LISTS = 64` bound exists because an unbounded `reserve()` aborts with `-fno-exceptions`; CPYOS's `manifest.browseLists.reserve(browse["lists"].size())` (`:200`) is still unbounded against a hostile `plugin.json`. Fix that locally (clamp to 64) rather than porting the commit.

### `861683cc` — feat(plugin): filter catalog lists by declared capabilities (#4260)

- **代价/风险**：**KEEP_REJECTED**
- **拒绝理由与翻转条件**：Needs `firmwareidentity::PLUGIN_CAPABILITIES`: CPYOS has no `src/util/FirmwareIdentity.h`, no `PLUGIN_API_VERSION`/`PLUGIN_CAPABILITIES` (grep 0), and no `matchesCapabilities()` — the whole capability vocabulary is absent, so a filter would advertise a scheme no CPYOS plugin can declare. Flip: with the capability API, then this commit (it also needs the list index).

### `c9f64d0d` — feat: move plugin settings to a Plugins web page, consolidate styles (#4272)

- **代价/风险**：**KEEP_REJECTED** (fold into 057e16a4)
- **拒绝理由与翻转条件**：Pure relocation/CSS consolidation for upstream's layout: CPYOS mounts plugins inline (`#plugin-container`) and has no `web.css`/`Header.html.inc`. One salvageable idea inside it: serve `shared/PluginHost.js.inc` (8 837 B) once instead of inlining it into 5 documents (en/zh Settings, en/zh Files, Runner) — but the gzip saving is ~2-3 KB of a 160 KB pack and it only pays off together with 057e16a4's shared-host split. Take it there, not on its own.

### `f3ace194` — test: trim redundant cover cache cases

- **代价/风险**：**KEEP_REJECTED**
- **拒绝理由与翻转条件**：Test-only consolidation of `ensureThumbnail`/`checkCachedCover` cases, including the `RegeneratesHeaderOnlyThumbnail` case this commit deletes. CPYOS's loader has diverged (Gray8 `_g8` thumbnails, 10-slot PSRAM byte cache, `thumbPathFor` memo), so trimming would remove coverage of code CPYOS changed, not duplicates.

### `052d481a` — test(tapzone): pin DISABLED to a stored 4

- **代价/风险**：**KEEP_REJECTED**
- **拒绝理由与翻转条件**：File/directory does not exist. CPYOS's tap action enum is `tapzones::Action::NONE = 0` (not 4), and the migration it pins (`seedZones()` from gesture pairs) is already covered: `test/inx_navigation/test_tap_zone_geometry.py:14,89` sweeps every gesture combination through the real header.

### `5eb498b0` — test(settings): mixed gesture pair migration regressions

- **代价/风险**：**KEEP_REJECTED**
- **拒绝理由与翻转条件**：Same missing directory. `seedZones()` mixed-pair behaviour is exercised by the existing geometry test; nothing new to pin.

### `18d611f2` — test: fix two host-test failures on macOS / parallel ctest

- **代价/风险**：**KEEP_REJECTED**
- **拒绝理由与翻转条件**：`test/test_recursive_dictionary.py` and `DictionaryHtmlPagesTest.cpp` are not in CPYOS (the recursive-dictionary test dir was not taken with 6376516c). Nothing to fix.


## 构建/CI/文档（9）

### `59912ea9` — link available PR firmware artifacts

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Nothing to fix in a workflow CPYOS does not run. Portable only if CPYOS ever enables PR firmware previews (then: link only unexpired artifacts, never claim "not available" when the build succeeded).

### `35072257` — README → 2.0.0

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Branding + version scheme (`0.x-cpyos`, tag `cpyos-X.Y.Z`).

### `e7b47ee5` — CI on push to `develop`

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：CPYOS branches are `cpyos`/`work/*`; upstream workflows are disabled.

### `6ec32892` — 1.6.5 release notes

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：CPYOS keeps `docs/releases/<v>.md` + `.ota.json`; layout already matches.

### `997b4331` — restrict Stable to four targets

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：CPYOS publishes one board (readpico, tag-scoped) and already selects OTA by board tag; the portable rule is already its behaviour. Keep in mind only if CPYOS ever adds a second channel.

### `414b3797` — OPDS harness render-lock lifetime

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Stub now needs `~RenderLock()`. CPYOS's real `RenderLock` has a destructor plus a same-task recursion guard (AGENTS §49.5/§49.7), so upstream's stub is strictly weaker.

### `c17b80ab` — Paper Mono in frontlight guide

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Nothing in CPYOS's guide refers to frontlight; rule has no consumer.

### `a6db8f15` — README link → Silkscreen

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Branding/links.

### `91ed3ef8` — README bolding

- **代价/风险**：KEEP_REJECTED
- **拒绝理由与翻转条件**：Markdown only.

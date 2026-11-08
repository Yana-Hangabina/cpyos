# 桶 3 逐项复审（D_SKIP 88 + SDK_DEFER 8）

复审方式：每条重新阅读上游 diff 与 CPYOS 当前实现，按收益/影响用户/成本/风险/依赖重判；旧结论只作为待验证的假设。判定枚举：ADOPT_NOW / ADOPT_PARTIAL / ADOPT_AFTER_SDK / KEEP_REJECTED / NEEDS_DEVICE_CHECK。

汇总：ADOPT_PARTIAL 14 · ADOPT_AFTER_SDK 2 · NEEDS_DEVICE_CHECK 2 · KEEP_REJECTED 70；SDK_DEFER 8 项见末节。


---

## renderer/EPUB + storage/covers/JSON

# Bucket 3 — renderer / EPUB / storage / covers / docs (15 items)

Re-evaluated against `work/upstream-sync` @ `dfadf349` (buckets 1+2 merged).
Prior verdicts were treated as claims, not evidence; every item was re-read from the diff and from the current tree.

Evidence anchors used repeatedly:
- `src/SdCardFontSystem.cpp:339-350` — `setupUiFallbacks()` returns early when `CROSSPOINT_SYSTEM_FONT_PACK` +
  `system_font_pack::ready()`, and again for `ENABLE_CHINESE_VERSION && !BOARD_HAS_PSRAM`. Both shipped targets
  (readpico, C3 X3/X4) define the pack (`platformio.ini:331`, `:1193`), so the SD UI-fallback probe block is dead
  on every device CPYOS ships.
- `lib/GfxRenderer/GfxRenderer.cpp:349-420` — `resolveTextFontId()` is already glyph-driven (counts missing
  codepoints over a 12-entry candidate chain: fallback + `supplementalFontMap_` + emoji), not CJK-gated.
- `lib/Epub/Epub/ParsedText.cpp:1035-1100` — `computeLineBreaks()` honours `calculateRubyExtraStartOffset` /
  `calculateRubyExtraEndOffset` and kinsoku hanging; `:1139-1210` `computeHyphenatedLineBreaks()` does **neither**.
- `lib/Epub/ContentProtection.cpp:110-133` — probe → `openFromScan()` → `isProtected()`; a non-`aes128` manifest
  yields `protected_=false` and the book opens normally (no error path).
- `freeink-sdk/libs/book/ContentProtection/src/ProtectedBook.cpp:295` — only `aes128-cbc` is matched.
- Already-merged upstream: `7352b33f` = "upstream 3db7a96b" (cover payload/size validation) — the final state of
  the three cover commits is in the tree at `src/util/BookCoverLoader.cpp:20-45`.

| SHA | subject | benefit | cost/risk | deps | verdict | rationale |
|---|---|---|---|---|---|---|
| fdf5f76c | non-CJK GUI fallback font support | ~0 for CPYOS: the widened probe list sits in a block that cannot run (SYSTEM_FONT_PACK early return on both targets; C3 also returns at `:350`), and the `GfxRenderer` half is superseded by CPYOS's missing-codepoint resolver. Only residual is deleting the now-dead `utf8IsCjkCodepoint` (`lib/Utf8/Utf8.h:80`, 0 callers). | 3 files, 90% comment churn; the `SdCardFontSystem` hunks conflict. | none | KEEP_REJECTED | Every hunk is either unreachable on Read Pico/C3 or already implemented better. Change only if CPYOS drops the system font pack (then the probe gate becomes live and 14 probes beat 10). Dead helper can be deleted by hand any time. |
| 327e7219 | recognise AES-256-CBC manifests | Real only with an SDK that can decrypt AES-256. With today's SDK the probe change is a no-op: `openFromScan()` finds no `aes128-cbc` URI, `isProtected()` stays false, the reader opens the file exactly as it does now. Prior reason ("would be flagged protected then fail") is wrong mechanically, but the conclusion holds for a different reason. | 1 line + 1 test. | needs `ProtectedBook` AES-256 support (local SDK patch or gitlink bump). | ADOPT_AFTER_SDK | Port the one-line probe + test together with (and only after) adding `aes256-cbc` to `freeink-sdk/.../ProtectedBook.cpp` + its crypto. Today's protected-EPUB population (JJWXC) is `aes128-cbc`, so there is no current user-visible bug. |
| 0305aaa6 | improve epub rendering (2.0.0 layout) | Would fix real typographic gaps CPYOS still has: drop caps, `white-space`, `font-size` keywords, small-caps, tables with column pre-scan, richer CSS cascade, widow/orphan control. | 45 files / +4.9k; per-word font ids + `Page` tags 4/5 rewrite the layout core; collides with CPYOS `SECTION_FILE_VERSION` 86/87, vertical text, `PageBottomAlign`, ruby geometry, 16-level AA path, page cache. | excludes: source ranges/clippings, per-word fonts, BLE. | KEEP_REJECTED | Structural rewrite, not a merge. `Page.h` here has tags 1-3 only and no `hasWordFonts`; adopting means re-deriving CPYOS's whole paging layer with a device-verified plan. Changes only if CPYOS decides to rewrite layout deliberately (then re-derive per feature: drop caps → separate plan). |
| 2c09b8ce | remove unused image-existence helper | none (compiled-out dead code). | 3 lines. | none | KEEP_REJECTED | `ImageBlock::imageExists()` (`blocks/ImageBlock.cpp:47`) has zero callers; deleting it is a 3-line edit with no merge value. Hand-delete if the file is touched. |
| cb2a4c78 | correct hyphenation with dropcaps | **Real bug remains in CPYOS**: `computeHyphenatedLineBreaks()` starts `lineWidth = 0` and never applies `calculateRubyExtraStartOffset`/`calculateRubyExtraEndOffset`, and it has no `lastFittingBreak` correction or kinsoku-hanging allowance — unlike `computeLineBreaks()`. With hyphenation on + ruby (furigana) or hanging punctuation, lines overflow the right margin. Affects both boards, EPUB only. | `lib/Epub/Epub/ParsedText.cpp` only (~25 lines); pagination changes ⇒ bump `SECTION_FILE_VERSION` 86/87 for a one-time rebuild. | none (drop-cap `maxLines` half is N/A: CPYOS has no drop caps and no `maxLines`). | ADOPT_PARTIAL | Port only the ruby/legality half into `computeHyphenatedLineBreaks()`: init `lineWidth` from `calculateRubyExtraStartOffset`, gate "word fits" on `+ extraEndOffset` (mirror `computeLineBreaks`), track `lastFittingBreak` using `TokenBoundary::allowsBreak`, and keep the hanging allowance. Reuse upstream's em-dash test vectors. Skip `maxLines`, `endIsKnown`, `isLastLine = lineBreak == words.size()` (all only meaningful for bounded layout). |
| b0aadf2a | shorten JPEG/cover comments | none — the annotated code (`bb4f8785`) is already present and correct: `JpegToBmpConverter.cpp:650-656` has `scaleDenominator`, `BookCoverLoader.cpp:27-33` validates `pixelDataOffset + pixelBytes`. Only comment wording differs. | comment-only. | none | KEEP_REJECTED | Doc commit for state CPYOS already has. |
| 809c1dab | LilyGO T5S3 expander button → down | none: no LilyGO env in `platformio.ini`; the reader hunk is a comment edit. | — | `#if FREEINK_DEVICE_LILYGO_T5S3` subsystem not in CPYOS. | KEEP_REJECTED | Hardware CPYOS does not ship. Revisit only if a T5S3 target is added. |
| 186d9407 | stop retrying a failed cover conversion | none left: its sentinel design was reverted upstream, and the end state is already merged as `7352b33f` (upstream `3db7a96b`) — `BookCoverLoader.cpp:20-45` validates the payload and returns `Terminal` for unrepairable caches. | — | — | KEEP_REJECTED | Superseded in-tree; taking it would re-introduce the 0-byte sentinel. |
| 12e441f7 | drop the failed-cover empty sentinel | none: intermediate state of the same series; `3db7a96b` (merged) is the final form. | — | — | KEEP_REJECTED | Same as above. Verify against `7352b33f` if in doubt. |
| 6d2cd3a1 | refactor agents guidelines | Upstream replaces `AGENTS.md` with `.agents/rules/*` and moves skills. CPYOS already has `.agents/skills/` (8 skills incl. its own `sync-upstream`) plus a project-specific `AGENTS.md` that is the authoritative handover doc. | 30+ doc files; would delete CPYOS's AGENTS.md. | none | KEEP_REJECTED | Guidance-only; taking it would replace CPYOS project knowledge with generic upstream rules. Individual review skills could be cherry-picked by hand; no product effect. |
| 3607ea7b | further refine agents guidelines | none (incremental doc edits on the above). | — | — | KEEP_REJECTED | Same. |
| d1f5d1ec | yet more refinement of agents guidelines | none (rewrites the `.agents/rules` that CPYOS does not adopt). | — | — | KEEP_REJECTED | Same. |
| 65e3f9d0 | custom HTTP headers for OPDS / KOReader Sync | Genuine gap: CPYOS's OPDS stores only `name/url/username/password` (`src/OpdsServerStore.h`), so token/reverse-proxy-protected catalogs (Cloudflare Access, `X-API-Key`) cannot be reached; KOReader sync has the same shape. Small, additive plumbing (`std::vector<Header>` already flows into `runGetSecure`). | `lib/HttpHeader` (38 lines) + `OpdsServerStore` JSON key + 2 `SettingsList` rows + i18n keys + `opds.json` migration note. Skip `src/clippings/` (not adopted) and the KOReader half if scoped down. Note `lib/AsciiText` is absent here — inline a trim helper. | none; conflicts with CPYOS settings/i18n only. | ADOPT_PARTIAL | Take the primitive + OPDS plumbing (store 2 headers per server, obfuscate values like passwords, send on every request). Defer KOReader-global headers and the web-settings-page hunk. Feature work, not a bugfix — schedule deliberately; no device risk beyond one settings JSON key. |
| bb855ef9 | clarify scope for reusable downstream primitives | none for the firmware: 14 lines of policy text in upstream's `SCOPE.md`. | — | — | KEEP_REJECTED | Doc for upstream's contribution policy; CPYOS keeps its own scope doc. |
| fecee610 | Bluetooth page turner for X3/X4 (one module owns BLE) | CPYOS already ships BLE HID page turning: `src/BleInput.{h,cpp}` (start contexts, per-board memory gates, `encodeKey`/`describeKey`), `src/BleKeyMapping.h`, `BluetoothSettingsActivity`, `BleButtonMapActivity`, reader auto-start in `MappedInputManager`/`EpubReaderActivity`. Upstream adds a second, 6.3k-line radio owner (`BlePageTurner` runtime, radio policy, heap-restart, link notes) plus its own settings screen. | 64 files, `freeink-sdk` gitlink bump. | freeink-sdk gitlink + BLE subsystem. | KEEP_REJECTED | A parallel implementation on the same radio; merging means either deleting CPYOS's BLE stack or running two owners. Optional nuggets if BLE drain ever becomes a complaint: idle radio auto-off (`kIdleOffMs = 30 s`) and the measured X3/S3 heap budgets — cherry-pick as small constants, not as a module. |

## Summary

Counts: **ADOPT_NOW 0 · ADOPT_PARTIAL 2 · ADOPT_AFTER_SDK 1 · KEEP_REJECTED 12 · NEEDS_DEVICE_CHECK 0.**

Two prior verdicts were confirmed for a *different* reason than stated (327e7219 is a no-op today, not a breakage;
186d9407/12e441f7 are already in-tree via `7352b33f`). One prior "superseded" was too generous to CPYOS: the
hyphenated line-break path really is missing ruby overhead handling. Everything else is genuinely moot —
unreachable config (fdf5f76c), hardware we do not ship (809c1dab), doc-only (b0aadf2a, bb855ef9, the three
guidelines commits), or duplicates of code CPYOS already has (2c09b8ce, fecee610).

Top 5 worth revisiting
1. **cb2a4c78 — ADOPT_PARTIAL.** Only actionable correctness bug in the slice: hyphenation + ruby/hanging
   punctuation overflows the right margin because the greedy path ignores `calculateRubyExtra{Start,End}Offset`.
   Port ~25 lines into `computeHyphenatedLineBreaks()`, bump `SECTION_FILE_VERSION` 86/87, reuse upstream's
   em-dash fixtures.
2. **65e3f9d0 — ADOPT_PARTIAL.** OPDS (and later KOReader) custom auth headers; additive, unlocks proxied
   catalogs. Scope to `lib/HttpHeader` + `OpdsServerStore` + `HttpDownloader` + one settings row pair.
3. **327e7219 — ADOPT_AFTER_SDK.** One line + one test, but only meaningful once `ProtectedBook` can decrypt
   `aes256-cbc`; bundle the two, never the probe alone.
4. **fdf5f76c — hygiene only.** Delete the dead `utf8IsCjkCodepoint`; the probe/resolver changes add nothing.
5. **0305aaa6 — not a merge, but do not forget the gap.** Drop caps, `white-space`, `font-size` keywords,
   tables and richer CSS are still absent. If CPYOS wants them, re-derive feature-by-feature against
   `ParsedText`/`Page` 86/87 rather than importing the 2.0.0 layout rewrite.


---

## UI-A（home/library/keyboard/layout/nav）

# Bucket 3 — home / library / keyboard / layout / nav / settings / boards (14 items)

Re-evaluated against `work/upstream-sync` @ `dfadf349` (buckets 1+2 merged, SDK still pinned at `96de1be`).
Prior verdicts were treated as claims only; every SHA was re-read from the diff and checked against the current tree.

Evidence anchors used repeatedly:
- `platformio.ini:303-331` — `[c3_hardware]` defines **both** `-DFREEINK_DEVICE_X4=1` and `-DFREEINK_DEVICE_X3=1`,
  and `[env:default]` (`:333`) extends it; `scripts/nightly_targets.py:5-8` ships `xteink_x4` for models
  **`xteink_x3` + `xteink_x4`**. CPYOS therefore *does* ship an X3 image — the "no X3 env" premise is false.
- `freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h:587-597,832` — `BatteryGaugeConfig` has
  `gaugeAddr/i2cSda/i2cScl/i2cHz/chargerAddr` only: **no design-capacity field**, and the comment at `:324`
  states "X3 (gauge) and X4 (ADC) share one C3 binary".
- `freeink-sdk/libs/hardware/BatteryMonitor/{include,src}` — no `loadDesignCapacity()`; `readGaugeSoc()` reads
  BQ27220 `STATE_OF_CHARGE` directly (`BatteryMonitor.cpp:287`).
- `freeink-sdk/libs/hardware/Imu/` — `hasGyroscope()` exists only in `test/host/test_sc7a20h.cpp`; the pinned
  `Imu.h` API has no such method.
- `lib/hal/HalTiltSensor.cpp:62-101,163-200` — CPYOS keeps an explicit `_accelOnly` path (SC7A20H gravity-rate
  tilt + shake gate); `isAvailable()` is the SettingsList gate.
- `freeink-sdk/libs/hardware/BoardConfig/include/BoardConfig.h` — `viewableInsets` exists (`X4 = {9,7,3,7}`,
  `READ_PICO` `{}`), but there is **no `cornerRadius` / `headerOffsetY` / `tabBar*Pct` field**.
- `src/components/UITheme.cpp:172-190` — `getScreenSafeArea()` already applies `getOrientedViewableTRBL()`;
  CPY theme overrides header/status bar (`src/components/themes/cpy/CpyTheme.cpp:917,990,1169`).
- `src/SettingsList.h:239-260,715-733` — CPYOS `entryCount = 93`, `reserve(entryCount)`, erase, then
  rebuild at exact size; `test/settings_api/test_settings_api_keys.py` static_asserts `entryCount` against
  every capability combination.
- `src/activities/settings/SettingsActivity.cpp:625-671,778` — section headers are `ListItem{isHeader}` rows with
  `actionValue = -1`, mapping/tests documented in AGENTS §26.
- `src/activities/settings/KOReaderSettingsActivity.h:12` — `MENU_ITEMS = 8`, no server-type row; repo-wide
  `KOReaderServerType` has 0 hits.
- `src/network/CrossPointWebServer.cpp:389-401` — `/api/plugins`, `/plugin`, `/plugins-run`,
  `/api/plugin-jobs*`, `/api/plugin-fs` are always registered, with **no enable/disable gate**.
- `src/CrossPointSettings.h:319` — `enum UI_THEME { CPY = 0, UI_THEME_COUNT }`: only the CPY theme ships;
  there is no `UiContentSelection` anywhere in the tree.

| SHA | subject | benefit | cost/risk | deps | verdict | rationale |
|---|---|---|---|---|---|---|
| 0a907772 | load X3 battery capacity into its BQ27220 gauge | **Real bug on a board CPYOS ships.** X3 has a BQ27220 gauge (`BoardConfig.h:324`), the C3 binary selects it at runtime, and CPYOS never programs the design capacity — SoC is whatever the gauge's data memory holds. The fix is 7 lines: one `BatteryMonitor::loadDesignCapacity()` attempt under `RenderLock::Mode::Try` in `loop()`, retried until it returns true. Non-gauge boards return false, so it is inert on X4/ReadPico. | 1 file, 7 lines; one static bool; conflicts trivially with CPYOS's `halTiltSensor.update(...)` call above it. | `BatteryMonitor::loadDesignCapacity()` is **absent at pin `96de1be`** → freeink-sdk bump/backport (BQ27220 data-memory write + verify). | ADOPT_AFTER_SDK | The prior "X3-only, no X3 env" reason is false: `platformio.ini` compiles `FREEINK_DEVICE_X3` and the release target covers X3. Port the 7-line hunk after adding the SDK call (a ~20-line BQ27220 Data-Memory write, mirrored from upstream SDK `d9a1304c`), then verify SoC on an X3 (charge/rest) and confirm X4/ReadPico stay on the ADC/PMU path. |
| 43694c90 | show popup for kosync server-type selector | None as written. CPYOS has no KOReader server-type feature at all (`KOReaderServerType` 0 hits; `MENU_ITEMS = 8` with no row 3 server type), so there is no 3-value cycle to replace. The commit is a UX polish for a setting CPYOS never added. | 2 files + freeink-sdk gitlink bump (SDK half is unrelated). | freeink-sdk bump; setting does not exist. | KEEP_REJECTED | Nothing to fix. Revisit only if CPYOS adds the kosync/other server-type setting — then use `OptionPopup` (already the CPYOS pattern for `SettingInfo::Enum`) instead of the upstream `optionPopup` member. |
| c201af43 | dict: multi-token lookup using clippings | Genuine missing capability (phrase/multi-word lookup, cross-page selection), but it is delivered *by replacing* `DictionaryWordSelectActivity` with `ClipSelectionActivity`, which CPYOS does not have. CPYOS already owns a text-selection model (`AnnotationStore`, 650 ms long-press, 64 px slop, §22.6/§33). Porting the upstream shape means introducing a second selection engine. | 3 code files + clippings subsystem; conflicts with CPYOS's own word-select and annotation paths. | upstream clippings (`d7fcc7ce`, rejected); `ClipSelectionActivity` absent in CPYOS. | KEEP_REJECTED | Dependency is not portable. If phrase lookup is wanted, build it **CPYOS-native**: seed `DictionaryDefinitionActivity` from the `AnnotationStore` range selected by long-press/drag (one lookup over `paragraph → text range`), and keep the existing two-step "select then confirm" UX. That is a new feature, not this commit. |
| 8fc2b661 | check for gyroscope presence in tilt sensor init | Negative for CPYOS. Read Pico's SC7A20H has **no gyroscope**, and CPYOS deliberately derives tilt from the gravity derivative plus a shake gate (`HalTiltSensor.cpp:62-101,163+`). Upstream's `_available = _sdkImu.hasGyroscope()` would set `_available = false` on the only target it matters for and delete "Tilt/Shake page turn" from SettingsList (`isAvailable()` gate). | 2 files; would silently remove a shipped feature. | `Imu::hasGyroscope()` **does not exist** at pin `96de1be` (only in the SDK's own host test). | KEEP_REJECTED | Both the API and the semantics are wrong here. CPYOS already solved the underlying concern (accel-only part) explicitly. The only adoptable nugget for a future SDK bump is replacing the `#if FREEINK_DEVICE_READPICO` + `imuType == Sc7a20h` probe with `!hasGyroscope()`, which is cosmetic. |
| 01f09e0e | respect content-area insets and corner radius | ~0 today. It needs `BoardConfig::ACTIVE.cornerRadius` / `headerOffsetY` / `tabBarHeightPct|OffsetY|WidthPct`, none of which exist at pin `96de1be`. CPYOS's only panels are rectangular (`READ_PICO` viewableInsets = 0, X4 = `{9,7,3,7}` already honoured by `getScreenSafeArea`), and CPY theme draws its own header/status bar. The one universal hunk (`props.leadingStyles = plainStyles(solid(Black))`) is cosmetic. | 8 files + freeink-sdk bump; Library/Wifi/Home/CoverGrid hunks collide with CPYOS rewrites (`LibraryListActivity`, Wi-Fi sheet, My/FindBooks). | needs freeink-sdk bump (board profile fields) and a rounded-corner target. | KEEP_REJECTED | No board CPYOS ships has a rounded panel and no target page is missing the safe-area inset. Change if a rounded-corner device is added: then port `BaseTheme::headerBand()` + the status-bar `max(margin, corner)` clearance (and the tab-divider/tab-pct helpers), not the Library/Wifi/Home hunks. |
| 944b741e | clang-format the menu hit-test rect initializer | None — pure re-wrap of a statement. | 1 file, whitespace. | none. | KEEP_REJECTED | Formatter churn on code CPYOS replaced (`HomeActivity` → `InxRecentActivity`/My). A future clang-format-21 pass covers it. |
| e529cadb | global plugin system toggle + plugin hub bootstrap | **Portable half is real and security-relevant**: CPYOS always registers `/api/plugins`, `/plugin`, `/plugins-run`, `/api/plugin-jobs*` and `/api/plugin-fs` (FS read/write from the LAN) with no master switch, and the catalog scans the SD card on entry. A `pluginsEnabled` toggle (default off for new installs, migrated on when plugin dirs already exist) + gating those endpoints and the catalog scan removes an always-on attack surface and the boot/scan cost. The hub bootstrap, install quarantine, `FirmwareIdentity` and `PluginHubInstallActivity` are hub-specific and do not apply. | Toggle: 1 settings field + 1 list row + 2 i18n keys + gates in `CrossPointWebServer` (7 endpoints) + `PluginCatalogActivity`; ~6 files. Risk: users with existing plugins must keep working — copy upstream's migration rule verbatim. | none (no SDK change). Skip the hub/quarantine/FirmwareIdentity hunks. | ADOPT_PARTIAL | Take the master switch + endpoint/catalog gating only. Small, additive, and closes an endpoint surface CPYOS opened wider than upstream (plugin FS API). The hub is a product decision, not needed. |
| 87fc4ab7 | settings grouping (hub + per-group sub-activities) | CPYOS already has grouped settings: section header rows inside one `SettingsActivity` (`isHeader`, `actionValue=-1`), focus skips headers, and `test/settings_row_mapping.py` pins the row→setting mapping. Upstream's version replaces the tabbed page with a `SettingsListActivity` hub and a filtered `SettingsActivity` — a navigation redesign, not a fix. | `SettingsActivity.{h,cpp}`, `SettingsList.h`, `ActivityManager`, list icons; would re-break CPYOS's row mapping, icons test and §26 fixes. | none. | KEEP_REJECTED | Same feature, different design, and CPYOS's variant is already verified on device. Change only as a deliberate UX decision (hub vs. headers); then re-derive, do not merge. |
| ec32b959 | share actions and consolidate page selection | The bug class is INX-theme-only: `defersContentSelection()` is defined as "touch && `SETTINGS.uiTheme == INX` && usesMainTabBar" (`Activity.cpp`), and the payload is a new `UiContentSelection` framework. CPYOS ships only `UI_THEME { CPY=0 }` and has zero `UiContentSelection`/`focusMainTabContent` hits, so nothing applies. Shared-action refactors target CPYOS-rewritten pages (`InxRecent`, `ReadingStats`, `About`, `AppsMenu` deleted). | 28 files + 326 lines of tests; requires the retired INX theme and a selection framework. | INX theme + `UiContentSelection` (absent). | KEEP_REJECTED | Cannot apply without resurrecting INX. If the CPY main-tab bar ever grows a "focus the page first" model, the two real UX fixes are worth re-reading: don't swallow the first Confirm, and fold the focus repaint into the release frame (`mainTabTouchFocusPending`). Not now. |
| d52fc820 | remove unused rectangle touch helper | Negative: `wasTapInRect` is used by 12 activities (2048, avatar, chess, gomoku, minesweeper, sokoban, sudoku, reading-stats ×2, `FullScreenMessageActivity`, `ImageViewerActivity`) — ~24 call sites. | 3 files; deleting it breaks the build. | none. | KEEP_REJECTED | Upstream-only cleanup. Would only become applicable if CPYOS migrated every game/viewer to the FreeInkApp hit-rect model. |
| c7996b9f | reserve capacity for the page bottom-align entry | No merge value: the fix is "make `entryCount` match the table". CPYOS has its own table (`entryCount = 93`, capability addends), rebuilds the vector at exact size after erasing, and `test/settings_api/test_settings_api_keys.py` static_asserts the count for every capability combination. | 1 line, upstream numbers. | none. | KEEP_REJECTED | Same guarantee already enforced structurally and by a stronger test. **Actionable note:** AGENTS §40.3 records that CPYOS's own `test_settings_api_keys.py` currently fails on "a 5-item insertion batch" — that is this bug class landing on CPYOS's table. Fix it CPYOS-side (recut the batches / bump `entryCount`), do not take upstream's numbers. |
| 261726f6 | BOOX Picco device | No Picco target, and every meaningful hunk is Picco hardware: SGM41562 charger config, TMA525C touch sleep, `holdWhileUsbPowered` light-sleep loop, `SlideSwitch`, `partitions_picco.csv`, frontlight/RTC wiring. `startDeepSleep(..., wakeOnHeldButton)` is only reachable from Picco's main path. | 26 files + freeink-sdk bump (new board, charger, touch). | Picco hardware, SDK board profile, CI workflows. | KEEP_REJECTED | Hardware CPYOS does not ship and cannot test. Revisit only if a Picco target is added; the only possibly reusable idea for Read Pico is `wakeOnHeldButton`, which its PMU hand-off already covers differently. |
| 31480212 | clang-format 21 cleanups in tap-zone settings | None — whitespace in `TapZoneSettingsActivity.cpp`. | 1 file. | none. | KEEP_REJECTED | Formatter-only; a clang-format-21 pass on CPYOS (when installed) covers it. |
| 35dccb87 | split the tap-zone batch and reserve its capacity | No merge value, same class as `c7996b9f`: upstream splits one `insert` batch so it fits the reserved capacity. CPYOS's table has different composition and its API test asserts the exact count; it is not affected by upstream's batch boundaries. | 1 file. | none. | KEEP_REJECTED | Bookkeeping for upstream's numbers. If CPYOS's own `entryCount` assertion is currently red (§40.3), split CPYOS's batches / recount there instead. |

## Summary

**Counts: ADOPT_NOW 0 · ADOPT_PARTIAL 1 · ADOPT_AFTER_SDK 1 · NEEDS_DEVICE_CHECK 0 · KEEP_REJECTED 12 (14 total).**

Top 5 worth revisiting:
1. **`0a907772` X3 battery gauge (ADOPT_AFTER_SDK)** — genuine wrong-SoC bug on a board CPYOS ships; the earlier "no X3 env" reason is factually wrong (`c3_hardware` compiles `FREEINK_DEVICE_X3`; `nightly_targets.py` lists X3 under `xteink_x4`). Blocked solely on `BatteryMonitor::loadDesignCapacity()`.
2. **`e529cadb` plugin master switch (ADOPT_PARTIAL)** — CPYOS exposes plugin FS/job endpoints on the LAN unconditionally; the toggle + endpoint gating is ~6 files of security hardening. Skip the hub.
3. **`c201af43` phrase lookup** — do not port the clippings dependency; implement CPYOS-native on top of `AnnotationStore` range selection.
4. **`c7996b9f` / `35dccb87` capacity class** — nothing to merge, but CPYOS's own `test/settings_api_keys.py` is reported red on a 5-item batch; fix CPYOS's `entryCount`/batch split.
5. **`01f09e0e` corner-radius insets** — only if CPYOS ever ships a rounded-corner panel (then port `headerBand()` + status-bar clearance after an SDK bump); zero benefit on readpico/X3/X4 today.

Reasons that were re-verified as sound: `8fc2b661` (would delete the accel-only tilt/shake feature and needs an API absent from the pin), `d52fc820` (~24 live call sites), `87fc4ab7`/`ec32b959` (CPYOS design supersedes; INX framework absent), `261726f6` (no Picco target), the two clang-format commits, and `43694c90` (setting does not exist).


---

## UI-B（reader menu/tap zones/popup/sleep/clippings/BLE）

# Bucket 3 — UI-B re-evaluation (upstream `a6fd569f`, CPYOS `dfadf349`)

Prior "D_SKIP" verdicts were re-tested against the actual CPYOS code, not trusted.

| SHA | subject | benefit | cost/risk | deps | verdict | rationale |
|---|---|---|---|---|---|---|
| e9635421 | reserve capacity for the tap-zone row | none here | none | — | KEEP_REJECTED | CPYOS has its own count (`SettingsList.h:239` = 93 + caps) and an API test that asserts it (`:723`). Nothing to port; the invariant (bump count in the same commit as a base row) already holds. |
| cac12269 | incomplete-type owner lifecycles out of line | compile hygiene only | tiny | — | KEEP_REJECTED | `ActivityManager.h:95-101` still inlines ctor/dtor and every env builds; the `Activity` fwd-decl + `unique_ptr` member is only ill-formed in a TU that destroys the manager without `Activity.h`, and none exists. Route if it ever breaks: move both to `.cpp`, include `Activity.h` there (drop the `onSelectBook` rename — CPYOS's HomeActivity is unrelated). |
| b823d54b | page-turn DynamicEnum owns its value | none | — | upstream `touchControlsVersion` | KEEP_REJECTED | The real fix (getter/setter branch **before** `valuePtr`) is already in CPYOS (`SettingsActivity.cpp:928` before `:534`; `settingValueText` at `:1348` before `:1338`). CPYOS lists 5 labels including DISABLED, so stored 4 is not out of range; it never writes `touchControlsVersion`. |
| e41982a4 | keep mixed gesture pairs intact | none | — | same scheme | KEEP_REJECTED | CPYOS expands the legacy mode only when **both** gesture keys are absent (`CrossPointSettings.cpp:568-577`) and never rewrites `previousPageGesture`; INVERTED_TAP is honoured at read time in `detectTouchPageTurn`, so the zone swap the commit adds is not needed. |
| ad24aea8 | delete clipping popup blinks | none | — | upstream clippings | KEEP_REJECTED | No `ClipSelection/EpubReaderClipping*` in `src/activities/reader`; CPYOS uses AnnotationStore + InlineConfirm. |
| 471243b2 | hide long-press menu on Home-key/Picco boards | latent only | product regression if taken as-is | `BoardConfig::hasTouch()` | KEEP_REJECTED | `if (hasTouch()) return 0` would disable long-press Confirm on **readpico** (touch), where `confirmLongPressThreshold()` (`EpubReaderActivity.cpp:3183`) drives bookmark/dict/menu and the row is deliberately visible. No shipped CPYOS board sets `hasHomeKey` (X3/X4/readpico all false), so the stale-value defect is unreachable. Flip only if a home-key board is added — and then use `hasHomeKey()`, not `hasTouch()`. |
| 5407ffcd | consolidate touch settings filtering | none | product regression | SDK bump | KEEP_REJECTED | Moves the filter from `hasHomeKey` to `hasTouch`, i.e. removes a working readpico feature; CPYOS's filter (`SettingsActivity.cpp:472`) is the correct one for its board set. Bundled gitlink bump is unwanted. |
| 88ea9bcf | 1.1/1.2 line spacing for `.cpfont` | subjective typography | 1-line | — | KEEP_REJECTED | Reverts upstream's own 1.3/1.6 to 1.1/1.2; CPYOS chose 1.3/1.6 deliberately (`getReaderLineCompression`, `CrossPointSettings.cpp:958`) and CJK bodies read better airy. Flip if users report wide/extra-wide as too loose on an SD `.cpfont`. |
| fb2a8a8f | new Picco & Sticky displays | none for shipped boards | high | freeink-sdk gitlink bump; `board.statusBarOffsetY/headerOffsetY/bottomCornerRadius` absent | KEEP_REJECTED | 4 of 10 hunks need the new SDK/board fields. ★ Its `Frontlight.present()` gate is a **trap**: CPYOS uses `FrontlightPanelActivity` as the control center on touch boards **without** a frontlight (`MappedInputManager.cpp:535` says so), so the gate would remove the control center from readpico. `BwFrameKeeper` no-ops where absolute gray is supported (both CPYOS boards). |
| 8b4c0d6e | unify INX header status placement | none | high | INX theme | KEEP_REJECTED | `src/components/themes/` holds only `cpy` and `upstream`; the `inx` theme is deleted (`OptionPopup.h:32` "INX is retired") and `UI_THEME` has a single value. Dead code path. |
| b2b1e15a | icons instead of text labels in FrontlightPanel | visual preference | medium | `readerToolbarIcons` | KEEP_REJECTED | CPYOS's sheet is its own labelled-tile grid (deliberate, §27/V-series); no defect being fixed, and the icon row is the base b2b1e15a's successor feature builds on. |
| 0727f9f0 | Bluetooth quick toggle | **real bug fix hidden in it** | low (2 hunks) | — | **ADOPT_PARTIAL** | "Refresh screen works only in Home, not in Reader": CPYOS drops the promotion on every grayscale present. `displayGrayscaleBase` (`GfxRenderer.cpp:4261/4271`) ignores `nextRefreshOverridePending`, and `commitGrayscale16` (`:2763`) only clears the from-gray variant then re-arms from-gray — so a `promoteNextRefresh(FULL)` from the REFRESH tap-zone (`ReaderActivity.cpp:324/346`) or control-center ghost clean (`FrontlightPanelActivity.cpp:170`) is silently dropped on 16-level text / image pages. Port exactly the `GfxRenderer` hunk (both `displayGrayscaleBase` overloads) **plus** `commitGrayscale16`: `clean |= pending && !fromGray`, clear pending. Reject the BLE toggle (CPYOS has `BluetoothSettingsActivity`, no `bleturner`) and the `wasLightPanelGesture` un-gating (already done). Needs one device check: AA page + REFRESH zone must now flash one GC16. |
| 3f8345c5 | toolbar padding with roundedslides theme | none | low | — | KEEP_REJECTED | CPYOS ships only `UI_THEME::CPY`; under CPY layout the row is never inverted and the cursor is a thin bar outside the inset, so `sidePadding = 0` (`ReaderToolbarUi.cpp:747`) is intentional and correct. Applies only if a selection-shape theme is revived. |
| 8f4d035c | Picco switch as hardware setting | none | — | `SlideSwitch.h` (absent) | KEEP_REJECTED | Picco-only board; `src/util/SlideSwitch.h` does not exist. The generic bits (`HalFrontlight::begin` brightness-before-warmth; `withWebHidden`) are moot: CPYOS boards have no frontlight driver path, and there is no setting-list-driven web API to hide the raw clock/timezone indexes from. |
| 6b863e48 | hide dict button when no dict available | small UX | small | upstream clippings | **ADOPT_PARTIAL** | The clippings row is gone, and CPYOS already hides the menu row when the name is empty (`EpubReaderMenuActivity.cpp:103`). Residual real gap: an empty check misses a **deleted/unresolvable** dictionary — the row is offered, then `openDictionaryWordSelect` toasts `STR_DICT_NO_DICT_SET` (`EpubReaderActivity.cpp:839`). 2-line port: gate both sites on `DictionaryRegistry::resolveBasePath(SETTINGS.dictionaryName, …)`. |
| 4ef42ee1 | skip debug-only diagnostics in release builds | none | — | — | KEEP_REJECTED | The two guarded blocks compute `HalMemory::getInternalHeap()`; **neither file contains it** (0 matches). CPYOS's remaining `LOG_DBG("LIB"/"DICT", …)` blocks only format existing fields, and all envs build warning-free. |

## Summary

Counts: **0 ADOPT_NOW, 2 ADOPT_PARTIAL, 0 ADOPT_AFTER_SDK, 14 KEEP_REJECTED, 0 NEEDS_DEVICE_CHECK** (1 partial needs one device check).

Top 5:
1. **0727f9f0 (ADOPT_PARTIAL)** — only real bug in the slice. "Refresh screen" is a no-op while reading anti-aliased text or an image page: the one-shot FULL promotion is never consumed by `displayGrayscaleBase`/`commitGrayscale16`. ~6 lines in `GfxRenderer`; verify with the REFRESH tap zone on a 16-level page.
2. **6b863e48 (ADOPT_PARTIAL)** — swap the empty-string dictionary guard for `DictionaryRegistry::resolveBasePath` at `EpubReaderMenuActivity.cpp:103` + `EpubReaderActivity.cpp:840`; stops a Lookup row that can only toast.
3. **fb2a8a8f is a trap, not a skip** — its `Frontlight.present()` gate would delete CPYOS's control center on exactly the boards it ships, because CPYOS deliberately reuses the panel for frontlight-less touch devices. Record that in the merge log so a future "board support" cherry-pick does not take the hunk blindly.
4. **471243b2** — the prior "apply only with 5407ffcd" is the wrong condition; the correct narrow guard is `hasHomeKey()`, and no shipped board sets it, so skip now and revisit on board expansion.
5. **Everything else is genuinely inapplicable** — clippings (`ad24aea8`, part of `6b863e48`), deleted INX theme (`8b4c0d6e`), absent boards/SDK fields (`fb2a8a8f`, `8f4d035c`), or already fixed in CPYOS with a different scheme (`b823d54b`, `e41982a4`).


---

## fonts/i18n

# Bucket 3 — fonts / i18n re-audit (b3-fonts.tsv, 13 items)

Repo read-only: `.worktrees/upstream-sync` @ `dfadf349`, upstream `a6fd569f`.
Every verdict below was re-derived from `git show <sha>` + the CPYOS tree in that worktree.
Evidence commands are inlined so the next reader can re-run them.

## Hard evidence gathered first (used by several rows)

- `assets/fonts/CPYOSHanSansSC-Regular-8105.ttf` cmap = **9,031** codepoints, and it **contains**
  U+0410 А, U+0401 Ё, U+2116 №, U+00B7 ·, U+203A ›; it does **not** contain U+018F Ə / U+0259 ə.
- `lib/I18n/translations/` = 35 files, **no azerbaijani.yaml**.
- `lib/EpdFont/builtinFonts/all.h` includes only `chinese_chess_16`, `notosans_8`,
  `notosans_cjk_8` (+`cjk_10/12` when no pack), `ubuntu_10/12`. 17 `notoserif_*` headers are on disk.
- Shipped targets compile **no** built-in 12/14/16/18 pt Latin/CJK face: `main.cpp:337-360` and
  `all.h` gate them behind `CROSSPOINT_SYSTEM_FONT_PACK` / `CROSSPOINT_EXTERNAL_FONTS`; the reader
  face is `externalCjk12Data` filled from the pack.
- `SdCardFont.cpp:1934 buildAdvanceTablePacked()` uses `resolveStyleMask()` / `hasStyle(si)` /
  `styleData(si)`; no `styles_[si]` per-style index anywhere (grep `styles_\[`).
- `grep -rn "variantFontId|loadFamilyVariant|cssVariant"` → **no hits**: CPYOS has no CSS size-variant
  font path, so the double-load `2648933a` fixes cannot happen here.
- `UiHighDpiProfile::reader12FontId = 0x4738000C` (hardcoded) is used in `CrossPointSettings.cpp:1062`
  and `main.cpp:921`; pagination invalidation instead rides `SECTION_FILE_VERSION` (86/87) plus the
  pack's own `system_font_<crc>.dat` name.
- SD font catalog is fetched from `CrossMuxEndpoints::FONT_MANIFEST_FORMAT`
  = `https://%s/api/assets/fonts/.../fonts.json` (host = crossmux.com/.cn), via
  `FontDownloadActivity.cpp:357`. CPYOS publishes firmware/Pages/FontExtras, **not** this catalog.

## Table

| SHA | subject | benefit | cost/risk | deps | verdict | rationale |
|---|---|---|---|---|---|---|
| 40b313a3 | Azerbaijani translation + Ə/ə glyph support | Ə/ə is a **real** gap in the system pack (cmap probe: absent); az is not shipped as a locale | 38 of 40 files are regenerated `builtinFonts/*`; none of those faces is compiled on readpico (pack) and only 8 pt on C3 → pure churn + content-hashed UI ids move for no visible gain. `azerbaijani.yaml` would render Ə as tofu | system-pack `cmap` (TTF + `cpyos_han_sans.def` + C3 `system_font.dat`), `test_system_font_pack` | KEEP_REJECTED | Only the 2-glyph idea is valid, and its correct route is the same one §55.3 used for `·/«/»`: add U+018F/U+0259 to `CPYOSHanSansSC` and rebuild the `.def`, *not* 38 regenerated headers. Flip: if az (or any Latin-Ext-B locale) is added → add those two codepoints to the pack first, then take the yaml only. |
| d7fcc7ce | clip/highlight EPUB text | none left to gain | 29-file conflict against a rewritten parser; needs per-word source ranges | `AnnotationStore` already ships | KEEP_REJECTED | CPYOS has `lib/Epub/Epub/AnnotationStore.{h,cpp}` with its own UI; upstream's version is anchored on `ParsedText` source ranges that §40.2 rejected for size (≈8 B/word, doubled for CJK tokens). Nothing to port. |
| 965722cf | offer Libron in Manage Fonts | 1 new `.cpfont` family in the SD downloader | 11-line YAML only; inert until a catalog is built **and hosted** | `sd-fonts.yaml` → `generate-font-manifest.py` → `build-sd-fonts.py`; catalog host | KEEP_REJECTED | The device reads its catalog from `CrossMuxEndpoints::FONT_MANIFEST_FORMAT` (crossmux host). A local `sd-fonts.yaml` entry reaches no user unless CPYOS starts publishing that JSON itself; CPYOS's own extra-font channel is FEXT1 (`font-extras/packs.json` → Pages). Flip: only if CPYOS hosts its own `fonts.json` — then this is a 11-line ADOPT_NOW. |
| 1899de8f | Cyrillic in built-in subsets | claims to fix Russian/Ukrainian/Bulgarian EPUBs rendering *nothing* | script-only (2 lines) → **zero runtime effect** until the 6 CJK headers are regenerated, and regenerating them reverts CPYOS's calibrated tiers (script defaults: 3500 chars @8/10/12, i18n-only @14/16/18, vs CPYOS's 6879/803 split) | `build-cn-builtin-fonts.sh`, exact source OTF | KEEP_REJECTED | The stated bug does not exist here: the reader face comes from the system pack (`main.cpp:355`, `CROSSPOINT_SYSTEM_FONT_PACK`), and the pack has А/Ё/№ (cmap probe). Taking the 2 lines would look like a fix while changing nothing. Flip: only as part of a deliberate, calibrated header regeneration. |
| ce781eb8 | prewarm visible CJK glyphs before list painting | removes a first-paint glyph-decompress stall | bumps the `freeink-sdk` gitlink (CPYOS pins patched `96de1be`) + rewrites `UiListActivity::render` (16-level canvas, band updates) | SDK `ScanMode::UiCjk`, `UiAppHost` scan-before-paint | KEEP_REJECTED | The stall it fixes is `FontDecompressor` group decompress on built-in compressed faces. On both shipped targets `typography::apply()` rebinds every UI role to the vector pack (`SystemFontPack`), so that path is unreachable except the 8 pt emergency fallback. Idea only. Flip: a measured first-list-paint stall on device with the pack absent. |
| fc2de152 | move NotoSerif to optional SD font | upstream flash saving | upstream's 18 headers are already **dead** here (0 flash saved); but `build-font-ids.sh:19-28` hashes them into `NOTOSERIF_12..18_FONT_ID`, and `CrossPointSettings.cpp:1073-1080` still returns those ids | `all.h`, `build-font-ids.sh`, `CrossPointSettings` | KEEP_REJECTED | Deleting the headers without upstream's settings rework **breaks id generation**; the ids have no registered face anyway (see side finding). Hygiene only, negative flash delta, non-zero risk. Flip: only if we drop the `NOTOSERIF` reader-family option first. |
| 7242d9ae | generate font IDs with Python | drops the ruby dependency | rewrites the generator for **upstream's** id set only | `build-font-ids.sh` | KEEP_REJECTED | CPYOS's `fontIds.h` carries macros upstream's porter does not emit (`CJK_UI_18_FONT_ID`, `CJK_READER_SIZE_FONT_ID(pt)`, the `CJK_READER_SIZE` static_asserts). Adopting it would silently drop them; a Python port is only worth doing as our own rewrite. |
| 3f0f4fad | ship Cyrillic + refresh pagination with font data | (a) Cyrillic: already covered; (b) content-hashed reader id instead of `reader12FontId = 0x4738000C` | regenerates 6 CJK headers (≈110 k lines) + `share-cn-font-intervals`; reworks `publish/build-font-ids`; would key reader ids on `notosans_cjk_12.h`, which is **not compiled** on readpico/C3 (pack supplies the face) | header regen pipeline, `UiHighDpiProfile`, `fontIds.h`, `main.cpp` | KEEP_REJECTED | (a) is a no-op here (pack has А/Ё/№). (b) is real but wrong-tooled: the live reader face is the pack, and CPYOS already invalidates with `SECTION_FILE_VERSION` 86/87 per font change (§43.2/§45.2), which is the same guarantee without hashing an uncompiled header. Flip: if pack font files ever change silently and a stale-pagination bug is observed. |
| cf50ce73 | Ubuntu for the small UI font | one fewer face on low-DPI | churns `all.h`, `fontIds.h`, `main.cpp`, deletes `notosans_8_regular.h` | `CROSSMUX_UI_PROFILE_HIGH_DPI` | KEEP_REJECTED | `main.cpp:407-411`: `SMALL` = `notosans_12` on high-DPI, `notosans_8` only otherwise; `UI_10/UI_12` are already Ubuntu. On the pack both are rebound to pack size faces, so this is invisible to users. |
| 2648933a | reuse loaded UI font for CSS variants | +2.9 KB heap on X3 | touches `SdCardFontManager`/`SdCardFontSystem` | per-CSS-size variant path | KEEP_REJECTED | That path does not exist in CPYOS (grep for `variantFontId`/`loadFamilyVariant` → 0 hits); `loadFamilyExtraSize` already reuses an already-loaded size (`SdCardFontManager.cpp:104-108`), which is the whole local benefit. Zero effect, and it rewrites the manager we patched. Flip: if a CSS-size variant path is introduced. |
| 98da1782 | compacted style API in advance-table scan | fixes a wrong-style read created by `b912e000` | 2 lines | requires `b912e000` (not in slice) | KEEP_REJECTED | `buildAdvanceTablePacked()` already uses `resolveStyleMask()` + `hasStyle(si)` + `styleData(si)` (`SdCardFont.cpp:1934-1975`); there is no `styles_[si]` per-style indexing to repair. Cherry-picking it ahead of `b912e000` is actively wrong. |
| c6a2e311 | build fix + regression test after #3948/#4114 | none | test-only | `83df537d`/`b912e000` | KEEP_REJECTED | It adds `AdvancesBuildForAStyleStoredInAnotherSlot` for the `98da1782` bug — the bug is absent locally, so the test would assert behaviour CPYOS already has under a different implementation. |
| 072b6fc7 | line spacing as % of font size | finer spacing control (7 steps) | 19 files / 31 hunks; **rebuilds every paginated section**; removes the per-family multipliers; collides with CPYOS's vertical-text and `PageBottomAlign` paths | `ReaderRenderSpec`, `ChapterHtmlSlimParser`, `GfxRenderer`, settings+YAML | KEEP_REJECTED | `CrossPointSettings::getReaderLineCompression()` (`:935-980`) is a deliberate 4-level, per-family design (0.90–1.05 sans, 0.95–1.2 serif, 0.95–1.6 SD/vector); §40.2 already rejected upstream's retune. Only the "line height from font size" idea transfers, and it needs device comparison. Flip: continuous-spacing requests + a device pass on vertical text. |

## Summary

**Counts:** ADOPT_NOW 0 · ADOPT_PARTIAL 0 · ADOPT_AFTER_SDK 0 · NEEDS_DEVICE_CHECK 0 · **KEEP_REJECTED 13**.

All thirteen prior rejections survive independent re-derivation, but three rationales were **strengthened or corrected**:

1. **1899de8f / 3f0f4fad (Cyrillic)** — prior text said "Cyrillic is already in the system pack" only by reference; **verified**: the pack cmap has U+0410/0401/2116, and readpico compiles no built-in 12/14/16/18 pt face at all (`main.cpp:355`, `all.h`). The upstream bug ("renders nothing") is structurally impossible here.
2. **fc2de152** — prior text called the 18 `notoserif_*` headers "dead files, delete is optional hygiene". True for compilation, but they are **inputs to `build-font-ids.sh`** and their ids are still returned by `getReaderFontId()`; deleting them alone would break id generation. Stronger reason to reject, not weaker.
3. **965722cf** — prior text called it "harmless but buys nothing until a manifest is regenerated". Sharper: the manifest the device reads is **hosted by CrossMux** (`CrossMuxEndpoints::FONT_MANIFEST_FORMAT`), so the entry buys nothing at all unless CPYOS starts publishing that JSON.

**Top 5 (if any budget ever appears):**
1. **3f0f4fad (b)** — the only real defect-adjacent idea: a hardcoded reader id means pack font changes rely on a manual `SECTION_FILE_VERSION` bump. Cheap mitigation without the header regen: derive the reader id from the pack's content CRC (which already names `system_font_<crc>.dat`) instead of hashing an uncompiled header.
2. **40b313a3 (glyphs only)** — add U+018F/U+0259 to `CPYOSHanSansSC` (2 cmap entries + rebuild `.def`), same shape as the §55.3 `·/«/»` fix; do **not** take the 38 regenerated headers.
3. **ce781eb8** — worth remembering only if a list-paint stall is measured with the pack missing.
4. **1899de8f** — free but inert; take only inside a deliberate, calibrated header regeneration.
5. **072b6fc7** — the "line height from font size" fragment is the only transferable piece.

## Side finding (out of slice, worth a ticket)

`CrossPointSettings::getReaderFontId()` still maps `fontFamily == NOTOSERIF` to `NOTOSERIF_12/14/16/18_FONT_ID`, but **no such face is registered**: `all.h` and `main.cpp` include only `notoserif_title_23`, and nothing calls `insertFont()` with those ids. So selecting the Serif reader family resolves to an id with no face. This is why `fc2de152` cannot be taken piecemeal — the setting has to be removed or re-pointed to an SD family first.


---

## network/plugins + misc

# Bucket 3 — NETWORK / PLUGINS / MISC (17 commits, re-derived)

Worktree: `.worktrees/upstream-sync` @ `dfadf349`. Upstream `a6fd569f`. Read-only; nothing applied.
Every prior "do not merge" reason was re-checked against the code in this tree.

Legend: **ADOPT_PARTIAL** = a separable hunk/idea is worth taking, the rest is not · **KEEP_REJECTED**
= nothing to take now (+ the concrete flip condition) · **NEEDS_DEVICE_CHECK** = real concern, only
measurable on hardware.

| SHA | subject | benefit | cost/risk | deps | verdict | rationale |
|---|---|---|---|---|---|---|
| 5789260c | style: refresh device web UI (#3950) | Low (UX consistency) | ~10 lines, no asset change | none | **ADOPT_PARTIAL** | Take only the `handleGetSettings` hunk: emit `"type":"toggle"` for a 2-label `OFF/ON` ENUM (`CrossPointWebServer.cpp:1772-1790`, needs `s.enumStringValues.empty() && labels.size()==2 && labels[0]==STR_STATE_OFF && labels[1]==STR_STATE_ON` + `if (toggle) break;`). Verified: `en|zh SettingsPage.html:343/383/423/437` already render `type==='toggle'`, and `handlePostSettings` dispatches by setting type, so a toggle rendered from an ENUM still POSTs 0/1 correctly. Exactly **one** CPYOS setting matches today (`STR_SEND_METADATA`, `SettingsList.h:646`) — small but free. **Reject the rest**: 1 067-line restyle + `assets/web.css` + 3 new routes; CPYOS has `en/`+`zh-CN/` pages packed as CWP1, so this is a full page rewrite plus new `AssetId`s, pack rebuild and browser re-verification for zero functional gain. |
| a722472c | Reset publication object using uniform initialization | none | — | f2120a3f detail page | **KEEP_REJECTED** | `OpdsPublication` / `openPublicationDetail()` do not exist in this tree (`grep` 0 hits; `src/activities/browser/OpdsBookBrowserActivity.*` has no publication detail path). The hunk *is* a real shadowing bug (`OpdsPublication publication;` re-declared inside the `if`), but there is nothing to fix. Flip: only if the OPDS item-detail page is ported. |
| 58d9c776 | refactor: dialogs, consistent prompts, polish js | Low (2nd toast no longer cut short) | 3 lines × 2 files | none | **ADOPT_PARTIAL** | Take the toast-timer fix: `en/SettingsPage.html:332-337` and `zh-CN/SettingsPage.html:318-323` call `setTimeout(...4000)` with **no handle**, so a second message inside 4 s is hidden by the first message's timer. Upstream's shared `showWebToast` (`Footer.html.inc`) differs only by `let webToastTimeout; clearTimeout(...)`. Everything else (native `<dialog>` rewrite of 6 modals, `web.css`, footer split, `alert()/confirm()` replacement) is per-page UI for upstream's layout; CPYOS pages are restyled and already have their own modals. |
| 919ec426 | docs(weread): correct reading-duration claims | Medium (docs assert a feature that does not exist) | 2 doc files | none | **ADOPT_PARTIAL** (docs) | Verified in code: the WeRead progress body carries only `pr`, `psvts`, `pclts`, `ts`, `rn` (`WeReadClient.cpp:1392-1453`) — **no duration field anywhere** (`grep Duration/duration` in `activities/apps/weread` = 0). Yet `weread/README.md:15,48` and `docs/engineering/chinese-build.md:186-190` promise whole-second reading-time uploads. Apply the wording correction; drop upstream's `#332` link (different tracker). |
| 55a77c2e | fix: web UI dropdown chevron inset (#4090) | none in CPYOS | — | `html/assets/web.css` | **KEEP_REJECTED** | Target file absent. CPYOS keeps native `appearance` on `select` (`en/SettingsPage.html:95-110`: `padding:6px 10px; min-width:160px`, no `appearance:none`), so the flush-chevron artefact this patches cannot occur. Flip: a user reports the arrow touching the border — then add the `appearance:none` + 36 px padding rule to the page's inline CSS. |
| e93f8d1f | feat: combine fixed and dynamic plugin lists (#4198) | none now | — | remote list index (`browse.lists_url`) | **KEEP_REJECTED** | Needs `listIndexLoaded` / `fixedListCount` on top of `loadBrowseListIndex()`, none of which exist: CPYOS has only manifest-declared `browse.lists` + `wantsListPicker()` (`PluginCatalogActivity.cpp:200,457`), and `grep lists_url` = 0 hits in `src/` and `docs/`. Flip: after the dynamic list-index feature is ported (it is not in this slice). |
| d5d0bea1 | feat: headers and mapping for plugin list index (#4197) | none now | — | same as above | **KEEP_REJECTED** | The diff *edits* `loadBrowseListIndex(int&)`, which does not exist in CPYOS. Separate finding worth acting on independently: its `MAX_BROWSE_LISTS = 64` bound exists because an unbounded `reserve()` aborts with `-fno-exceptions`; CPYOS's `manifest.browseLists.reserve(browse["lists"].size())` (`:200`) is still unbounded against a hostile `plugin.json`. Fix that locally (clamp to 64) rather than porting the commit. |
| 861683cc | feat(plugin): filter catalog lists by declared capabilities (#4260) | none now | — | `FirmwareIdentity.h` + list index | **KEEP_REJECTED** | Needs `firmwareidentity::PLUGIN_CAPABILITIES`: CPYOS has no `src/util/FirmwareIdentity.h`, no `PLUGIN_API_VERSION`/`PLUGIN_CAPABILITIES` (grep 0), and no `matchesCapabilities()` — the whole capability vocabulary is absent, so a filter would advertise a scheme no CPYOS plugin can declare. Flip: with the capability API, then this commit (it also needs the list index). |
| 057e16a4 | perf: minify web UI, load EPUB optimizer on demand (23 KB) | Medium (File Manager download 84 KB → ~29 KB gzip; flash) | vendored minifiers + browser smoke test | pack rebuild (automatic) | **ADOPT_PARTIAL** | Measured on this tree: `en/FilesPage.html` = **55 366 B gzip**, `jszip.min.js` = **28 379 B gzip** ⇒ every File Manager visit pulls ~84 KB, exactly upstream's "before". `scripts/web_assets.py:minify_html()` only strips HTML comments/whitespace and *assumes* JS is pre-minified (`:141`), so CPYOS's 175 KB inline script ships unminified. Step 1 (cheap, separable): vendor `rjsmin`/`rcssmin` (Apache-2.0) and minify inline `<script>`/`<style>` in `asset_payload()`, with upstream's `obj.in / 2` guard; cuts both FilesPage assets. Step 2 (later): move the optimizer to its own asset/route and load JSZip on demand — same 84→29 KB win but a re-implementation inside CPYOS's restyled page (the optimizer reads the page's top-level scope, upstream documents that coupling as the main risk). Needs a browser pass over upload/convert/rename-from-metadata. |
| c9f64d0d | feat: move plugin settings to a Plugins web page, consolidate styles (#4272) | Low | new page + route + pack asset + i18n | `web.css`/`Header.html.inc` absent | **KEEP_REJECTED** (fold into 057e16a4) | Pure relocation/CSS consolidation for upstream's layout: CPYOS mounts plugins inline (`#plugin-container`) and has no `web.css`/`Header.html.inc`. One salvageable idea inside it: serve `shared/PluginHost.js.inc` (8 837 B) once instead of inlining it into 5 documents (en/zh Settings, en/zh Files, Runner) — but the gzip saving is ~2-3 KB of a 160 KB pack and it only pays off together with 057e16a4's shared-host split. Take it there, not on its own. |
| f3ace194 | test: trim redundant cover cache cases | none | — | — | **KEEP_REJECTED** | Test-only consolidation of `ensureThumbnail`/`checkCachedCover` cases, including the `RegeneratesHeaderOnlyThumbnail` case this commit deletes. CPYOS's loader has diverged (Gray8 `_g8` thumbnails, 10-slot PSRAM byte cache, `thumbPathFor` memo), so trimming would remove coverage of code CPYOS changed, not duplicates. |
| d5b6c74d | fix: keep the BLE radio off in a book when no remote is chosen (#4074) | none directly | — | `lib/BlePageTurner` absent | **NEEDS_DEVICE_CHECK** (CPYOS analogue) | The module does not exist (CPYOS: SDK `BleKeyboardHost` + `src/BleInput`). But the same question is live: `main.cpp:315-324` calls `bleinput::ensureStarted(..., Reader)` whenever the reader is foreground and BLE is enabled — **no bonded/known-remote gate**, so the radio advertises in every book. Upstream's answer is `Why::NoBond` in `radioVerdict()`. Restating that idea for CPYOS (only auto-start in the reader if a bond exists, or gate on the Bluetooth settings page) is a design change whose cost is power draw, i.e. only measurable on battery. |
| 052d481a | test(tapzone): pin DISABLED to a stored 4 | none (covered) | — | `test/touch_page_turn/` absent | **KEEP_REJECTED** | File/directory does not exist. CPYOS's tap action enum is `tapzones::Action::NONE = 0` (not 4), and the migration it pins (`seedZones()` from gesture pairs) is already covered: `test/inx_navigation/test_tap_zone_geometry.py:14,89` sweeps every gesture combination through the real header. |
| 5eb498b0 | test(settings): mixed gesture pair migration regressions | none (covered) | — | same | **KEEP_REJECTED** | Same missing directory. `seedZones()` mixed-pair behaviour is exercised by the existing geometry test; nothing new to pin. |
| f0dfe5fc | fix: reduce protected-book and clipping sync memory usage (#4081) | Medium (diagnostics on the JJWXC protected path) | 1 line | none | **ADOPT_PARTIAL** | Take the one-liner: `lib/Epub/ContentProtection.cpp:134` currently does `err = "cannot open protected content";` with **no** cause — add `LOG_ERR("CPRO", "Manifest open failed: %s", book->lastError().c_str());` (`lastError()` is already used at `:86`). This is CPYOS's critical path (`CPRO open` failures are the JJWXC "index failure" class). **Reject the rest**: the C3 `FrameBufferLoan` belongs to `clippingSync::run()` (no `src/clippings/` in CPYOS), CPYOS's KOReader sync has no clipping leg, and the `freeink-sdk` gitlink bump would destroy the 31-file local patch. |
| 18d611f2 | test: fix two host-test failures on macOS / parallel ctest | none | — | files absent | **KEEP_REJECTED** | `test/test_recursive_dictionary.py` and `DictionaryHtmlPagesTest.cpp` are not in CPYOS (the recursive-dictionary test dir was not taken with 6376516c). Nothing to fix. |
| 136c2655 | chore: add license notices around the codebase (#4064) | Medium (attribution completeness for a public AGPL fork) | copy 3 text files + pointer lines; no code | conflicts with CPYOS licence scheme | **ADOPT_PARTIAL** | CPYOS's `THIRD_PARTY_NOTICES.md` (84 lines) covers upstream/kiikoread/MuPDF/FreeType/Noto/pypinyin/Lucide/wolfSSL/miniz but has **no provenance** for minibidi (Arabeyes, MIT), expat, epdiy-tools, atomic14, AnimatedGIF, zxing — all shipped. Lift `licenses/borrowed-code.txt`, `licenses/fonts.txt`, `licenses/hyphenation.txt` into CPYOS's `LICENSES/` with a short README, and add the one-line pointer header to the copied files (`lib/MiniBidi/minibidi.c` etc.). **Reject**: `release.yml`/`release_candidate.yml` edits, `.gitattributes`, and the 36-file banner churn — CPYOS's CI is `cpyos-release.yml`/`pages.yml` and its licence texts/scope differ. |

## Summary

Counts: **ADOPT_PARTIAL 6** (5789260c, 58d9c776, 919ec426, 057e16a4, f0dfe5fc, 136c2655) ·
**KEEP_REJECTED 10** (a722472c, 55a77c2e, e93f8d1f, d5d0bea1, 861683cc, c9f64d0d, f3ace194,
052d481a, 5eb498b0, 18d611f2) · **NEEDS_DEVICE_CHECK 1** (d5b6c74d) · ADOPT_NOW 0.

Every prior rejection held up on re-derivation; two corrections to the old reasoning: (a) `wantsListPicker()`/
fixed `browse.lists` *do* exist — what is missing is the remote list index (`lists_url`), so the flip point is
that feature, not the plugin system as a whole; (b) 919ec426 is not "docs churn" — CPYOS's docs assert a
WeRead duration upload that the code does not perform.

Top 5 by value/risk:
1. **f0dfe5fc** — 1 line; makes protected-book open failures self-explanatory (JJWXC path).
2. **58d9c776** — 3 lines × 2 packed pages; fixes the real "second toast vanishes early" bug.
3. **919ec426** — docs currently promise a feature that does not exist; cheap correction.
4. **057e16a4** — measured 84 KB per File Manager visit; rjsmin step is small and separable, optimizer split later.
5. **136c2655** — provenance for minibidi/expat/epdiy/AnimatedGIF before more public releases; texts only.

Independent finding (not from these SHAs): `PluginCatalogActivity.cpp:200` `browseLists.reserve(browse["lists"].size())`
is unbounded — a hostile `plugin.json` can abort the firmware (`-fno-exceptions`); clamp to 64 as upstream did.


---

## build/CI/docs

# Bucket 3 — build / CI / docs (13 items), re-derived

Repo inspected read-only at `/Users/hongbin/Documents/projects/cpyos/.worktrees/upstream-sync`
(HEAD `dfadf349`); CPYOS side: `repos/cpyos-public` (`platformio.ini`, `scripts/git_branch.py`,
`scripts/verify_release_artifacts.py`, `.github/workflows/`, `docs/`).

| SHA | subject | benefit | cost/risk | deps | verdict | rationale |
|---|---|---|---|---|---|---|
| 5a51bf32 | restore simulator HAL compatibility | none by itself | — | simulator pin `ad24d40→5a199a0`; CPYOS pins `58b6694d` in both `platformio.ini:373` and `test/CMakeLists.txt` (consistent) | NEEDS_DEVICE_CHECK | Simulator is a separate repo whose HAL must track reader HAL. CPYOS added its own HAL surface (`displayWindow`/`displayPanelWindow`/`cleanWindow`/`waitForScanIdle`/`idlePowerTick`), so an upstream pin cannot be adopted blind. Flip: when a merged slice needs a sim HAL added after `58b6694d`, bump to ≥`e5e716a` and re-run `simulator_readpico` + the Read Pico self-test. |
| 59912ea9 | link available PR firmware artifacts | PR reviewer convenience | JS workflow + node-run test | `.github/workflows/pr-firmware-links.yml` exists in CPYOS but is **disabled**; no enabled PR firmware build | KEEP_REJECTED | Nothing to fix in a workflow CPYOS does not run. Portable only if CPYOS ever enables PR firmware previews (then: link only unexpired artifacts, never claim "not available" when the build succeeded). |
| 35072257 | README → 2.0.0 | — | — | upstream `version = 2.0.0`, badges | KEEP_REJECTED | Branding + version scheme (`0.x-cpyos`, tag `cpyos-X.Y.Z`). |
| e7b47ee5 | CI on push to `develop` | — | — | upstream `ci.yml` branch list | KEEP_REJECTED | CPYOS branches are `cpyos`/`work/*`; upstream workflows are disabled. |
| a66d76f4 | unify firmware builds / release artifacts | **one portable technique**: `env.AddBuildMiddleware` injects `-DCROSSPOINT_VERSION` only into TUs whose source text contains the token → version/branch/sha change rebuilds ~1 object instead of the whole tree. Also: sanitize `"`/`\` out of git output before building a C string literal; release modes hard-fail on unknown/short revision | platformio.ini restructure is **all upstream** (`mode_dev/stable/nightly`, deletion of `-DCROSSPOINT_VERSION` flags, `-DENABLE_SERIAL_LOG`/`-DLOG_LEVEL` moved into modes, `gh_release_rc` becomes nightly). CPYOS keeps version in `build_flags` per env (`-cpyos`, `-cpyos-rc+${CROSSPOINT_RC_HASH}`) and `verify_release_artifacts.py` asserts the embedded string. Middleware would need re-verification of `esp_app_desc` and a build (currently `readpico` cache is reused; a failed middleware experiment = 14 min) | `git_branch.py` already present (global `env.Append`, so CPYOS's `default` recompiles everything when branch/sha changes); must keep `crosspoint.version`-based strings, `custom_sdkconfig` untouched | ADOPT_PARTIAL | Port only `AddBuildMiddleware` + git-output sanitization into CPYOS's existing `scripts/git_branch.py` (also wrap the `-DCROSSPOINT_VERSION` build_flags for `readpico*`); reject mode sections, the `<base>/-rc+/-dev-` policy (CPYOS tag scheme differs) and `test_build_modes.py` (it asserts "no `CROSSPOINT_VERSION` in env build_flags", the opposite of CPYOS). Provenance hard-fail is already covered by `--release` rejecting `-dirty`. |
| 6ec32892 | 1.6.5 release notes | bilingual highlights layout | — | `.github/release-notes/<v>.md` | KEEP_REJECTED | CPYOS keeps `docs/releases/<v>.md` + `.ota.json`; layout already matches. |
| 997b4331 | restrict Stable to four targets | rule: publication eligibility is a **separate explicit field** from the local build mode; a target missing from the requested channel returns `unsupported_channel`, never a silent cross-channel/board fallback | one-off `repair_stable_165.py` + workflow | `scripts/nightly_targets.py supportedChannels` | KEEP_REJECTED | CPYOS publishes one board (readpico, tag-scoped) and already selects OTA by board tag; the portable rule is already its behaviour. Keep in mind only if CPYOS ever adds a second channel. |
| 414b3797 | OPDS harness render-lock lifetime | — | — | `scripts/tests/test_opds_safety.py` absent in CPYOS | KEEP_REJECTED | Stub now needs `~RenderLock()`. CPYOS's real `RenderLock` has a destructor plus a same-task recursion guard (AGENTS §49.5/§49.7), so upstream's stub is strictly weaker. |
| c8175b1f | bound Read Pico simulator wake check | two facts: (a) a CI job that drives an interactive simulator self-test **must** carry `timeout-minutes` or it hangs until the 6 h default; (b) the wake test hung until its script was set **before** GPIO initialization, because upstream changed startup-input parsing; full four-orientation + wake check ≈ 6.2 s | sim pin `e112668→e5e716a` | upstream `hardware-ci.yml` (disabled in CPYOS), CPYOS sim pin | ADOPT_PARTIAL | Add `timeout-minutes` to the simulator/self-test step if CPYOS ever re-enables `hardware-ci.yml`; record fact (b) next to CPYOS's own simulator notes. Pin bump folds into 5a51bf32. |
| c17b80ab | Paper Mono in frontlight guide | rule: hide the warmth slider when the board has no warm channel (`gpioWarm` unset ⇒ `hasColorTemperature()` false) | — | upstream `USER_GUIDE.md`; CPYOS Read Pico has **no frontlight**, no Paper Mono | KEEP_REJECTED | Nothing in CPYOS's guide refers to frontlight; rule has no consumer. |
| a6db8f15 | README link → Silkscreen | — | — | upstream README | KEEP_REJECTED | Branding/links. |
| 91ed3ef8 | README bolding | — | — | upstream README | KEEP_REJECTED | Markdown only. |
| 7dc84ed6 | Waveshare 3.97 + TG28 PMIC | two portable facts: (a) **esptool 5.3.0 aborted at 7.6 % with `Packet content transfer stopped` at 460800/921600 baud; 4.8.1 worked** — CPYOS's PIO venv is on **5.3.0** and its readpico native USB-Serial/JTAG writes verify, so this is board/path-specific and must be re-checked before any esptool bump; (b) doc-honesty rule: never record a chip-ID readback that was never observed (TG28 is AXP2101-register-compatible, driver never reads `0x03`) | — | Waveshare target (CPYOS does not build it); Read Pico PMIC is CW32L010, not AXP2101 | ADOPT_PARTIAL | Port only the esptool-version note into CPYOS flashing docs/AGENTS §5 ("pin esptool; 5.3.0 is what we verify with; upstream saw it abort on another board") and the "no unobserved readback" rule. Board pin maps, PMIC nets, timings: not applicable. |

## Summary

Counts: **ADOPT_NOW 0 · ADOPT_PARTIAL 3 · ADOPT_AFTER_SDK 0 · NEEDS_DEVICE_CHECK 1 ·
KEEP_REJECTED 9.**

Confirmations of prior rejections: 9/13 hold — branding/docs-only READMEs (35072257, a6db8f15,
91ed3ef8), disabled-workflow fixes (e7b47ee5, 59912ea9, c8175b1f's pin half), release-process files
(6ec32892, 997b4331), a stale test stub (414b3797), and hardware CPYOS does not ship (c17b80ab).

Top 5, in priority order:
1. **a66d76f4** — `AddBuildMiddleware` so `-DCROSSPOINT_VERSION` reaches only consumer TUs; a
   version/branch/sha change then costs one object, not the tree. Sanitizing `"`/`\` out of git
   output belongs with it. Reject the mode/version-policy restructure.
2. **7dc84ed6 (esptool fact)** — CPYOS builds on esptool 5.3.0; upstream observed 5.3.0 aborting a
   flash at 7.6 %. Write down the verified version + the counter-example before anyone bumps it.
3. **c8175b1f** — `timeout-minutes` on any interactive-simulator CI step, and the "set the self-test
   script before GPIO init" ordering fact for Read Pico wake tests.
4. **5a51bf32** — simulator pin is a moving HAL contract; CPYOS's own HAL additions mean the pin
   must be validated by a sim build, not copied.
5. **997b4331 (rule only)** — keep "publication eligibility ≠ local build mode" and "no silent
   channel/board fallback" as a stated invariant for any future second channel.


---

## SDK_DEFER

# Bucket 3 — freeink-sdk: the 8 SDK_DEFER commits and the gitlink gap

Repo under review (read-only): `/Users/hongbin/Documents/projects/cpyos/.worktrees/upstream-sync`
(`work/upstream-sync`, HEAD `dfadf349`). Gitlinks: ours `96de1be6…`, `upstream/main` `d9a1304cd2…`
(verified with `git ls-tree`). Slice: `.scratch/upstream-audit/sync/b3-sdk.tsv`.

## 0. What was re-verified (and what the prior docs got wrong)

Everything below was checked against the local SDK clone in
`.worktrees/upstream-sync/freeink-sdk`, which has **both** objects (it is the patched 96de1be
worktree, 36 dirty entries). No re-clone, no builds, no repo edits.

Confirmed:

| Claim | Evidence |
|---|---|
| `96de1be` is an ancestor of `d9a1304c`; gap = 202 commits | report 09 §1; `git merge-base` OK |
| Patch set is 31 files / 114 hunks, 184,196 B, CFF patch 55 files / 879,253 B | `grep -c '^@@'` = 114, `'^--- '` = 31 |
| Re-port against `d9a1304c` fails **12 hunks in exactly 4 files** | `git archive d9a1304c` → `/tmp` + `patch --dry-run`: `FreeInkDisplay.cpp` 6/7, `EpdiyLcdDriver.cpp` 4/4, `EpdiyLcdDriver.h` 1/2, `SdmmcBlockDevice.cpp` 1/5 |
| `libs/display/EpdiyLcd/**` and `libs/hardware/BoardReadPico/**` have **zero** upstream churn | `git diff --shortstat 96de1be d9a1304c -- <path>` empty for both |
| Upstream CFF is strictly better: cff+psaux+**pshinter+psnames**, and `psintrp.c:590` heap-allocates `CF2_GlyphPath` (`ft_mem_alloc`) instead of a 16 KB frame | `git ls-tree`/`git grep` at `d9a1304c` |
| Upstream still lacks our SD DMA alignment (comment at `SdmmcBlockDevice.cpp:205` still lies; `:138` is `heap_caps_malloc`) | `git grep` at `d9a1304c` |
| Upstream still has no water ripple / area / band / CKV / async scan / PMU wake / rail idle | report 09 §4; `e0470_page_turn` count = 0 |

**Corrections to the TSV / sync plan** (these change verdicts):

1. `6b5566c8` does **not** need a new SDK API. `BatteryMonitor::isExternalPowerPresent()` already
   exists at `96de1be` (`BatteryMonitor.h:90`) and its gauge path already infers X3 external power
   from the BQ27220 `Current()` sign (`BatteryMonitor.cpp:525/609`). The app hunk only deletes
   `HalGPIO`'s duplicate raw-I2C probe. **Portable today.** The SDK delta of that commit is gauge
   I2C hardening (`ensureWire()` returns bool, drain on short reads) — nice, not required.
2. `ad08729e` is **inert for CPYOS**. `wifiMaxTxPower` lands in that commit's SDK bump
   (`a3a920d2→974029e8`, `BoardConfig.h`) but is `60` **only for `EEGO_A4`**; every other profile
   (Read Pico, X3/X4, …) is `0`, so `setup()`'s `if (field != 0)` never installs the handler.
3. `60d3c8bb` (cover upscale, listed "SDK blocked" in `docs/plans/2026-11-08-upstream-sync.md`) has an
   SDK delta of **BMI270 IMU** — unrelated to covers. Its app side uses app-owned
   `HalDisplay::Controller`/`Bitmap` APIs. Likely portable now; trial-port it before paying Tier-3 cost.
4. `ce781eb8`'s SDK delta is a **10-line `FreeInkApp.h`** addition; the rest is app-owned
   (`EpdFontFamily::hasCodepoint`, `FontDecompressor::prewarmCache`). Small dependency, not a rewrite.
5. `93a14afd` is not just "tracking": the fix inside `32a265fa→7fcd3b34` is `c2d711a`
   *"prevent X3 grayscale SPI deadlock"* — **one line, `txn.end()` in `Uc8279Driver.cpp`** — and
   `7fcd3b34` is **not** an ancestor of `96de1be`. Staying put keeps a real C3/X3 bug.
6. Conversely the delta brings network work CPYOS would kill for: `fail fast when a host name does not
   resolve` (`7ca28e5`), `connection-owned buffers for bulk TLS reservations` (`e49d6b8`),
   `SecureHttpClient` header/record reservations, OPDS capacity tests — 1,512 insertions in
   `libs/network` alone. This is the strongest functional argument for bumping, stronger than any of
   the 8 commits in the slice.

## 1. Per-commit table (the 8 `SDK_DEFER` rows)

All eight are ancestors of `upstream/main` and every gitlink they point at is an ancestor of
`d9a1304c`, so a single move to `d9a1304c` contains all of them.

| SHA | fixes / adds | app-side portable? | boards | needs SDK API | verdict |
|---|---|---|---|---|---|
| `6b5566c8` | X3 power detection moved into SDK; app loses 52 lines of raw `BQ27220` I2C in `lib/hal/HalGPIO.cpp` (+`device-variants.md`, test) | **Yes** — API exists at 96de1be (TSV wrong). 3 files, −49/+3 net app lines | **C3 X3/X4** (`default`) | No | **Port now** (cheap dedup); SDK bump adds gauge I2C hardening |
| `93a14afd` | SDK bump for **X3 anti-aliasing freeze #3960** = `c2d711a` missing `txn.end()` in `Uc8279Driver` → grayscale SPI deadlock | No app hunk | **C3 X3/X4** | n/a (SDK-only) | **Fold into bump**; do not drop — it is a live C3 bug |
| `ad08729e` | WiFi max-TX-power cap via `ARDUINO_EVENT_WIFI_{STA,AP}_START` (12 app lines) | Blocked at 96de1be (field absent) **but** the only board with a value is EEGO A4 (`60`), every CPYOS board is `0` | none shipped | `BoardConfig::wifiMaxTxPower` | **Skip** (zero value for Read Pico / X3 / X4); revisit only on a SAR/regulatory ask |
| `98b8ac04` | EEGO A4 touch calibration | n/a | unshipped | n/a | Fold into bump |
| `49b84fdc` | Picco display variant | n/a | unshipped | n/a | Fold into bump |
| `9ba67ea5` | `connect(shouldAbort)` before `addHeader()` in `PluginHttp.cpp` (7/3) and `/api/fetch` (`CrossPointWebServer.cpp` 14/10) — a 4 KB plugin cookie stops starving the TLS handshake | **No** — needs `SecureHttpClient::connect()` + `setReserveFullRecord()`; verified absent at 96de1be, present at d9a1304c | all (plugin relay/fetch) | `SecureHttpClient::connect(const AbortCallback&)`, `setReserveFullRecord()` | **Port immediately after the bump** — CPYOS has the same ordering today (`PluginHttp.cpp:33-34`) and a documented `largest≈7 KB` handshake history |
| `10709120` | EEGO A4 battery levels from ADC on some units | n/a | unshipped | n/a | Fold into bump |
| `37138937` | bump for eego + picco fixes | n/a | unshipped | n/a | Fold into bump |

**Also gated on the same bump** (sync plan "SDK 阻塞清单"; re-assessed):

| SHA | what | real blocker | verdict |
|---|---|---|---|
| `60d3c8bb` | cover upscale + `BmpToBmpConverter` + "set cover on touch boards" (~190 new app lines, 178-line test) | **Probably none** (SDK delta = BMI270 IMU) | trial-port **now**; promote or defer on evidence |
| `e54c0087` | Metalio sleep (2-2 app lines) | Metalio not shipped | skip |
| `ce781eb8` | prewarm visible CJK glyphs before list painting | 10-line `FreeInkApp.h` addition | port with bump (UI latency win) |
| `f0dfe5fc` | protected-book / clipping-sync memory | `ProtectedBook` manifest reserve in SDK (323 lines + tests) | port with bump |
| `6b5566c8` app half | see above | none | port now |

## 2. Integration options

| | work breakdown | files / hunks | risk | what breaks if skipped | effect on the final `git merge upstream/main` |
|---|---|---|---|---|---|
| **(i) `d9a1304c` + re-port, keep patches** | Take scratch SDK worktree at d9a1304c → re-port the 12 failing hunks (facade + adapter) → **delete `patches/freeink-sdk-cff/`** (upstream CFF supersedes), keep `FREEINK_FONT_ENABLE_CFF=1` → re-measure render-stack need → regenerate `96de1be-local.patch` → build matrix → device soak | 27/31 files apply clean (incl. all 19 epdiy + 4 BoardReadPico + `PanelDriver.h` + `FreeInkDisplay.h` + `SecureClient.cpp`); **12 hunks manual in 4 files**; CFF 55-file patch deleted | highest concentration in `FreeInkDisplay.cpp` (upstream +731/−358: rewrote the facade, moved grayscale composition into `displayGrayscaleBaseWithContext`, changed `cleanupGrayscaleWithPreviousBuffer` semantics). Our band diff, `presentOverlay`, `GrayArea` commit, async start/finish and water-turn plumbing must be re-threaded. `EpdiyLcdDriver` must adopt `controllerName`/`powerOffBetweenFrames`/`displayGrayscaleBaseStart`/`displayCommitted`/`requestResync` | — (functionally neutral) but the **36-dirty-file / patch-drift hazard stays**: `git submodule update --init` still nukes the work; every future SDK bump re-pays the same 12 hunks | `freeink-sdk` still conflicts on every slice; we keep resolving it with "ours + patch", i.e. the merge never learns the SDK state |
| **(ii) stay at `96de1be`, resolve merge with "ours"** | ~0 now | 0 | none now | SDK freezes 202 commits back: keeps **X3 grayscale SPI deadlock**, no `SecureHttpClient::connect`, no fail-fast DNS / connection-owned TLS buffers, no `reserveFullRecord`, no protected-book memory fix, no CFF/pshinter, no BMI270/GT911/touch work. Cost compounds every week and the eventual port gets harder | trivial now, worst later (same 12 hunks, larger facade delta) |
| **(iii) fork SDK at `d9a1304c` + our re-ported work, move gitlink** | (i) **plus**: fork `Free-Ink/freeink-sdk` (MIT — verified `LICENSE`) as `Yana-Hangabina/freeink-sdk`, branch `cpyos` = `d9a1304c` + our work as **normal commits** → `git submodule set-url freeink-sdk …` + move gitlink → delete `patches/freeink-sdk*` + the `apply.sh`/`assume-unchanged` steps in `cpyos-release.yml:86-101` → rewrite AGENTS §15/§31 and the patch READMEs | same 12 hunks as (i) + 1 gitlink line + 2 workflows + 3 docs; 2 patch dirs deleted | fork is a new artifact to keep in sync; upstream sync becomes `git merge upstream/main` **inside the fork** (conflict surface = our 31 files only, instead of "regenerate a patch from a dirty tree") | — | **best**: the gitlink becomes a single deterministic answer, so `git merge upstream/main` never re-conflicts on `freeink-sdk`; the "never run submodule update" trap disappears |

Decision: **(iii) is the target, and (i) is its first half** — do (i) in a scratch tree, then land it as
fork commits. (ii) is only defensible if the SDK bump is deliberately deferred past the firmware merge;
it must then be scheduled, not left implicit.

## 3. Sequenced plan and effort

| Phase | Work | Effort |
|---|---|---|
| **P0 — harvest the free wins** | Port `6b5566c8` app hunk (verified portable). Trial-port `60d3c8bb`; if it builds, it is a normal bucket-1 item, not Tier 3 | **0.25 dev-day** |
| **P1 — scratch re-port** | `/tmp` worktree at `d9a1304c`; re-port 12 hunks; drop CFF patch; `grep` self-proof (`epd_draw_base_start`, `claim_frame_start`, `e0470_textturn_soft_settle`, `water_hot_base`) | **1.5–2 dev-days** |
| **P2 — facade + adapter** | `FreeInkDisplay.cpp` band/overlay/idle/gray-area/async on upstream's rewritten facade; `EpdiyLcdDriver.{h,cpp}` onto the new driver contract; `SdmmcBlockDevice` mount loop = upstream 40 MHz + our 40→20→10 fallback; keep DMA alignment | **1.5–2 dev-days** |
| **P3 — fork + gitlink** | Fork, branch, land P1/P2 as commits, `submodule set-url`, move gitlink, delete patch dirs, edit `cpyos-release.yml`, rewrite patch READMEs + AGENTS §15/§31 | **0.5–0.75 dev-day** |
| **P4 — build matrix** | `readpico`, `readpico_nosource` (`nm \| grep -c JjwxcActivity` = 0), `default` (C3 — remember the ROM-libc `cp sdkconfig.default …/esp32c3/sdkconfig`), `readpico_nightly`; host tests incl. `EpdiyLcd/test/host/test_transactions.py`, `band_policy`, `changed_band`, `line_mask`, `otf_support` | **0.5 dev-day** |
| **P5 — device** | `exp/refresh_soak.py` 38/38; water ripple; 16-level AA + band/local-clean; rails idle off/on; PMU short-press wake; SD mount log `SDMMC mounted at 40000 kHz`; C3/X3 grayscale path | **1 device-day** |
| **P6 — port the gated app commits** | `9ba67ea5` (plugin connect), `ce781eb8`, `f0dfe5fc`, `6b5566c8` if not done in P0; skip `ad08729e` / `e54c0087` | **0.5 dev-day** |
| **P7 — final merge** | `git merge upstream/main` with the fork gitlink already in place; resolve only app files | **0.5–1 dev-day** |

Totals: **(i) = 4–5 dev-days + 1 device day**; **(iii) = (i) + 0.5–0.75 day** (≈5 dev-days + 1 device
day). The report's 3.5–4.5 d is slightly optimistic once the fork plumbing and the 4 gated app commits
are counted.

Interactions that must be respected:

- **Order matters for the final merge**: land the gitlink switch (P3) *before* `git merge
  upstream/main`, so the merge has one deterministic `freeink-sdk` resolution. Bucket 1/2 already merged
  app files that assume SDK behaviour (fonts, renderer, storage); they compile unchanged after the bump,
  but the 12 re-ported hunks must be in place first or the build fails in `lib/hal/HalDisplay.cpp`.
- **Release pipeline**: `cpyos-release.yml:86-101` currently does `submodule update --init` → `apply.sh`
  ×2 → `git update-index --assume-unchanged freeink-sdk`. Under (iii) that collapses to `submodule
  update` (fork URL) and the `--assume-unchanged` line goes away; `verify_release_artifacts.py
  --release` gets *easier* (clean submodule instead of assumed-unchanged). CFF stays a platformio flag,
  not a patch step.
- **5 data blobs**: the SDK bump cannot change `pinyin.dat` / `hyphen.dat` / `i18n_pack.dat` /
  `web_pack.dat` / `emoji.dat`. Expect byte-identical `build/data_manifest.txt` size+sha256; **no
  data-partition reflash** is implied by this work. A data reflash is only forced if the *firmware* step
  changes i18n keys (it should not) — verify with `generate_data_manifest.py --check` and
  `verify_release_artifacts.py`. `SECTION_FILE_VERSION` may move if the facade changes page geometry;
  that is a one-time on-device page-cache rebuild, not a partition write.
- **CFF drop has a follow-on**: upstream's heap `glyphPath` removes the 16,128 B frame, so the 32 KB
  PSRAM render stack may be reducible — **keep 32 KB until measured** (AGENTS §18 measured peak ≈23 KB
  *with* the old interpreter frame).

## 4. Patch-only SDK API ledger — do not delete the patches early

Verified callers in the app tree (`grep -rl` under `src lib`):

| API (patch-only) | Callers | Feature that dies |
|---|---|---|
| `epdiyLcdUpdateArea` | `lib/hal/HalDisplay.cpp`, `lib/GfxRenderer/GfxRenderer.cpp` | minute-clock / band window push |
| `epdiyLcdCleanArea` | `HalDisplay.cpp` | local (rect) ghost clean |
| `epdiyLcdScanInProgress` | `HalDisplay.cpp` (`waitForScanIdle`) | present barrier before SD writes |
| `epdiyLcdTakePushStats` | `HalDisplay.cpp` | `CMD:PUSHSTATS` / soak assertions |
| `displayWindow` / `displayPanelWindow` | `HalDisplay.*`, `GfxRenderer.*`, `LibraryListActivity`, `main.cpp` | band & window refresh |
| `idlePowerTick` / `epdiyLcdRailsKeepalive` | `HalDisplay.*`, `GfxRenderer.*`, `PersistableStore.h`, `main.cpp` | 8 s rail idle poweroff (XOE release) |
| `commitGrayscale16{,Area,Areas}`, `beginGrayscale16From1bpp` | `HalDisplay.*`, `GfxRenderer.*` | 16-level AA regions, gray→gray no-promotion |
| `setPageTurnDirection` | 5 files incl. `EpubReaderActivity` | water-ripple page turn plumb |
| `epdiyLcdSetAaRealDiagonal` | `main.cpp` | `CMD:AAWAVE` A/B |
| `BoardReadPico::pmuTakeKeyWakeup` / `pmuLastEventId` | `lib/hal/HalPowerManager.cpp` | timer-only light sleep + short-press wake |
| SD 40→20→10 ladder, cache-line DMA alignment | SDK-internal | slow-card mount recovery, no hidden 4 KiB bounce buffer |
| `SecureClient` abort-after-failed-connect | SDK-internal | plugin/font cancel during TLS retry |
| CFF/psaux (`cff-otf.patch`) | SDK-internal | `.otf` rendering — **but upstream replaces it, drop the patch, keep the flag** |

Removing the patches while still on `96de1be` breaks the link in ~10 app files; removing them *after*
(iii) is the intended end state, provided P2 re-ported all of the above as normal commits.

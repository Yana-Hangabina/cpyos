# 上游同步执行计划：桶 1 + 桶 2（2026-11-08）

分支：`work/upstream-sync`（worktree `.worktrees/upstream-sync`）。审计全文：
[`2026-11-08-upstream-sync-audit.md`](2026-11-08-upstream-sync-audit.md)。

## 目标与范围

- 在最终合并 `0x1abin/main` 之前，先把 254 个上游非合并提交中值得合入的 **113 项**落到 cpyos：**桶 1 = B_CLEAN 36**（可直接合并）、**桶 2 = C_CONFLICT_WORTH 77**（冲突但值得，按审计里的改法移植）。
- 本轮**不做**：D_SKIP 88（理由见审计）、A_ALREADY 45（无需动作）、上游合并提交本身、freeink-sdk 重移植（另立专项）。
- SDK 闸：依赖 `96de1be` 不存在的 SDK API 的条目一律标 `BLOCKED_SDK` 并停止，不强行改。

## 硬规则

1. 每项一个提交，提交信息带来源：`<type>(<area>): <desc> (upstream <sha>)`。
2. 禁止 PlatformIO 构建（共享包/并发风险）；可跑相关 host 测试；不得改其它分支、refs、远端或 `repos/cpyos-public` 工作树。
3. i18n 只**追加键**到 `lib/I18n/translations/english.yaml` 与 `chinese.yaml` 末尾；不改格式、不动其它语言文件。
4. `src/SettingsList.h` / `src/CrossPointSettings.*` 只**追加**行/字段，不重排、不复制上游的设置表结构。
5. `SECTION_FILE_VERSION` 保持 CPYOS 的 **86/87**；绝不采用上游 56/59（尤其 `9dfa92ba` 的 Section hunk）。
6. `0305aaa6`（上游 2.0.0 渲染重写）不整体采用；相关文件保留 CPYOS 实现，只按审计移植修补逻辑。
7. 涉及 `freeink-sdk` gitlink 的上游提交只取 app 侧 hunk；SDK 侧改动留给 SDK 专项。
8. 每项完成后写 `findings/applied-<owner>.md`（状态、提交哈希、改动文件、测试结果），并返回摘要。

## 分工

| owner | branch | 条目数 | B | C | 说明 |
|---|---|---:|---:|---:|---|
| renderer | `work/sync-renderer` | 10 | 3 | 7 | lib/Epub、lib/GfxRenderer、排版/分页/ruby/RTL |
| fonts | `work/sync-fonts` | 14 | 3 | 11 | lib/EpdFont、lib/I18n、字体链（顺序敏感） |
| storage | `work/sync-storage` | 23 | 11 | 12 | 封面/存储/JSON/书架/词典/睡眠图 |
| misc | `work/sync-misc` | 15 | 4 | 11 | 杂项：XTCH、zip、dictzip、TTC、captive portal、SDK 前哨 |
| network | `work/sync-network` | 18 | 5 | 13 | webserver/WebDAV/插件/OPDS/KOSync/WeRead |
| ui-a | `work/sync-ui-a` | 9 | 2 | 7 | 主页/书架/键盘/布局/导航 |
| ui-b | `work/sync-ui-b` | 14 | 3 | 11 | 阅读器菜单/点按区/OptionPopup/睡眠/图片灰阶 |
| build | `work/sync-build` | 10 | 5 | 5 | 构建脚本、CI、工具链、文档 |

### 条目清单（逐项勾选）

#### renderer（10）

- [ ] `149dd532` [B] fix: stop books with thousands of chapters aborting the index build
- [ ] `56c677f6` [C] fix: lend the framebuffer to the chapter build's image dimension probe
- [ ] `1a5c8cd1` [C] perf: Batch page-position reads
- [ ] `fc4b740a` [B] fix: copy a TXT cover with a checked read
- [ ] `daa004da` [B] fix(txt): resolve first-page chapter navigation
- [ ] `f8936ba6` [C] perf: finish chapter pagination in idle time
- [ ] `9c3fbdd1` [C] fix(ruby): handle ruby text narrower than its base text
- [ ] `72ca92fe` [C] fix(dict): strip CJK blanket-like punctuations
- [ ] `31dc683f` [C] fix: broken focus reading with RTL text
- [ ] `ad53eb64` [C] fix(ruby): make ruby renderings closer to web browsers

#### fonts（14）

- [ ] `f1d0af1a` [C] fix: keep SD-font kerning and ligatures in edge cases (#3838)
- [ ] `99c2f09d` [B] feat: Add vector font (.ttf/.otf) upload support in webserver (#3717)
- [ ] `7394e40f` [C] feat: render redaction blocks without a special font (#3882)
- [ ] `9610da70` [C] perf: stop SD-font caches from splitting the heap while reading (#3831)
- [ ] `e78f09fd` [C] fix: translate complete confirmation questions (#4097)
- [ ] `0080d8fe` [B] fix: highlight current selection in font downloader (#4101)
- [ ] `b912e000` [C] perf: store only present SD font styles (#4114)
- [ ] `83df537d` [C] perf: skip the advance-table scan once the table is full (#3948)
- [ ] `4663d177` [C] perf: share SD font interval tables across loaded fonts (#4145)
- [ ] `70b3b1a3` [C] perf: reduce font download heap (#4124)
- [ ] `3f387a98` [C] fix: Text Settings do not render correctly in landscape mode (#4172)
- [ ] `551d48e7` [B] fix: Correcting capitalization in English settings to make it uniform (#4224)
- [ ] `42d58daa` [C] chore: update translations (#4283)
- [ ] `51e0baa2` [C] fix: fix ttf font failure and memory cleanup (#4284)

#### storage（23）

- [ ] `e54c0087` [C] fix: Metalio E-Ink 4 powers off fully when it sleeps
- [ ] `f889b828` [C] fix: dictionary back overflow bug
- [ ] `9dd39984` [C] fix: check cached cover payload on Home fast paths
- [ ] `69c8d867` [C] fix(dict): show all matching `.syn` definitions
- [ ] `e0bcd26b` [B] feat: filter plugin events by book metadata
- [ ] `9ac185c8` [C] fix: URL-encode plugin template variables
- [ ] `b6605a51` [C] fix: decode inline dict type
- [ ] `00897520` [C] feat: accept server paths in JSON plugin catalogs
- [ ] `164f0995` [B] fix: keep heap free while the plugin relay buffers a response
- [ ] `a6fd569f` [B] fix(waveshare-epaper-397): always deep sleep, wake on any rotary key
- [ ] `813aaa12` [B] fix(bidiutils): canonicalize reversed tuple in emission path
- [ ] `1f77b83d` [B] fix: stop dropping log lines at the low-power clock
- [ ] `bb4f8785` [B] fix: map JPEG scale options to pixel divisors
- [ ] `f47a218b` [C] feat(kosync): add server profiles and extended metadata
- [ ] `3a03be89` [B] fix(KOSync): resolve upload XPaths for text outside `<p>`/`<li>`
- [ ] `aa76a31b` [B] fix(kosync): advertise percentage progress support
- [ ] `79915f07` [B] fix: wrap plugin event disclosures to fit screen
- [ ] `9561a741` [B] fix(sync): enforce C3 memory regression checks
- [ ] `c71c1562` [B] test: run standalone UI regressions with CTest
- [ ] `09fedb97` [C] fix: refresh Library after plugin book installs
- [ ] `83f9cc05` [C] fix(time): Use RTC for plugin time
- [ ] `7365c1ac` [C] fix: keep cumulative spine sizes in book.bin instead of RAM
- [ ] `3db7a96b` [C] fix: validate cached cover size against the BMP pixel data offset

#### misc（15）

- [ ] `f084a4dd` [C] fix(covers): reject metadata offsets and repair empty XTC thumbs
- [ ] `148e6e2b` [C] fix: XTCH books missing from library, XTC books show filename and unknown author (#4023)
- [ ] `92216b41` [B] refactor: remove unused zlib header helper (#4057)
- [ ] `77725069` [C] fix(storage): preserve saved JSON when snapshot allocation fails
- [ ] `c54ee33f` [C] fix(storage): reject partial analytics JSON writes
- [ ] `6f6bac9a` [C] fix(storage): recover retained JSON staging files
- [ ] `20516ddc` [C] test(tapzone): drive the Confirm entry states through finishShow
- [ ] `5d18bee7` [B] perf: allocate BiDi workspace on first use (#4116)
- [ ] `116195b4` [C] fix: ignore system folders in Library (#4184)
- [ ] `b05cb3e8` [B] docs: fix KOReader document ID offset comment (#4191)
- [ ] `60d3c8bb` [C] fix: Scale custom covers up & let you set them on touch boards (#4185)
- [ ] `280895ff` [B] fix: split dictzip inflate window into 8KB segments (#3826)
- [ ] `4042e454` [C] fix: open the hotspot captive portal on phones (#4211)
- [ ] `d663dcc6` [C] feat: Show every sleep image once in randomized order before repeats (#3841)
- [ ] `4c9ec287` [C] fix(font): render .notdef glyph for replacement characters in TTF fonts (#4256)

#### network（18）

- [ ] `f331030f` [B] Allow plugins to override download directory via config
- [ ] `cdac66ff` [B] fix(webdav): GET sends one byte per file
- [ ] `26ccbe28` [C] fix: say why an OPDS feed fetch failed
- [ ] `c1a62f66` [B] fix: prevent FilesPage modal from closing when selecting text
- [ ] `5bd05983` [B] fix: normalize web upload filenames to NFC
- [ ] `d6c1512e` [C] Merge PR #418 weread x3 sync
- [ ] `eaf5c73a` [C] fix(weread): preserve annotations as EPUB footnotes
- [ ] `39e56ca1` [C] fix(plugins): paginate static catalogs after compat filtering
- [ ] `b4d9458f` [C] fix: add hold-to-uninstall for plugins
- [ ] `daf0c177` [B] feat: add overwrite flag to /upload
- [ ] `c9cb1f8d` [C] perf: add ETag to plugin file responses
- [ ] `c3b16da5` [C] fix: plugin catalog paging with {page}+{limit}
- [ ] `db9dd391` [C] feat: method and body for /api/fetch
- [ ] `7990db2b` [C] fix: count untitled rows on server catalog pages
- [ ] `91e9efc3` [C] fix: make web Fonts and Settings pages work with many SD font families
- [ ] `e8299b5f` [C] fix: keep the web server responsive and within memory while plugins load
- [ ] `552c932b` [C] perf: stream plugin relay replies instead of buffering
- [ ] `2ec01c96` [C] perf: free about 5KB of heap while the web server runs

#### ui-a（9）

- [ ] `a0946097` [C] perf: avoid list navigation callback allocations
- [ ] `eced576b` [B] fix: add plugin-manager to return-selection mapping
- [ ] `8e30a699` [C] fix: stable tip positions in keyboard activity
- [ ] `50823ffa` [C] fix: Center button hints using the actual text height
- [ ] `aad48b40` [C] fix: Sub-chapter contents navigation
- [ ] `02d2a628` [C] fix(home): hit-test home menu rows from theme geometry
- [ ] `b90389ea` [B] feat: hide dictionary entry when none configured
- [ ] `6d649b66` [C] fix: add sleep screen confirmation
- [ ] `696fcc5c` [C] fix: avoid restoring stale home cover pixels

#### ui-b（14）

- [ ] `6fb907a6` [C] fix(reader): route the chapter tap zone through the TXT-aware navigation
- [ ] `cdab81d8` [C] fix(tapzone): own a fresh Confirm after the popup opens
- [ ] `6cff5326` [C] fix(tapzone): own a Confirm that arrived already released
- [ ] `e59f87b6` [C] fix: truncate the Wi-Fi SSID by width, not bytes (#4079)
- [ ] `6251054b` [B] fix: merge sleep image directory pools (#4025)
- [ ] `51089beb` [C] refactor: decouple sleep overlay from sleep mode (#3770)
- [ ] `2a8c1ec5` [C] fix: show cache-clear warning inside the dialog (#4105)
- [ ] `8cf400c6` [C] fix(dictionary): support multiple idx entries with same string (#4110)
- [ ] `0f4f82ec` [B] feat(plugins): Make session recording more robust (#3869)
- [ ] `f73d788a` [C] fix: OPDS crashes on the book details screen (#4165)
- [ ] `5d1142c1` [C] feat(reader): add Image Grayscale toggle for EPUB image pages (#4160)
- [ ] `40a633e7` [C] fix: reader menu selection (#4227)
- [ ] `d9764e2b` [B] feat: add end_percent to plugin reading events (#4201)
- [ ] `0ecc7d78` [C] fix: back tap open frontlight dropdown (#4276)

#### build（10）

- [ ] `9ea86717` [C] perf: skip unused Arduino SDK libraries (#3857)
- [ ] `bbb45c39` [B] fix: scope Git version to sources that use it (#3860)
- [ ] `6def3259` [B] fix(weread): preserve navigation after single-page shelf holds
- [ ] `e142df46` [B] fix(settings): explain language selection save failures
- [ ] `fe042860` [C] fix(txt): reject incomplete BMP cover copies
- [ ] `a5340ea6` [B] chore: compile out the wolfSSL debug trace, save 39Kb (#4213)
- [ ] `f2a1beda` [C] fix: rebuild core when component lists change (#4214)
- [ ] `28971493` [C] docs: clarify non-Latin UI fallback isn't CJK-specific
- [ ] `9f4e387d` [C] docs: explain IPA in dictionary pronunciations (#4034)
- [ ] `0fcdb3ec` [B] fix: allow deleting hidden files in the web UI (#4031)

## 集成与验收

1. 按 renderer → fonts → storage → misc → network → ui-a → ui-b → build 顺序把 8 条分支合入 `work/upstream-sync`。
2. 共享文件冲突（`SettingsList.h`、`CrossPointSettings.*`、i18n YAML、`OptionPopup.h`、`main.cpp`）在集成阶段统一解决。
3. 构建矩阵：`readpico`、`readpico_nosource`（`nm | grep -c JjwxcActivity` 必须 0）、`default`(C3)；跑相关 host 测试。
4. 真机：`exp/refresh_soak.py` 36/36 + 字体/词典/WebDAV/OPDS/OTA 冒烟。
5. 全部通过后，再执行上游 `git merge upstream/main`（下一阶段），把 behind 清零。

## SDK 阻塞清单（本轮不碰）

- `ad08729e`（`wifiMaxTxPower`）、`9ba67ea5`（`SecureHttpClient::connect()`）、`6b5566c8`（X3 power API）
- `60d3c8bb`（封面放大 + `BmpToBmpConverter` + gitlink）、`e54c0087`（Metalio 睡眠的 SDK 半）、`ce781eb8`、`f0dfe5fc`
- 这些在 SDK fork + 重移植（`d9a1304c`）完成后作为 Tier 3 处理。

# CrossMux Development Guide

Project: CrossMux, a community fork of CrossPoint Reader for ESP32 e-ink devices.
Mission: Keep reading fast and reliable while supporting lightweight apps, reading analytics, standby faces, and on-demand services within the hardware budget.
Targets: Xteink X3/X4 share an ESP32-C3 image; ESP32-S3 targets have separate images. See [`scripts/nightly_targets.py`](scripts/nightly_targets.py) for release targets and channels.

> **This file is a map, not a manual.** It holds the identity, the
> non-negotiable invariants, and a quick reference — then points to the deep
> engineering docs in [`docs/engineering/`](docs/engineering/index.md). Read the
> linked doc that matches your task instead of carrying everything in context.
> The detailed docs are the system of record; keep them current when behavior
> changes.
>
> `AGENTS.md` is the sole agent entrypoint and a regular file.
> On-demand skills, scripts, and references live directly in `.agents/skills/`.
> Links below are relative to the repository root.
>
> **Maintainers** — when merging upstream changes to this file, follow
> [docs/engineering/upstream-merge-policy.md](docs/engineering/upstream-merge-policy.md):
> keep this map thin; route deep content into `docs/engineering/`.

## AI Agent Identity and Cognitive Rules

* Role: Senior Embedded Systems Engineer (ESP-IDF/Arduino-ESP32 specialized), preserving CrossMux behavior and the HAL boundary across device targets.
* Primary Constraint: ESP32-C3 has about 380KB usable RAM and no PSRAM; shared reader code must fit that baseline. S3 budgets and capabilities are target-specific. Stability is non-negotiable.
* Evidence-Based Reasoning: Before proposing a change, you MUST cite the specific file path and line numbers that justify the modification.
* Anti-Hallucination: Do not assume the existence of libraries or ESP-IDF functions. If you are unsure of an API's availability for the ESP32-C3 RISC-V target, check the freeink-sdk source or the FreeInk SDK docs (https://freeink.org/llms.txt for an LLM-readable index) first.
* No Unfounded Claims: Do not claim performance gains or memory savings without explaining the technical mechanism (e.g., DRAM vs IRAM usage).
* Resource Justification: You must justify any new heap allocation (new, malloc, std::vector) or explain why a stack/static alternative was rejected.
* Verification: After suggesting a fix, instruct the user on how to verify it (e.g., monitoring heap via Serial or checking a specific cache file).

## Golden Rules — Non-Negotiable Invariants

These are the highest-frequency-violation rules. Each links to the doc with the
full reasoning, examples, and edge cases.

1. **ESP32-C3 sets the shared-code baseline: ~380KB RAM, no PSRAM.** Justify every heap allocation; prefer stack/static; `.reserve()` before `push_back` loops; mark constants `constexpr`. → [hardware-constraints.md](docs/engineering/hardware-constraints.md)
2. **Never bare `new`.** With `-fno-exceptions` a failed `new` calls `abort()`, not `nullptr`. Use `makeUniqueNoThrow<T>()` from `lib/Memory/Memory.h` (or `new (std::nothrow)` only when a C API takes ownership); always null-check and `LOG_ERR` on OOM. → [memory-and-allocation.md](docs/engineering/memory-and-allocation.md)
3. **All user-facing text uses `tr()`.** Never hardcode UI strings (logs may be hardcoded). → [ui-and-input.md](docs/engineering/ui-and-input.md)
4. **Use HAL classes, never the SDK directly** (`Storage`, `HalDisplay`, `HalGPIO`). → [architecture-and-patterns.md](docs/engineering/architecture-and-patterns.md)
5. **No `file.close()` on local `FsFile`** — `DESTRUCTOR_CLOSES_FILE=1` handles scope exit (close member files in `onExit()`, and before delete/reopen). → [build-system.md](docs/engineering/build-system.md)
6. **`memcpy` for unaligned reads.** RISC-V faults on raw `reinterpret_cast` of `uint8_t*` to wider types. ISRs need `IRAM_ATTR`; never call a mutex from an ISR. → [esp32-pitfalls.md](docs/engineering/esp32-pitfalls.md)
7. **Use `MappedInputManager::Button::*` logical buttons**, never raw `HalGPIO::BTN_*` (except in ButtonRemapActivity). → [ui-and-input.md](docs/engineering/ui-and-input.md)
8. **All rendering through the `GUI`/UITheme macro.** Never hardcode 800/480 — use `renderer.getScreenWidth()/getScreenHeight()`. → [ui-and-input.md](docs/engineering/ui-and-input.md)
9. **Free in `onExit()` what you alloc in `onEnter()`.** `vTaskDelete()` tasks before activity destruction; activities are heap-allocated and deleted on exit. → [architecture-and-patterns.md](docs/engineering/architecture-and-patterns.md)
10. **Bump the cache format version BEFORE changing a binary layout** (`book.bin`, `section.bin`); document it in `docs/file-formats.md`. → [cache-management.md](docs/engineering/cache-management.md)
11. **Edit sources, not generated files** (`*.generated.h`, `I18n*` generated headers). → [generated-files.md](docs/engineering/generated-files.md)
12. **Verify repo context before any git op; commit only when explicitly requested.** Unqualified PR requests target `0x1abin/crossmux:main` via `origin` without asking; an explicit user target overrides this default. Do not merge upstream during ordinary tasks. Never stage `.gitignore`d files. → [git-workflow.md](docs/engineering/git-workflow.md)
13. **One physical button gesture causes one action.** Normal Activities read the shared input snapshot; across popups and Activities, gate inherited held buttons and consume the triggering release. → [ui-and-input.md](docs/engineering/ui-and-input.md)
14. **Adjacent interactive controls keep at least 6 px of visible space.** Their hit regions must not overlap, and drawing plus hit-testing must use the same geometry source. → [touch-and-ui.md](docs/contributing/touch-and-ui.md)

## Quick Reference

**Platform detection** (run once per session): `uname -s` → `MINGW64_NT-*` (Windows Git Bash) / `Linux` / `Darwin` (macOS).

**Singletons**:
```cpp
#define SETTINGS CrossPointSettings::getInstance()  // User settings
#define APP_STATE CrossPointState::getInstance()    // Runtime state
#define GUI UITheme::getInstance()                   // Current theme
#define Storage HalStorage::getInstance()            // SD card I/O
#define I18N I18n::getInstance()                     // Internationalization
```

**Core commands**:
```bash
pio run                             # Build (default env)
pio run -t upload                    # Build + flash
pio run -e gh_release                # Unified-language X3/X4 stable firmware
pio run -e simulator -t run_simulator # Desktop X4 simulator (SDL2 + curl)
pio check                           # Static analysis (cppcheck)
./bin/ci-check                      # Full code-change checks (see contributor workflow)
./bin/clang-format-fix               # Format (CI uses clang-format 21+)
python3 scripts/debugging_monitor.py # Enhanced serial monitor
```

## The Map — where to look next

| Topic | Read this when… | Doc |
|---|---|---|
| Hardware & RAM budget | Allocations, strings/vectors, SPIFFS writes, the Resource Protocol | [docs/engineering/hardware-constraints.md](docs/engineering/hardware-constraints.md) |
| Memory & allocation | `new`/`malloc`/`makeUniqueNoThrow`, smart pointers, RAII | [docs/engineering/memory-and-allocation.md](docs/engineering/memory-and-allocation.md) |
| ESP32-C3 pitfalls | ISRs/IRAM, `string_view`, alignment, templates, JSON | [docs/engineering/esp32-pitfalls.md](docs/engineering/esp32-pitfalls.md) |
| Build system & flags | PlatformIO, build envs, critical flags, local overrides | [docs/engineering/build-system.md](docs/engineering/build-system.md) |
| Architecture & patterns | HAL, singletons, activity lifecycle, FreeRTOS, fonts | [docs/engineering/architecture-and-patterns.md](docs/engineering/architecture-and-patterns.md) |
| Coding standards | Naming, header guards, error handling | [docs/engineering/coding-standards.md](docs/engineering/coding-standards.md) |
| UI, orientation & input | Rendering, button mapping, UITheme, `tr()` | [docs/engineering/ui-and-input.md](docs/engineering/ui-and-input.md) |
| Generated files | HTML, i18n, fonts produced by build scripts | [docs/engineering/generated-files.md](docs/engineering/generated-files.md) |
| Testing & debugging | Build/monitor commands, crash playbook, verification, CI | [docs/engineering/testing-and-debugging.md](docs/engineering/testing-and-debugging.md) |
| Git workflow | Repo detection, branching, commits | [docs/engineering/git-workflow.md](docs/engineering/git-workflow.md) |
| Cache management | Cache structure, invalidation, format versioning | [docs/engineering/cache-management.md](docs/engineering/cache-management.md) |
| Unified languages & Chinese support | Runtime content profiles, embedded CJK fonts | [docs/engineering/chinese-build.md](docs/engineering/chinese-build.md) |
| Firmware releases | Stable/Nightly targets, packaging, GitHub/COS indexes, rollback, OTA contracts | [docs/engineering/firmware-release.md](docs/engineering/firmware-release.md) |
| Device variants | Shared X3/X4 image, separate S3 builds, hardware validation limits | [docs/engineering/device-variants.md](docs/engineering/device-variants.md) |
| System overview & dataflow | Runtime lifecycle, activity model, pipeline diagrams | [docs/contributing/architecture.md](docs/contributing/architecture.md) |
| Binary file formats | Byte-level cache/notes/font formats | [docs/file-formats.md](docs/file-formats.md) |
| i18n system | Translation workflow in depth | [docs/i18n.md](docs/i18n.md) |
| Web server API | HTTP/WebSocket endpoints | [docs/webserver-endpoints.md](docs/webserver-endpoints.md) |
| Scope & governance | Whether a feature belongs in the project | [SCOPE.md](SCOPE.md), [GOVERNANCE.md](GOVERNANCE.md) |

---

Philosophy: Reading comes first. Lightweight apps, analytics, standby customization, and on-demand services are part of CrossMux. New features must justify RAM, Flash, power, and maintenance costs; use [SCOPE.md](SCOPE.md) to evaluate them.

## 52. 晋江客户端私有化（Tier 3B）：边界、构建与 dev/public 同步

晋江（JJWXC）的具体实现已从公共源码树移出到 `src/jjwxc/`（私有），核心只保留
`src/booksource/` 的接口与空桩。完整契约、构建参数与验证命令见
[`docs/engineering/private-book-source.md`](docs/engineering/private-book-source.md)。

要点：

- **公共侧**：`src/booksource/BookSource.{h,cpp}`（类型 + 注册表）、`RowTail.h`（纯映射，有宿主测试）、
  `PsramAllocator.h`；阅读器保留源相关方法的**声明**与无组件时的空桩；`JjwxcCommentSidecar.h` 是
  离线计数格式，仍属公共。i18n 键、图标、AppId 按用户要求保留公开。
- **私有侧**：`src/jjwxc/{app,protocol,test}`，其中 `JjwxcSource.cpp` 是唯一的注册符号
  `booksource::privateSource()`。`JjwxcReaderMethods.cpp` 承载原 `EpubReaderActivity` 的晋江方法与
  三个调试入口；`contentsFilterable`/`rebuildContentsRows`/`contentsTocOfRow` 等**通用**目录工具留在公共。
- **开关**：dev 树 `[s3_ble_psram]` 定义 `-DCROSSPOINT_BOOKSOURCE_JJWXC=1` 并加
  `-Isrc/jjwxc/app -Isrc/jjwxc/protocol/src`；public 树**不定义**该宏，`[base]` 的
  `build_src_filter` 里 `-<jjwxc/>`，目录在 `.gitignore` 中。
- **私有仓**：<https://github.com/Yana-Hangabina/cpyos-jjwxc>（private，`main`，内容即 `src/jjwxc/`）。
  发布流水线用只读 deploy key（`secrets.JJWXC_DEPLOY_KEY`）把它 clone 到 `src/jjwxc`，
  再 `pio run -e readpico_jjwxc`；缺 secret 或 clone 失败会让发布**故意失败**。
  普通 `pio run -e readpico` 是公共/无源目标，CI 与普通 clone 都走它。
- **构建判据**：两种目标都必须 SUCCESS——
  `readpico`（dev，带私有组件）与 `readpico_nosource`（dev，验证公共回退）；
  public 树只有 `readpico`（等于 nosource）。public 镜像里
  `nm firmware.elf | grep -c JjwxcActivity` 必须为 **0**。
- **回归**：`src/BookSourceContractTest.cpp`（37 项：状态编号、图标枚举编号、无组件时全部
  helper 惰性、`rowTailFor` 全状态映射）必须绿；15 套协议/存储宿主测试（crypto/protocol/
  unpurchased/cookies/discover/favorites/progress/purchase/comments/devauth/bookcrypto/
  saybody/client/login/cachequeue）在 `src/jjwxc/protocol` 新路径下全部通过。
- **同步纪律**：dev 是唯一开发源；public 只提交公共侧，**绝不提交 `src/jjwxc`**。
  跨树改动拆成"公共接口/核心"与"私有实现"两半，公共侧必须能单独构建并通过契约测试。
  发布带晋江 App 的固件由 public 发布流水线（`readpico_jjwxc`）产出。
- **历史清理（2026-11-04）**：public 的 `cpyos` 历史已重写，从每个提交中剔除
  `lib/JjwxcApi`、`src/activities/apps/jjwxc`、`src/TlsProbe.*`、`src/JjwxcCacheQueueTest.cpp`；
  0.1.0–0.2.3 的 tag 与 release 已删除，旧资产备份在 `artifacts/legacy-releases/`。
  不可达对象可能仍留在 GitHub 存储与 fork network 中，直到 GitHub GC 或 Support 清理。

## 53. 0.4.0：网页阅读、单一工具抽屉与褪色遮罩

本地 dev 树（`feat/jjwxc`，`cacdf678`）在 0.3.0 之后新增了浏览器合并提交与两次弹窗表面修正。
本次把它们同步进 public 并发布 **0.4.0**（tag `cpyos-0.4.0`，commit `43f519b4`）。

### 同步范围

- **网页阅读 App**：新增 `lib/WebBrowser/`（URL、HTML 扫描、正文提取、分页、编码、书签/历史）、
  `src/activities/apps/webbrowser/`、`src/AppCatalog.{h,cpp}`、`test/webbrowser/`（176 项宿主检查 +
  2 个变异步骤）、`docs/engineering/web-browser.md` 与产品设计文档。
- **单一工具抽屉**：删除 `AppsMenuActivity`，`MyActivity` 改为 4 列应用网格，入口统一到
  `AppCatalog`；`AppVisibility` 追加 `WebBrowser = 18`（持久化位不复用），
  `AppVisibilitySettingsActivity` 改读目录。
- **弹窗表面**：`GfxRenderer::fillRectScrim` 由加黑点改为**褪色**（保留四分之一墨迹），
  `BaseTheme::drawModalCardFrame` + `CpyTheme` 的 2 px 实线描边；等待卡/消息卡/选项弹窗共用。
- **`src/main.cpp` 手工合并**：加入浏览器串口钩子与 `APPS → MyActivity(...,1)`；
  保留 public 独有的 `OTAPROBE`/`OTAINSTALL`、`STANDBY → goToReaderStandby`、`/BOOKS/test.*` 路径。
- **i18n**：新增浏览器文案，pack **1672 键 / 334,397 B**；数据分区布局不变
  （pinyin `0xe70000` / web `0xec6000` / hyphen `0xf00000` / i18n `0xf70000` / emoji `0xfd0000`）。
  非中英界面必须完整安装或重刷数据分区。

### 构建与发布

- 本地 `readpico`：RAM **91,072 B (27.8%)**、Flash **7,051,026 / 7,536,640 (93.6%)**。
- 本地 `readpico_jjwxc`：RAM **94,632 B (28.9%)**、Flash **7,342,778 / 7,536,640 (97.4%)**；
  本地镜像的 `app_desc` 版本仍是共享 `libesp_app_format.a` 的旧串，以 CI 为准。
- CI Release `41833061263` 成功：`app_desc project=crossmux version=cpyos-0.4.0`（无 `-dirty`），
  `readpico-firmware.bin` = `readpico-app1.bin` = **7,348,416 B**，
  sha256 `c4b920f2a8cacd9989320c39429dea0558c18f08539aa93fbf5fe8e97a666acb`，
  app0 余量 188,224 B；18 个资产。
- `manifest-upgrade.json` / `manifest-app.json` 同时写 app0 `0x10000` 与 app1 `0x740000`，
  并写五个数据包；`manifest-install.json` 仍是完整安装语义（bootloader/partition/boot_app0/app0）。
- **Pages 同 commit 去重**再次出现：release 的 deploy 与 branch Pages 都指向 `43f519b4`，
  live `latest.json` 停在 0.3.0。按既有做法推送 site-only `a484c042`
  （静态版本占位改为 0.4.0），Pages run `41838208992` 成功后 live `latest.json` /
  `ota/readpico.json` 均为 0.4.0，下载清单与站点功能卡同步。
- 用户明确要求**推送后不再做本地/真机验证**，因此本轮没有刷机；设备仍停留在用户手上的版本。

## 54. 0.5.0：章评、单行阅读器标题栏与界面细节

### 同步范围（dev → public）

- **私有组件**：`src/jjwxc` 已更新并推到 `Yana-Hangabina/cpyos-jjwxc`（commit `aef2b17`，
  章评与购买抽屉细节）。发布流水线从该仓 clone；公开树仍不包含 `src/jjwxc`。
- **章评**：阅读器 More / 经典菜单 →「章评」；`paragraphId == 0` 复用 `JjwxcCommentActivity`
  与 JJD1，缓存 `comments/<chapterId>-0.bin`，不提供写章评。
- **阅读器**：单行标题栏，进度百分比移到页脚波浪条右端。
- **工具抽屉**：三列，5 个汉字的「晋江文学城」不再截断。
- **休眠**：超时休眠即使选了 Quick Resume 也显示正常睡眠画面；`goToSleep()` 会等待 Sleep
  Activity 真正成为当前页，并在失败时直接画默认睡眠面，避免把旧页面留在屏上。
- **其他界面**：Wi-Fi 信号格/锁图标在右边距内对齐；等待卡按内容收缩；emoji 子集扩到 250 个
  码位且字号切换失败时保留旧面；设置选择器箭头、弹窗文字轴与底部留白、购买抽屉禁用按钮和
  摘要字号修正。
- **字体资源**：系统 TTF/def 增加 U+2039/U+203A 等箭头别名；emoji TTF 126,512 B；
  `scripts/build_emoji_subset.py` 与 `scripts/tests/test_emoji_pack.py` 随仓库提交。
- **README / 官网重写**：只说相对 CrossMux 上游的新增与改进，去掉许可证与设计理念段落；
  官网只保留功能、界面、设备、刷机与致谢。
- **i18n**：pack **1675 键 / 334,398 B**，数据分区尾部剩 4,528 B，布局不变；
  非中英界面必须完整安装或重刷数据分区。

### 构建与发布

- 本地 `readpico`：RAM **91,072 B (27.8%)**、Flash **7,053,898 / 7,536,640**；
  `readpico_jjwxc`：RAM **94,632 B (28.9%)**、Flash **7,347,550 / 7,536,640**。
- CI Release `41975232407` 成功，18 个资产；`app_desc project=crossmux version=cpyos-0.5.0`
  （无 `-dirty`），`readpico-firmware.bin` = `readpico-app1.bin` = **7,353,200 B**，
  sha256 `d2aa38a5120bdc6e33978d810f21b3fa962653c328a9b70d0c8c0a1fb757eaef`，
  app0 余 **183,440 B**。
- `manifest-upgrade.json` / `manifest-app.json` 同时写 app0 `0x10000` 与 app1 `0x740000`，
  并写五个数据包；`manifest-install.json` 仍是完整安装语义。
- 分支 Pages 在同一 commit 先部署了 0.4.0，release 的 deploy 被去重；推送 site-only 提交
  触发 Pages 后，live `latest.json` / `ota/readpico.json` 更新为 0.5.0。
- 按既有要求，推送后不做本地/真机验证。

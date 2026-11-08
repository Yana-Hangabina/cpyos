# 上游清零计划（阶段 2：桶 3 复审 + SDK + 最终合并）

分支：`work/upstream-sync`（HEAD 见 git log）。审计全文：
[`2026-11-08-upstream-sync-audit.md`](2026-11-08-upstream-sync-audit.md)；
桶 3 逐项复审附录：[`2026-11-08-upstream-bucket3-reaudit.md`](2026-11-08-upstream-bucket3-reaudit.md)。

## 0. 现状

- 桶 1+2 已合入：113 项中 111 项落地（`f47a218b` 未做，`60d3c8bb` 待定）。
- 构建：`readpico` / `readpico_nosource` / `default`(C3) 全部 SUCCESS。
- 真机：已刷入本分支（app0+app1+五个数据包），启动 `0.7.0-cpyos`、i18n `1800 keys ... ready`，
  `exp/refresh_soak.py` **36/36**。
- GitHub 仍显示 `behind_by 285`（merge base `83b0f81`）；清零需要把 `upstream/main` 合进来。

## 1. 桶 3 复审结论（96 项：D_SKIP 88 + SDK_DEFER 8）

复审原则：不采信旧否决理由；逐条读 diff 与 CPYOS 当前实现，按"收益 / 影响用户 / 成本 / 风险 / 依赖"重判。
结果：**ADOPT_PARTIAL 14 · ADOPT_AFTER_SDK 2 · NEEDS_DEVICE_CHECK 2 · KEEP_REJECTED 70**；
SDK_DEFER 8 项另见 §3（其中 3 条结论被修正）。

### 1.1 复审后值得做（不依赖 SDK）

| SHA | 内容 | 结论 | 移植要点 |
|---|---|---|---|
| `0727f9f0` | 菜单/点按区的"刷新屏幕"在 16 级文字页被静默丢弃 | **PARTIAL（真 bug）** | `displayGrayscaleBase` 忽略 `nextRefreshOverridePending`；`commitGrayscale16` 只清 `FromGray` 变体。补 2 处晋升消费，真机验证 |
| `6b863e48` | 词典文件被删除后仍显示查词入口 | PARTIAL | `resolveBasePath` 为空时隐藏 `EpubReaderMenuActivity` / 阅读器入口 |
| `f0dfe5fc` | 受保护书打开失败没有原因 | PARTIAL（1 行） | `LOG_ERR("CPRO","Manifest open failed: %s", book->lastError())` |
| `58d9c776` | 页面级 toast 定时器互相截断 | PARTIAL | 保存/取消 `showMessage` 的计时句柄（CPYOS 同 bug） |
| `e529cadb` | 插件总开关 + 端点/目录门控 | PARTIAL | 只要 `pluginsEnabled` 开关与端点门控；hub/quarantine 不做 |
| `65e3f9d0` | OPDS / KOReader 自定义 HTTP 头 | PARTIAL | 取 `lib/HttpHeader` + `OpdsServerStore` + `HttpDownloader`；跳过 KOReader/clippings/web |
| `cb2a4c78` | 带 ruby / 悬挂标点的断行偏移 | PARTIAL | `computeHyphenatedLineBreaks()` 补 `calculateRubyExtra*Offset` 与 kinsoku（约 25 行；需 `SECTION_FILE_VERSION` 86/87 bump） |
| `057e16a4` | 网页资源压缩（实测 File Manager 55.4 KB→28.4 KB gzip） | PARTIAL | 先接 rjsmin；EPUB optimizer 按需加载留后 |
| `136c2655` | 许可证/来源声明补齐 | PARTIAL | 取 minibidi/expat/epdiy/AnimatedGIF 的 provenance 文本；跳过 workflows |
| `5789260c` | 两档 OFF/ON 枚举用 toggle 文案 | PARTIAL | 仅 `"toggle"` hunk（当前恰好一个设置） |
| `919ec426` | WeRead 时长上报文档与实现不符 | PARTIAL（文档） | 实测请求体无 duration 字段，纠正文档 |
| `a66d76f4` | 版本号只注入使用它的 TU | PARTIAL（构建） | `env.AddBuildMiddleware` 精确注入 `-DCROSSPOINT_VERSION`，避免改版本全树重编；含 git 输出引号/反斜杠清洗 |
| `7dc84ed6` | esptool 5.3.0 在 7.6% 处中止刷写的记录 | PARTIAL（文档） | 记录已验证版本；不要在未验证时升级 |
| `c8175b1f` | 交互式模拟器 CI 需 timeout | PARTIAL（CI） | 给 sim 步骤加 `timeout-minutes` |

### 1.2 依赖 SDK（SDK 阶段再做）

| SHA | 内容 | 说明 |
|---|---|---|
| `0a907772` | X3 BQ27220 设计容量 | **旧结论错误：X3 在售**（`xteink_x4` 目标含 `xteink_x3`，`FREEINK_DEVICE_X3=1`）。阻塞点仅 `BatteryMonitor::loadDesignCapacity()` 不在 96de1be |
| `327e7219` | 识别 AES-256-CBC 清单 | 旧理由"会坏"不成立（当前探测是 no-op）；真正缺的是 SDK `ProtectedBook` 的 aes256-cbc |

### 1.3 需真机判定

- `d5b6c74d`：未选遥控时保持 BLE 射频关闭（CPYOS 有自己的 BLE 输入，需实测功耗/连接行为）。
- `5a51bf32`：模拟器 HAL pin 的契约（CPYOS 自己的 HAL 改动需验证 sim 构建）。

### 1.4 维持拒绝（类别 + 修正后的关键理由）

- **上游 2.0.0 渲染重写 / source ranges / drop caps / 表格**：与 CPYOS 分页缓存与渲染层冲突；
  其中 `0305aaa6` 会改写 `SECTION_FILE_VERSION` 语义。
- **clippings / 批注同步**：CPYOS 用 `AnnotationStore`，上游路径的文件已不存在；如需短语查询应做 CPYOS 原生实现。
- **上游 BLE 翻页模块**：CPYOS 已有自己的 BLE 输入，重复造会引入第二个射频 owner。
- **只认陀螺仪的倾斜**：会删掉 Read Pico 的加速度计倾斜/摇晃。
- **1.1/1.2 行距、INX 主题、面板/前端灯产品差异、上游板卡**：产品取舍，非缺陷。
- **品牌/发布/CI/文档重写**：与 CPYOS 站点、tag/版本方案、用户指南冲突。
- **字体子集/NotoSerif/字体 ID 生成器**：CPYOS 用系统字体包 + FEXT1 渠道；上游脚本产出的资产到不了用户。
- 三处修正：`0a907772`（X3 在售）、`f0dfe5fc`（有真实 1 行价值）、`327e7219`（旧理由错误但仍需 SDK）。

## 2. Phase A：非 SDK 收尾（约 2–4 开发日）

顺序按"真 bug → 能力 → 文档/CI"：

1. `6b5566c8`（X3 power detection）：**现在可移植**，`isExternalPowerPresent()` 在 96de1be 已存在；
   app 侧只是删掉重复的 I²C 读取。
2. `60d3c8bb`（封面放大 + `BmpToBmpConverter`）：复审认为**大概率不是 SDK 阻塞**（其 SDK delta 是 BMI270），
   先试移植 app 侧，失败再归入 Phase B。
3. `f47a218b`（KOSync profiles + series/ISBN-ASIN，约 500 行）：决定"做/不做"。
   做则需 `SettingsList` 增行 + i18n 键；不做则在审计里写明"用户价值低/维护面大"的具体理由。
4. 桶 3 §1.1 的 14 项 PARTIAL：先做 7 个 bug/能力项，再做 7 个文档/CI/工具项。
   需要 cache 版本 bump 的只有 `cb2a4c78`；其余不动二进制格式。
5. 验证：三构建 + host 回归 + 真机 soak（刷 app1；数据包仅在 i18n 键变化时重刷）。

## 3. Phase B：freeink-sdk fork + 重移植（约 4–5 开发日 + 1 真机日 + 0.5–0.75 天基建）

### 选项对比

| 选项 | 工作 | 风险/代价 | 结果 |
|---|---|---|---|
| (i) 取 `d9a1304c` + 重移植 | 27/31 文件干净；12 hunks 手改（`FreeInkDisplay.cpp` 6/7、`EpdiyLcdDriver.cpp` 4/4、`.h` 1/2、`SdmmcBlockDevice.cpp` 1/5）；删 CFF 补丁 | 显示 facade/adapter 语义重写，需真机回归 | 拿到上游 SDK 改进，但仍是"36 脏文件"模式 |
| (ii) 留在 `96de1be` | 0 | 冻结 202 个 SDK 提交；**X3 灰度 SPI 死锁修复拿不到**；fail-fast DNS / TLS 缓冲改进拿不到 | 最省事，但技术债累积 |
| **(iii) fork + gitlink（推荐）** | (i) + fork/`submodule set-url`/CI/文档 | 同 (i) + 0.5–0.75 天基建 | 永久消除 `submodule update` 毁补丁的隐患 |

### 工作拆解（推荐 iii，内部按 i 执行）

1. 在 SDK `d9a1304c` 上应用现有补丁：27 文件直接过；手改 12 hunks：
   - `FreeInkDisplay.cpp`：把我们的 async start/finish、band diff、`idlePowerTick`、水波纹穿透、`GrayArea` 提交
     接到上游重写后的 facade（`displayGrayscaleBaseWithContext` 等）。
   - `EpdiyLcdDriver.*`：按新驱动契约重写 adapter（`displayCommitted`/`requestResync`）。
   - `SdmmcBlockDevice.cpp`：mount 循环局部 rebase；DMA 对齐 hunk 仍适用。
2. **删除 `patches/freeink-sdk-cff/cff-otf.patch`**：上游 CFF 更完整（cff+psaux+pshinter+psnames，
   `psintrp.c` 堆分配 `glyphPath`），保留 `FREEINK_FONT_ENABLE_CFF`；之后重测渲染栈（32 KB PSRAM 栈可能可降）。
3. fork `freeink-sdk`，提交我们的移植，改 `.gitmodules` URL 与 gitlink；`apply.sh` 变为只校验版本/幂等提示。
4. 解锁：`0a907772`（X3 电量）、`327e7219`（AES-256，若 SDK 有 cbc256）、`9ba67ea5`（连接前加头）、
   `ad08729e`（复审：对 CPYOS 板卡 inert，可不做）、X3 SPI 死锁、TLS 内存改进。
5. 验证：三构建 + 显示相关 soak（水波纹、列带、局部清理、分钟时钟、掉轨）+ 字体（CFF/OTF）+ SD 卡。
   **显示层改动必须看实屏**，串口只能证明不崩。

## 4. Phase C：最终上游合并（清零，约 1.5–2.5 开发日）

1. 在 `work/upstream-sync` 上 `git merge --no-ff upstream/main`（预计 150 冲突，清单已分析）。
2. 解决策略（与桶 1/2 相同）：
   - CPYOS 自有实现（渲染层、设置、主题、站点/CI、品牌文档）保留 ours；
   - 上游新文件按已接受范围保留 theirs；
   - `SECTION_FILE_VERSION` 保持 86/87，绝不采用 56/59；`9dfa92ba` 的 Section hunk 不取；
   - `freeink-sdk` gitlink：Phase B 未完成时解析为 ours（96de1be）；完成后指向我们的 fork commit；
   - i18n 只并键；`SettingsList` 只追加。
3. 构建矩阵 + host 回归 + 真机 soak；推送 `cpyos` 后 GitHub `behind_by` 归零。
4. 若期间上游继续前进，重新 fetch + 再合一次（增量很小）。

## 5. Phase D：发版收尾（约 0.5–1 天）

- `scripts/generate_data_manifest.py` + `verify_release_artifacts.py`；
- 数据分区 i18n/web 键变化时完整刷写；
- 私有 JJWXC 组件同步推到 `cpyos-jjwxc`；
- `cpyos-X.Y.Z` tag/Release/Pages 按 §31 执行；发布后核对 `latest.json`、OTA 清单与 SHA256。

## 6. 建议排期与决策点

推荐顺序：**Phase A（1–2 天）→ Phase C（清零）→ Phase B（SDK）→ Phase D**。
理由：先拿低风险收益并尽早清零；SDK 重移植是显示层高风险改动，放在清零之后独立回归，
避免把"历史合并"和"显示栈重写"混在同一次验证里。

需要拍板：

1. 桶 3 的 14 项 PARTIAL 是否全做？（建议：7 个 bug/能力项全做；文档/CI 项可顺带）
2. `f47a218b`（约 500 行）做还是明确不做？
3. SDK 选 (iii) 还是先 (ii) 冻结？
4. Phase C 是否现在就执行（清零 GitHub 计数），还是等 Phase A 收尾后再合？

## 7. 红线

- `SECTION_FILE_VERSION` 保持 86/87；任何二进制缓存格式变更必须 bump 并写 `docs/file-formats.md`。
- 只有在 SDK 重移植完成后才删除 `cff-otf.patch`；当前构建仍依赖它。
- 共享 PlatformIO 包只在必要时按 §6 配方改（本次已按配方修过 C3 ROM-libc sdkconfig）。
- 数据分区五个 blob 的偏移以 `build/data_manifest.txt` 为准，不写死。
- 上游合并与 SDK 重移植都不要在脏工作树/并发构建时做（§38）。

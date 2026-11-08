# 上游能力吸纳路线图（新标准：只拒绝“冲突且没必要”）

本文替代 `2026-11-08-upstream-kept-rejected-review.md` 的结论部分。判断标准改为：

- **无冲突且有用** → 直接吸纳；
- **有冲突但有用** → 设计“无冲突吸纳”或“取长补短”的路线，而不是整包拒绝；
- **只有又冲突、又没必要**（例如围绕已删除的 INX 主题、或会删掉 CPYOS 刻意保留的功能）才彻底拒绝。

覆盖：上一版 70 项 `KEEP_REJECTED` **70/70 全部重审**（含此前遗漏的 `186d9407`/`12e441f7` 封面中间态、
`8fc2b661` 陀螺仪、`fecee610` 上游 BLE 模块）。详细论证见
[`2026-11-08-upstream-absorption-designs.md`](2026-11-08-upstream-absorption-designs.md)。

**用户已定（2026-11-08）**：Phase 0 等你发令后再开始；新增阿塞拜疆语；圆角按**内圆角**处理；
排版 **M1–M6 全做**；插件兼容性栈排入；执行顺序见 §6。

## 0. 总览

| 主题 | 原拒绝数 | 新处置（概要） |
|---|---:|---|
| EPUB 排版引擎（2.0.0） | 6 | **分阶段吸纳能力**：CSS 级联/选择器、white-space、分页/孤行、边框、块级字号、逐词字体、drop caps；不整包合并 |
| 批注/剪贴 | 3 | **取长补短**：保留 AnnotationStore；新增“长按选中 → 查词”入口；拒绝 clipping 子系统与 8 B/词 source ranges |
| 封面缓存中间态 | 2 | 终态已采用；`186d9407` 的“失败重试”需求转为 RAM 失败备忘（plan-later）；`12e441f7` 无动作 |
| 阅读器交互/设置 | 23 | 1 立即、12 部分吸纳、7 计划后来、**3 真拒绝**（长按菜单、读时 INVERTED_TAP、CPY 行内边距） |
| 板卡/外设 | 5 | 1 立即（陀螺仪能力查询）、1 部分（BLE 三个机制）、**3 板卡代码拒绝** |
| 字体/i18n | 11 | 7 进入 P0/P1 设计、2 条件性、**2 真拒绝**（1.1/1.2 行距、Ubuntu 小字号） |
| 网络/插件/Web | 7 | 1 立即（PR 固件链接）、2 部分（插件错误态/单次加载 JS、reserve 上限）、4 计划后来 |
| 构建/发布/文档 | 13 | 1 立即、5 部分（规则/文档）、3 计划后来、**4 真拒绝**（品牌/版本/发布产物） |

**真正“冲突且没必要”的约 20 项**，集中在：INX 主题相关（`ec32b959`/`8b4c0d6e` 的 INX 部分）、
会删掉 Read Pico 现有功能的 `471243b2`/`e41982a4`/`3f8345c5`、未发货板卡
（`261726f6`/`fb2a8a8f`/`809c1dab`）、CPYOS 自有品牌/版本/发布文档、已被 CPYOS 更好的实现取代的
若干字体/封面项。其余都有明确路线。

---

## 1. EPUB 排版引擎：分阶段吸纳，不换模型

上游 `0305aaa6` 是 45 文件、+4.9k 行的模型重写；CPYOS 的差距约 2.5k 行。**不整包合并**，而是把能力
拆成四层，每层都能独立验证：

### 1.1 能力清单与冲突（摘要）

| 能力 | CPYOS 现状 | 吸纳层级 | 关键冲突 |
|---|---|---|---|
| 复合/ID/后代选择器、specificity、`::first-letter` | 只有 `tag`/`.class`/`tag.class`，显式 TODO | **L1** | CSS 缓存 13→14；**必须同时** bump Section 89/88（Section 头不带 CSS 版本） |
| `white-space`/`<pre>` | 空白无条件折叠 | **L1** | 改变分词→所有度量变化 |
| `page-break-before/after` | 无 | L2 | 页边界变化 |
| 孤行/寡行、标题 `keep-with-next` | 无 | L2 | 必须在 `applyVerticalBottomAlign` **之前**执行；同时维护 `currentPageLinkLines` |
| 边框/底纹 | 无 | L2 | 新 `PageBorderBox` 标签；竖排需跳过 |
| 表格列宽预量（宽表） | 仅 4 列、32 词/格 | L2 | CPYOS 流式 Expat，需要缓存当前 `<table>` 子树（≤8 KB）；C3 保留 512 B/格 |
| 块级 `font-size`（标题/`.title`） | 整章单一字号 | **L3a** | 行高按块；`TextBlock` 序列化 +4 B/行 |
| 逐词字体 run | 无 | **L3b** | CPYOS 的 TextBlock 是**单一 arena**；上游的第二个 `wordFontData` 分配会破坏该不变量 |
| small-caps | 无（style bit 128 空闲） | L3c | 渲染层 `drawTextRun` 加 3/4 advance |
| drop caps | 无 | L3d（低优先） | 需要 4-bit 覆盖度缩放；竖排跳过；选区命中需文档化限制 |
| source ranges | 行级 `visibleOffset` | **L4 可选** | 上游 8 B/词；CPYOS 建议按 run 编码（1.5–2.5 B/run） |

### 1.2 路线与里程碑

- **M1（3–4 天，最高性价比）**：L1 选择器/级联/`::first-letter` 存储 + `white-space`/`<pre>`。
  只动 `css/CssParser`、`ChapterHtmlSlimParser`、`CssStyle`，不改 Page 模型与渲染器。
  第三方 EPUB 的 `.body p`、`div.chapter > p` 缩进/对齐立即生效。缓存版本：CSS 14、Section 89/88。
- **M2（+3 天）**：L3a 块级字号（标题、`<small>`、`.title{font-size:2em}`）。
- **M3（+5 天）**：L2 分页 + 孤行/寡行 + keep-with-next。先于底部对齐运行。
- **M4（+5 天）**：L2 边框/底纹 + 宽表预量。所有 `TAG_PageLine` 遍历点必须跳过新标签。
- **M5（+8 天）**：L3b 逐词字体 + L3c small caps。逐词槽位放进现有 arena（`+numWords` 字节），
  不引入第二个分配；所有度量函数（`measureWordWidth`、断行、ruby 偏移、逐行 erase）同步改。
- **M6（+5 天，已确定纳入）**：drop caps。16 级画布用 4-bit 覆盖度重采样；竖排禁用。
- **L4（5–7 天，可选）**：按 run 的 source ranges，典型 <200 B/页；当前没有任何功能依赖它。

“完整渲染能力（不含 drop caps）”约 **24 开发日**；含 drop caps 约 29。

### 1.3 明确不取

- 上游 `Section`/`Page`/`TextBlock` 整文件与 56–59 的版本号；
- `TextBlock::wordFontData` 第二个分配（破坏单一 arena）；
- 上游 `ParsedText` 的 `void` 回调签名（CPYOS 用返回 `bool` 提前停止）；
- 字体 variant LRU 注册表（CPYOS 已有每字号稳定 id，只需 ~40 行 provider）；
- 竖排里的边框/drop caps、通用 float 引擎、C3 上 2 KB/行的表格缓冲；
- `072b6fc7`/`88ea9bcf` 的行距模型（CPYOS 保留 4 档+按字族系数；百分比只作为可选覆盖）。

---

## 2. 字体：补齐能力，而不是替换资产管线

先纠正三个前提：系统包**已含** 66 个西里尔、48 个希腊字母与 `№`；`NOTOSERIF_*` 不是悬空 ID
（`main.cpp`/`SystemFontPack` 注册了真实面，只是 Serif 选项被 `ENABLE_CHINESE_VERSION` 隐藏）；
`98da1782`/`c6a2e311` 的修复已以 compacted 形式存在。

### 2.1 真正的缺口与吸纳

| 缺口 | 设计 | 代价/收益 |
|---|---|---|
| **字体包内容变化后分页不失效**：reader ID 是固定字面量，换 `.def` 后 `Section` 仍按旧 `fontId` 命中 | 新增 `scripts/build_font_ids.py` 统一生成所有 ID；`CJK_PACK_SIGNATURE = crc32(CPYOSHanSansSC.ttf) & 0xFFFF` 折进 `CJK_READER_SIZE_FONT_ID(pt)` | 0 flash/RAM；首次刷机后一次性重建分页；以后换字体不再手 bump Section 版本 |
| **稀有脚本 UI 面尺寸错误**：`loadSupplementFonts` 固定 12/14/16pt，不跟 `uiFontScale` | 按 `typography::pointsFor(scale)` 为每个活跃字号建面并挂到 UI 角色 | PSRAM +20–30 KB（装 world-scripts 时）；修复 Compact/XL 下 supplement 字过大/过小 |
| `Ə/ə` 缺失（阿塞拜疆语） | 在系统包里合并两个轮廓（不改 `chars_8105` 的 8,105 断言），新增 `chars_extra.txt`；**已确定新增 `az` 语言** | app0 +~1 KB、数据分区 +~10 KB；**需完整刷数据分区** |
| 字体 ID 三处来源、Ruby 生成器、无 `--check` | 统一到 Python 生成器 + `test_font_ids.py`；保留 `CJK_UI_*`/`CJK_READER_SIZE_FONT_ID(pt)` 宏 | 0 运行时开销；消除漂移 |
| 行距只有 4 档 | 增加可选 `readerLineSpacingPercent`（0=off），保留按字族系数 | +1 键 ×35 语言；section 头已含 `lineCompression`，无需缓存改动 |
| 未使用的 16 个 NotoSerif 头文件 | 删除头文件与生成器输入；保留 ID 别名（兼容旧设置与紧急回退链） | 仅仓库体积；不动用户可见行为 |
| Libron 字体目录 | 只在 `sd-fonts.yaml` 增加条目（数据），不接第二套 `fonts.json` | 0 固件成本；只有服务端 manifest 重建后可见 |

### 2.2 不取

- 重新生成 38 个位图头文件来补 `Ə/ə`/西里尔（Read Pico 用系统包，改动到不了用户）；
- `fc2de152` 整包（删头文件+ID 会破坏 ID 生成与设置迁移）；
- `2648933a`（CPYOS 没有 CSS font-size 前提，收益为 0）；
- `ce781eb8` 原样（预热的是内置位图解压器，系统包路径不用它）；
- `cf50ce73`（ReadPico 的 SMALL 已重绑到包，换 Ubuntu 只影响 C3 且改名）；
- `88ea9bcf`（会把 CPYOS 刻意的 1.3/1.6 行距改回 1.1/1.2）；
- 第二套 `.cpfont` 目录、SD 家族 content-hash ID（会在每次复制文件时改 ID）。

### 2.3 顺序

- **P0（≈1.5 天）**：字体 ID 生成器 + 包 CRC；supplement 面跟随 `uiFontScale`；删死头文件/死 helper；
  `Ə/ə`+`az`（已确认新增）。
- **P1（≈2 天）**：行距百分比覆盖；Libron 数据；C3 世界语言 UI 探针清理（仅在有需求时）。
- **P2**：CSS `font-size`（依赖排版 L1/L3a）；TTF 预热 + 列表扫描（需先有 `TtfEpdFont::prewarmText`）。

---

## 3. 批注/剪贴：保留 CPYOS 存储，补“短语查询”

CPYOS 的锚定其实**强于**上游：注解存 `spineIndex` + 章内可见码点范围，重排后自然落到同一段文字；
上游还要 `layoutSignature` + 页/词序回退。因此不引入 source ranges 也能做短语查询。

### 3.1 真实缺口

1. 选区只有 `[删除/高亮][批注]` 两个胶囊，没有“查词/查短语”入口；
2. 行内偏移漂移：`layoutPageWords()` 只累加词内码点，解析器却把空白也计入了偏移；
3. `quote` 拼接无分隔符（英文变成 `thequickbrown`）；
4. 选区只限单页；`quote` 120 B 截断。

### 3.2 设计：第三个胶囊

- 新选区：`[查词][高亮][批注]`，查词为默认（最常用，也受防双击保护）；
  编辑已有注解时保持 `[删除][批注]`（不提供查词，避免用截断的 quote）。
- 复用 `STR_LOOKUP`（31/35 语言已翻译），不新增按钮文案。
- 数据流：长按选区 → 点“查词” → `SelectionText` 组装文本 → `endSelection()` →
  以**子 Activity** 打开 `DictionaryWordSelectActivity` 的 `PhraseQuery` 构造 → 直接查词 →
  进入定义页（递归查词可用）→ Back 回原页原段落。位置不丢，因为子 Activity 是 push 不是 replace。
- 文本规则（纯函数 `src/util/SelectionText.h`）：段落边界 `\n`、有间隔 `' '`、粘连不加；
  去掉行尾 discretionary hyphen、em-space/U+00A0 前缀；按 token 边界截到 64 B（避免 C3 OOM）。
- 候选阶梯：Latin 整短语→最长词→锚点词对（≤3 次）；CJK 整串→4/3/2 字窗口（≤4 次）。
  命中回退时在定义页顶部提示“短语未收录，已查：<headword>”（新增 1 个键）；全未命中保留
  NotFound 弹窗，Confirm 再打开选择器让用户点词。
- 最小选区：≥2 个可见码点且至少一个 token 经 `cleanWord` 后非空。

### 3.3 阶段

- **Phase A（1.5–2 天）**：第三个胶囊 + `SelectionText` + `PhraseQuery` 构造 + 候选阶梯 + 宿主测试
  （含变异：去掉 CJK 间距规则必须变红）+ `CMD:PHRASE` 真机脚本。
- **Phase B（2–3 天，仅在实测到漂移时）**：`TextBlock` 每词 1 B `gapBefore`（arena 5→6 B/词），
  重建偏移；Section 89/88。比上游 8 B/词便宜 8 倍。
- **Phase C（0.5 天，可选）**：注解列表“再次查词”（必须从实时页面按范围取文本，不能用截断 quote）。

### 3.4 不取

- `ClippingStore`/`ClipSelectionActivity` 整套（与 AnnotationStore 重复、第二套磁盘格式）；
- `TextBlock::SourceRange` 8 B/词（arena +160%）；
- 3 页预载窗（只有在做跨页选区时才有意义）；
- KOReader clipping 同步（独立功能）。

---

## 4. 圆角与设备抽象：先把地基做对

### 4.1 事实（含用户照片确认）

- **用户提供的实机照片确认：需要处理的是“内圆角”** —— 即白色边框与显示区之间的开孔圆角，
  不是机身外轮廓。内容（状态栏左右簇、页脚、Tab 端点）靠近的是这个内圆角。
- Read Pico 的 `viewableInsets = {5,5,8,5}`，但 `cornerRadius` 从未配置（默认 0）。
- 对仓库内真实设备照片的边框开孔做圆拟合，估计 **r ≈ 30 面板像素（20–45）**；
  用户照片与之相符：内圆角可见，且只影响最底部/最侧边 8–12 px 的行（切角 ≤10 px）。
- SDK `d9a1304c` 的 `BoardConfig` **已带** `cornerRadius`/`bottomCornerRadius`/`viewableInsets`，
  且我们的补丁不碰 `BoardConfig.h` → **无需 SDK 改动**。
- 用户照片只作为判断依据，**不提交进仓库**（项目规则：设备截图不入库）；精确数值仍需一次
  1:1 标尺照片，放进 `artifacts/` 并记录在 `docs/engineering/read-pico.md`。

### 4.2 抽象

- `lib/GfxRenderer/PanelInsets.h`（纯函数）：`cornerCut(radius, depth)`、`sideClearance(inset, radius, depth)`。
  比上游的 `max(margin, corner)` 精确：30 px 半径不会把页脚整体推内 25 px。
- `UITheme::getCornerRadius()/getBottomCornerRadius()` 暴露板级数值；app 不直接读 `BoardConfig`。
- **语义是“内容区内圆角”**：clearance 作用在显示区四角（不是机身外角），上下半径可不同
  （`bottomCornerRadius` 缺省回落到 `cornerRadius`）。
- 消费点：`BaseTheme::drawStatusBar` 的左右簇、阅读器页脚 `readerChromeMetrics`、CPY 标题带/底栏端点、
  书架位置读数、Toast。全部写成 `max(existing, cornerCut(...))`，**radius=0 时逐字节不变**。
- 不移植 `BaseTheme::headerBand()` 原样（它会二次内缩 CPYOS 已按安全区传入的 rect）；
  只取 `headerBand`/`headerStatusHeight` 的单源几何与状态栏 clearance，坐标仍用 CPYOS 的。

### 4.3 阶段

- **(a) 1 天**：抽象 + 零视觉 delta。验收是 Home/书架/阅读器/设置的截图 A/B **逐字节相同** +
  `refresh_soak` 36/36。
- **(b) 0.5 天 + 一次拍照**：在玻璃上测真实半径（画 1 px 标尺 + 全刷拍照），再把数值写进
  `READ_PICO.cornerRadius`（这是 SDK 工作树补丁，需并入 `d9a1304c-local.patch`）。若测得 ≤8 px，
  记录 0 并停止（切角 <1 px，低于面板自身容差）。
- **(c) 未来设备**：profile + env + 启动时 `selectDevice`，只需填半径/inset，页面代码不再改。

### 4.4 不取

- 未发货板卡的驱动代码（Picco/Sticky/BOOX/LilyGO/EEGO）；
- `headerOffsetY`/`statusBarOffsetY`/`tabBar*Pct`（所有在售板都是恒等值）；
- ★ `fb2a8a8f` 的 `Frontlight.present()` 门控——Read Pico 的 `present()` 恒为 false，
  会删掉触控板上的控制中心；
- “因为用户说有一点圆角就把 radius 猜成 8”。

---

## 5. 其余项的新标准复核（摘要）

### 5.1 UI/设置（23 → 3 真拒绝）

- **立即**：`43694c90` KOSync 服务器类型改用 `OptionPopup`（现在是无提示三态循环）。
- **部分吸纳**：`5407ffcd` 只把散落的板级过滤收进 `isSettingAvailableOnBoard`；`b823d54b` 只取
  “getter/setter 分支先于 valuePtr”的守卫；`c7996b9f`/`35dccb87`/`e9635421` 取“计数自检 + 批次≤4”
  的不变量（CPYOS 自己的 `test_settings_api_keys.py` 正因这个缺陷失败）；`ec32b959` 只取
  “CPY 不画焦点环时跳过整页重绘”那一个 hunk；`8f4d035c` 只取时钟可用性过滤；
  `01f09e0e` 取 `headerBand`/状态栏 clearance（与圆角设计合并）；`cac12269` 取 ctor/dtor 外移；
  `4ef42ee1` 给 CPYOS 自己的调试块加 release 守卫；`052d481a`/`5eb498b0` 写成 CPYOS 自己的测试。
- **计划后来**：`87fc4ab7`（设置分组）、`8b4c0d6e`（CPY 版状态栏 overlay）、`b2b1e15a`（前端灯图标，
  先量 35 语言是否溢出）、`944b741e`/`31480212`（统一 clang-format 21 pass）、`d52fc820`（先迁移
  4 个手写命中面）、`f3ace194`（按 CPYOS 的封面缓存重新定表）。
- **真拒绝**：`471243b2`（会在触控板上删掉长按菜单）、`e41982a4`（CPYOS 的读时 INVERTED_TAP 已解决）、
  `3f8345c5`（CPY 行内边距是刻意的）。

### 5.2 网络/插件/Web + 构建/发布（20 → 10 真拒绝）

- **立即**：`59912ea9`（CPYOS 的 `pr-firmware-links.yml` 与上游父版本逐字节相同，而 `ci.yml`
  只上传 `firmware.bin`，导致每个 PR 都误报“Firmware is not available”）。
- **部分吸纳**：`d5d0bea1` 先加 `MAX_BROWSE_LISTS` 边界（CPYOS 的 `reserve()` 目前无上限）；
  `c9f64d0d` 取 `loadPlugins` 的 `ok/Array.isArray` 错误态 + `PluginHost.js` 只加载一次；
  `e7b47ee5` 把 `ci.yml` 的 push 分支加上 `cpyos`；`997b4331` 把两条发布规则写进文档；
  `bb855ef9` 在 CPYOS 的 `SCOPE.md` 写 6–8 行 downstream 条款。
- **排入 Batch 2（已确定）**：插件 capability/list-index 栈（先建 `FirmwareIdentity` 词汇表，
  再 `d5d0bea1` 映射、`e93f8d1f` 合并行为、`861683cc` requires 过滤）。
- **计划后来**：`a722472c`（出现出版详情页时的一行规则）、`414b3797`（统一测试桩的 RenderLock 生命周期）。
- **真拒绝**：`55a77c2e`（CPYOS 的 select 保持原生箭头）、`18d611f2`（文件不存在且同类修复已在）、
  `35072257`/`6ec32892`/`a6db8f15`/`91ed3ef8`（README/版本/发布产物归 CPYOS）、
  `c17b80ab`（未发货硬件）、`6d2cd3a1`/`3607ea7b`/`d1f5d1ec`（上游已回退的规则树，CPYOS 已有序）。

### 5.3 板卡/外设与剩余 4 项

- **立即**：`8fc2b661` 用 `Imu::hasGyroscope()` 取代板宏判断（SDK d9a1304c 已提供），
  **保留**加速度计倾斜/摇晃。
- **部分吸纳**：`fecee610` 只取三个机制——30 s 空闲关射频、启动重试 5 s×6 次（避免每 2 s 释放
  SD 字体缓存）、切屏/睡前先关射频；模块本身不要。
- **计划后来**：`186d9407` 的“失败重试”需求 → `BookCoverLoader` RAM 失败备忘（不写 0 字节哨兵）。
- **无动作**：`12e441f7`（纯回退，终态已由 `3db7a96b` 取代）。
- **真拒绝**：`261726f6`/`fb2a8a8f`/`809c1dab`（无硬件可验证），以及 `fb2a8a8f` 的前光灯门控。

---

## 6. 已确定的执行顺序（用户 2026-11-08 拍板）

已确认的决定：

- **Phase 0 暂缓**，等你明确发令后再开始；
- **新增阿塞拜疆语**（`Ə/ə` + `az`）；
- 圆角按**内圆角**处理（内容区开孔），不是机身外角；
- **排版 M1–M6 全做**；
- **插件兼容性栈排入**计划。

在 Phase 0 发令后，按下面的依赖顺序执行（不是简单按编号）：

### Gate 0 — 等你发令

### Batch 0A：基础与不变量（无布局风险）
1. 字体 ID 生成器 + `crc32(ttf)` 折入 `CJK_READER_SIZE_FONT_ID(pt)`（先做，避免与后续 Section bump 混在一起）。
2. 设置容量自检 + 批次≤4（修 `test_settings_api_keys.py` 的既有失败）。
3. UI/网络速赢：`43694c90`、`5407ffcd`、`b823d54b`、`cac12269`、`4ef42ee1`、
   `8f4d035c`（仅时钟可用性）、`59912ea9`、`e7b47ee5`、`d5d0bea1`（仅边界）、`c9f64d0d`（两个缺陷）。
4. 陀螺仪能力查询 `8fc2b661`；BLE 三机制 `fecee610`。
5. 测试补齐：`5eb498b0`、`052d481a` 写成 CPYOS 自己的 suite。

### Batch 0B：内圆角与安全区（数据 + 零 delta）
6. `PanelInsets.h` + `UITheme` accessors + 5 个消费点；radius=0 时截图逐字节 A/B。
7. 用 1:1 标尺照片确定内圆角（用户照片已确认“有内圆角”；数值测出前保持 0）。
8. `headerBand`/`headerStatusHeight`（`01f09e0e`）与上述合并。

### Batch 0C：短语查询
9. `[查词][高亮][批注]` 第三胶囊、`SelectionText`、`PhraseQuery`、候选阶梯、宿主与真机测试。

### Batch 0D：语言与字体数据
10. `Ə/ə` 合并进系统包 + `az` 语言 + i18n pack 重刷（完整数据分区刷写）。

### Batch 1：排版（M1→M6，串行以控制缓存版本）
11. M1 CSS 级联/选择器/white-space（CSS 14、Section 89/88）。
12. M2 块级字号（Section 90/89）。
13. M3 分页/孤行寡行/keep-with-next。
14. M4 边框/底纹/宽表预量。
15. M5 逐词字体 + small caps（单一 arena，不引第二分配）。
16. M6 drop caps（16 级 4-bit 覆盖度；竖排禁用）。
17. L4 按 run 的 source ranges（**可选**，M5 稳定后再决定）。

每个 M 落地都会 bump 一次 Section；开发期接受多次重建，发版只算一次。

### Batch 2：插件兼容性栈（可与 Batch 1 并行、独立工作流）
18. 兼容性词汇表：`FirmwareIdentity`、`PLUGIN_API_VERSION`、`PLUGIN_CAPABILITIES`、`filtersCompatibility`。
19. 动态列表索引：`lists_url`/`lists_headers`/`lists_items`/`{list_id}`，`MAX_BROWSE_LISTS` 边界。
20. 合并行为：固定列表在索引失败时仍可用、Back 不重取。
21. `requires` 过滤（malformed → hide）。

### Batch 3：字体 P1/P2 与其余计划项
22. 行距百分比覆盖；Libron 目录数据；TTF 预热（仅在性能日志证明需要时）。
23. 设置分组、CPY 版状态栏 overlay、前端灯图标（先量 35 语言是否溢出）、统一 clang-format 21 pass、
   `wasTapInRect` 迁移、封面失败 RAM 备忘、`a722472c`（出现详情页时）、`414b3797`（统一测试桩）。

验收门禁（每批）：三构建 + 宿主测试 + `refresh_soak` 36/36；排版改动额外过
“16 级 AA / 竖排 / 底部对齐 / JJWXC 气泡”四项；圆角改动过截图 A/B。

### 估时（单线 / 并行）

| 批次 | 内容 | 开发日 |
|---|---|---:|
| 0A | 字体 ID/容量/UI/网络/陀螺仪/BLE 机制/测试 | 3–4 |
| 0B | 内圆角与安全区（含 1:1 测量） | 1.5 |
| 0C | 短语查询 Phase A | 1.5–2 |
| 0D | `Ə/ə` + `az`（含完整刷数据分区） | 0.5 |
| 1 | 排版 M1–M6 | 29–30 |
| 2 | 插件兼容性栈 | 5–8 |
| 3 | 字体 P1/P2 与其余计划项 | 5–10 |
| 可选 | L4 按 run 的 source ranges | 5–7 |

Batch 0 约 1.5–2 周；Batch 1 与 Batch 2 可由两条工作流并行，墙钟约 7–9 周（含每批构建/真机门禁）；
全部串行则约 10–12 周。Phase 0 的时钟从你发令当天开始。

## 7. 已解决的决策点

1. ~~Phase 0 是否开始~~ → **暂缓，等你发令**。
2. ~~是否新增阿塞拜疆语~~ → **新增**。
3. ~~圆角是外角还是内角~~ → **内圆角**（内容区开孔）；数值用 1:1 标尺照片定，测出前为 0。
4. ~~排版哪些做~~ → **M1–M6 全做**；L4 source ranges 作为可选尾项。
5. ~~插件兼容性是否排期~~ → **排入 Batch 2**，可与排版并行。

## 8. 与旧文档的关系

- 本文取代 `2026-11-08-upstream-kept-rejected-review.md` 的“维持拒绝”结论；
  旧文档保留为逐项原始记录，但**不再是决策依据**。
- 七份详细设计（排版/字体/批注/圆角/UI 重分类/网络构建重分类/剩余 4 项）见附录
  [`2026-11-08-upstream-absorption-designs.md`](2026-11-08-upstream-absorption-designs.md)。
- 清零与 SDK 的完成状态仍以 `2026-11-08-upstream-zeroing-plan.md` 为准。

# 维持拒绝项审阅（按改动点重组）

范围：桶 3 复审后仍判 `KEEP_REJECTED` 的 **70 项**。本文不做逐 commit 摘录，而是先把它们归并成
八个“改动点/功能面”，说明上游想改什么、CPYOS 为什么不需要、以及什么条件下值得重开；
commit 号只作为索引放在每节末尾与附录里。

复审前提：CPYOS 已在阶段 1–3 合入 111 + 19 项，SDK 已到 `d9a1304c`；下列结论都是对照**当前分支**重读
diff 与代码后得出的，不沿用旧判定。

## 0. 结论摘要

| 主题 | 条数 | 性质 | 一句话结论 |
|---|---:|---|---|
| 1. 排版引擎重写（上游 2.0.0） | 6 | 结构性冲突 | 不是补丁，是另一套分页/渲染模型；只能作为独立项目重做 |
| 2. 批注/剪贴（clippings、source ranges） | 3 | 架构冲突 | CPYOS 用 AnnotationStore；上游方案按词存范围，CJK 代价翻倍 |
| 3. 封面缓存中间态 | 2 | 已被更好实现取代 | 终态 `3db7a96b` 已采用；中间态会倒退 |
| 4. 阅读器交互与设置 | 23 | 产品取舍 + 维护动作 | 多数是 CPYOS 已有等价实现，或只对未发布板卡有意义 |
| 5. 板卡与外设 | 5 | 未发货硬件 | 除陀螺仪/BLE 两条“会删掉现有能力”的陷阱外，均为板卡专属 |
| 6. 字体与 i18n | 11 | 资产/管线不同 | 系统字体包 + FEXT1 渠道已覆盖；上游脚本产出的资产到不了用户 |
| 7. 网络/插件/Web UI | 7 | 能力模型缺失或纯搬迁 | 插件 capability/list-index 词表不存在；Web 改版与 CPYOS 打包方式冲突 |
| 8. 构建/发布/CI/文档 | 13 | 品牌与流程差异 | CPYOS 有自己的一套；少数可移植规则已被采纳为约束 |

三类处理建议：

- **永久不做（架构/产品决定）**：主题 1、2、5 中的板卡项、6 的大部分、8 的发布/品牌项。
- **条件性重开（有明确触发条件）**：主题 4 的板卡相关、主题 5 的 az/字体管线、主题 7 的插件能力、
  主题 8 的多渠道发布。
- **无需合入的维护动作**：主题 4/7/8 里的格式化、死代码、缺失测试；这些应当作为 CPYOS 自己的
  清理任务做，而不是从上游搬。

---

## 1. 排版引擎重写（上游 2.0.0）

**上游改动点**：`0305aaa6` 把 EPUB 排版换成一套新模型——CSS cascade（复合类、后代/子选择器、
`::first-letter`）、source ranges（每个词记录源文件偏移）、drop caps、表格列宽预量、逐词字体、
small-caps 等。配套提交还包括：为探测图像尺寸借出帧缓冲、去掉无用的图像存在性辅助、
以及 2.0.0 的注释/文档整理。

**CPYOS 现状与拒绝理由**：CPYOS 的分页层是围绕三平面渲染、列带/局部刷新、16 级 AA 和
`SECTION_FILE_VERSION` 86/87 建起来的；上游模型会重写 `Page/TextBlock/ParsedText/CssParser`，
并要求重新定义分页缓存格式。source ranges 按词存储，英文约 8 B/词，CJK 单字成词后接近翻倍，
与 C3 的 RAM 预算和 CPYOS 的缓存体积直接冲突。这不是“挑几个 hunk”能解决的问题。

**何时重开**：只有把“换排版引擎”当成独立项目、带真机分页/刷新回归和缓存迁移方案时才做；
届时按特性逐项决策（CSS 选择器、drop caps、表格），而不是整包合并。

**本组还包含**：`fdf5f76c`（非 CJK 回退探测）——探测逻辑在 CPYOS 的系统字体包/C3 早退路径下不可达，
且现有解析已按字形覆盖选择字体；`2c09b8ce`（删除无人调用的 `ImageBlock::imageExists`）、
`b0aadf2a`（JPEG/封面注释整理）属于顺手清理，自己删即可，没有合并价值。
`072b6fc7`（行距按字号百分比）与 `88ea9bcf`（把 1.3/1.6 改回 1.1/1.2）是 CPYOS 刻意的排版选择：
四档、按字族分别设定，CJK 正文留白更大；只有用户明确反馈“SD 字体行距过松”时才重新评估。

索引：`0305aaa6`、`fdf5f76c`、`2c09b8ce`、`b0aadf2a`、`072b6fc7`、`88ea9bcf`。

## 2. 批注/剪贴体系（clippings、source ranges）

**上游改动点**：`d7fcc7ce` 引入基于 source ranges 的划线与批注；`c201af43` 在其上做“多词短语查询”，
把选区内容送进词典；`ad24aea8` 是剪贴弹窗的闪烁修复。

**CPYOS 现状与拒绝理由**：CPYOS 已有 `lib/Epub/Epub/AnnotationStore`（高亮/批注/导出，EPUB+PDF），
上游的 `ClipSelectionActivity`、`EpubReaderClipping*` 在公共树里已经不存在。source ranges 的存储
代价同上（按词偏移、CJK 翻倍）。**如果要做短语查询**，正确路线是 CPYOS 原生实现：用长按/拖动
在 `AnnotationStore` 里选出一个文本范围，再从当前段落文本做一次词典查询，而不是恢复上游的
clip 子系统。

**何时重开**：产品决定要做“选中短语查词”时，按上面 CPYOS 原生路线做；裁剪弹窗的闪烁修复只在
引入对应弹窗后才相关。

索引：`d7fcc7ce`、`c201af43`、`ad24aea8`。

## 3. 封面缓存中间态

**上游改动点**：`186d9407` 用 0 字节文件当“转换失败”的终止标记；`12e441f7` 又把这个哨兵删掉。

**CPYOS 现状与拒绝理由**：CPYOS 已采用这条链的**终态** `3db7a96b`（校验 `bfOffBits` 与像素数据
是否越界、拒绝 header-only 存根），并在 `7352b33f` 落地。中间态单独合入会倒退到“0 字节也算有效”
或重复一次已经修过的逻辑。

**何时重开**：不需要。只需在改动封面缓存时对照终态实现。

索引：`186d9407`、`12e441f7`。

## 4. 阅读器交互与设置

这一组最杂，按“上游想改的点”再分六类：

### 4.1 长按菜单与触控板过滤

上游把“长按菜单”在带 Home 键/无触控的板卡上隐藏，并把过滤条件从 `hasHomeKey` 改成 `hasTouch`。
CPYOS 的 `hasTouch()` 在 readpico 上为真，长按 Confirm 驱动书签/词典/菜单；照搬会**删掉现有功能**。
CPYOS 当前的过滤条件（`SettingsActivity` 里的 `hasHomeKey`）才符合自己的板卡集合。
重开条件：CPYOS 真的发布一台带 Home 键的板卡时，按那台板卡重写过滤，而不是全局改 `hasTouch`。

索引：`471243b2`、`5407ffcd`。

### 4.2 手势迁移与枚举存取

上游修改了触摸/手势迁移版本号与混合手势对的保留逻辑。CPYOS 的迁移只在**两个手势键都不存在**时
展开旧值，且从不改写另一侧；`INVERTED_TAP` 在读取时处理。上游要修的“迁移后配对丢失”在 CPYOS
的迁移方案里不存在，照搬反而会引入上游的 `touchControlsVersion` 约定。

索引：`b823d54b`、`e41982a4`。

### 4.3 设置表容量与分组

上游连续几笔在给设置表加条目时调整 `entryCount`/批次容量；`87fc4ab7` 则是设置分组（hub + 子页）。
CPYOS 使用自己的设置表与计数，且分组早已按“分类头 + 行内边距”实现并在真机验证过。
**值得注意的旁证**：CPYOS 自己的 `test_settings_api_keys.py` 目前因“一次插入 5 项”失败，
属于同一类“增行时忘同步计数”的缺陷——应当在 CPYOS 的表里修，而不是搬上游的 bookkeeping。
分组设计是产品取舍（hub 还是分组头），不合并。

索引：`c7996b9f`、`35dccb87`、`e9635421`、`87fc4ab7`。

### 4.4 INX 主题与工具栏样式

上游的“统一 INX 头部状态栏”“共享 INX 页面选择动作”“圆角工具栏内边距”都建立在 INX 主题上。
CPYOS 已删除 `inx` 主题（`UI_THEME` 只有 `CPY`），这些代码路径不可达；`sidePadding = 0` 是 CPY
布局下光标画在行外的刻意结果。`b2b1e15a` 把前端灯面板从文字标签改成图标，CPYOS 的**标签瓦片
网格**是刻意设计，不是缺陷。`8f4d035c` 的 Picco 开关只对未发布板卡有意义。

索引：`ec32b959`、`8b4c0d6e`、`3f8345c5`、`b2b1e15a`、`8f4d035c`。

### 4.5 弹窗、圆角与安全区

`01f09e0e` 要内容区适配圆角和 inset：CPYOS 发货的 Read Pico 是直角屏，现有页面都带安全区，
没有可复用的圆角容器。`43694c90` 的“KOSync 服务器类型选择弹窗”在 CPYOS 里对应的是
`SettingInfo::Enum` + `OptionPopup` 的既有模式；等真正加入服务器类型设置时按 CPYOS 模式做。

索引：`01f09e0e`、`43694c90`。

### 4.6 生命周期、格式化、死代码与缺失测试

`cac12269` 把 `ActivityManager` 的构造/析构移出头文件：CPYOS 头文件内联版本在所有环境都能编译，
只有“销毁 manager 但没包含 `Activity.h` 的 TU”才会不合法，而这样的 TU 不存在。
`944b741e`/`31480212` 是 clang-format 变更，CPYOS 本机没装 clang-format 21，合并只制造噪音。
`d52fc820` 删除的矩形命中辅助在 CPYOS 仍有约 20 处调用（游戏/查看器）。
`4ef42ee1` 跳过的调试代码块里引用的 `HalMemory::getInternalHeap()` 在 CPYOS 的两个文件里根本不存在。
`052d481a`/`5eb498b0`/`f3ace194` 是测试：前两者对应的 `test/touch_page_turn` 目录不存在，
且混合手势/禁用档位已由现有几何测试覆盖；`f3ace194` 的封面缓存测试针对上游 loader，CPYOS 已换成
Gray8 `_g8` 缩略图、10 槽 PSRAM 缓存与路径记忆，裁剪会丢掉本地覆盖。

索引：`cac12269`、`944b741e`、`31480212`、`d52fc820`、`4ef42ee1`、`052d481a`、`5eb498b0`、`f3ace194`。

## 5. 板卡与外设

**上游改动点**：新增 LilyGO T5S3、BOOX Picco、Picco/Sticky 显示、Picco 硬件开关，以及陀螺仪检测、
上游 BLE 翻页模块。

**CPYOS 现状与拒绝理由**：

- 未发货板卡（LilyGO、Picco、Sticky）没有硬件可验证，驱动字段也依赖新 SDK 的板级描述；
  合入只增加死代码。`fb2a8a8f` 里还有一个**陷阱**：它的 `Frontlight.present()` 门控会关掉 CPYOS
  在**无前光触控板**上用 `FrontlightPanelActivity` 当控制中心的路径。
- `8fc2b661` 会把倾斜翻页改成“只认陀螺仪”，直接删掉 Read Pico 的加速度计倾斜/摇晃；
  真正可取的只是未来 SDK 探针换成 `!hasGyroscope()`，而不是删功能。
- `fecee610` 是另一套 BLE 翻页模块，与 CPYOS 现有 BLE 输入抢同一个射频；合并意味着要么删掉现有
  实现，要么同时跑两个 owner。可借鉴的只有空闲 30 s 自动关射频的参数。

**何时重开**：发布对应板卡时按板卡移植；BLE 只有在 CPYOS 决定统一到上游模块时；陀螺仪检测只在
未来 SDK 探针重构时作为一个小技巧吸收。

索引：`809c1dab`、`261726f6`、`fb2a8a8f`、`8f4d035c`、`8fc2b661`、`fecee610`。

## 6. 字体与 i18n

**上游改动点**：给内置位图子集补 Cyrillic、补 Azerbaijani 的 Ə/ə、把 NotoSerif 移出内置、
用 Python 生成字体 ID、复用 UI 字体做 CSS 变体、修 advance table 的样式索引、在列表绘制前预热
CJK 字形、下载目录里加 Libron 字体。

**CPYOS 现状与拒绝理由**：CPYOS 的阅读/界面字体来自**系统可缩放字体包**（`CPYOSHanSansSC`，
`main.cpp` 在 `CROSSPOINT_SYSTEM_FONT_PACK` 下直接绑定），内置 12/14/16pt 位图面在 readpico 上
根本不编译，所以“Cyrillic 缺失”“预热解压卡顿”“CSS 变体重复加载”这些上游问题在 CPYOS 的路径上
不可达。字体 ID 由 CPYOS 自己的 `fontIds.h`/生成器产出，带上游脚本会静默丢掉 `CJK_UI_18_FONT_ID`、
`CJK_READER_SIZE_FONT_ID(pt)` 等宏。NotoSerif 头文件仍是 ID 生成器的输入，单独删除会破坏生成。
字体扩展走 FEXT1（Pages 清单 + SD 安装），上游的 `sd-fonts.yaml` 目录到不了用户。

**可吸收的两个小点**（都不必合 commit）：
- 若将来加 Azerbaijani 等 Latin-Ext-B 语言，按 §55.3 的方式给系统字体 cmap 补 U+018F/U+0259，
  重建 `.def`，而不是合并 38 个生成的位图头文件。
- 若将来重新启用内置位图面，再按上游方式补 Cyrillic 子集并做分页失效。

索引：`40b313a3`、`1899de8f`、`3f0f4fad`、`ce781eb8`、`fc2de152`、`7242d9ae`、`cf50ce73`、
`2648933a`、`98da1782`、`c6a2e311`、`965722cf`。

## 7. 网络/插件/Web UI

**上游改动点**：OPDS 发布对象的初始化修复；Web UI 的 chevron/CSS 修复、把插件设置搬进独立页面；
插件目录的“固定+动态列表合并”“列表索引与 header 映射”“按 capability 过滤”；以及两组测试修复。

**CPYOS 现状与拒绝理由**：

- OPDS 的 shadowing 修复位于一个 CPYOS 不存在的“出版详情”路径；如果将来加详情页，照着修即可。
- Web UI 的搬迁/CSS 合并基于上游的文件布局（`web.css`、`Header.html.inc`），CPYOS 的页面是
  打包进 `CWP1` 的独立源文件；chevron 问题在 CPYOS 保留原生 `select` 外观后不会出现。
- 插件 capability 过滤/动态列表索引依赖一套 CPYOS 没有的词汇表（`FirmwareIdentity.h`、
  `PLUGIN_API_VERSION`、`PLUGIN_CAPABILITIES`、`lists_url`）；先有这套模型才谈过滤。
  **值得单独修的自有隐患**：`PluginCatalogActivity` 对 `browseLists` 的 `reserve()` 没有上限，
  在 `-fno-exceptions` 下会 abort；应加一个和上游 `MAX_BROWSE_LISTS` 等价的边界。
- 两组测试对应的文件/目录在 CPYOS 不存在（递归词典测试未随功能一起移植）。

索引：`a722472c`、`55a77c2e`、`e93f8d1f`、`d5d0bea1`、`861683cc`、`c9f64d0d`、`18d611f2`。

## 8. 构建/发布/CI/文档

**上游改动点**：PR 预览固件链接、README 升到 2.0.0、develop 分支 CI、1.6.5 发布说明、Stable 只保留
四个目标、OPDS 测试的渲染锁生命周期、前端灯文档、README 链接/加粗、AGENTS 指南重构、SCOPE 文档。

**CPYOS 现状与拒绝理由**：CPYOS 有独立的 `cpyos-release.yml`/`pages.yml`、`cpyos-X.Y.Z` tag 方案、
`docs/releases/<v>.md` + `.ota.json`、自有 AGENTS/用户指南与站点；上游的发布流程、品牌与文档
结构都不能直接套。`414b3797` 的测试桩要求 `~RenderLock()`，而 CPYOS 的真实 `RenderLock`
已有析构与同任务递归保护，比上游桩更强。`997b4331` 的“发布资格 ≠ 构建模式、不得静默回退渠道”
两条规则已经被 CPYOS 的发版校验覆盖。

**可吸收的只有规则本身**（不搬文件）：多环境/多目标发布时保持“资格与构建分离”“不回退渠道”；
PR 预览若将来启用，只链接未过期的构建产物。

索引：`59912ea9`、`35072257`、`e7b47ee5`、`6ec32892`、`997b4331`、`414b3797`、`c17b80ab`、
`a6db8f15`、`91ed3ef8`、`6d2cd3a1`、`3607ea7b`、`d1f5d1ec`、`bb855ef9`。

## 9. 这次复审顺带发现的三个“自有问题”（与合入无关，建议另开任务）

1. `test_settings_api_keys.py` 因“一次插入 5 项”失败——设置表增行时计数/批次未同步，
   与上游 `c7996b9f`/`35dccb87` 是同一类问题，但应在 CPYOS 的表里修。
2. `PluginCatalogActivity` 的 `browseLists` `reserve()` 无上限，`-fno-exceptions` 下 OOM 会 abort。
3. `UiHighDpiProfile::reader12FontId` 硬编码；字体包内容变化时缺少类似“包 CRC 参与分页失效”的保护
   （上游用头文件哈希，路线不同）。

## 附录：commit → 主题索引

- 主题 1：`0305aaa6` `fdf5f76c` `2c09b8ce` `b0aadf2a` `072b6fc7` `88ea9bcf`
- 主题 2：`d7fcc7ce` `c201af43` `ad24aea8`
- 主题 3：`186d9407` `12e441f7`
- 主题 4：`471243b2` `5407ffcd` `b823d54b` `e41982a4` `c7996b9f` `35dccb87` `e9635421` `87fc4ab7`
  `ec32b959` `8b4c0d6e` `3f8345c5` `b2b1e15a` `8f4d035c` `01f09e0e` `43694c90` `cac12269`
  `944b741e` `31480212` `d52fc820` `4ef42ee1` `052d481a` `5eb498b0` `f3ace194`
- 主题 5（板卡/外设）：`809c1dab` `261726f6` `fb2a8a8f` `8fc2b661` `fecee610`
- 主题 6（字体/i18n）：`40b313a3` `1899de8f` `3f0f4fad` `ce781eb8` `fc2de152` `7242d9ae` `cf50ce73`
  `2648933a` `98da1782` `c6a2e311` `965722cf`
- 主题 7（网络/插件/Web）：`a722472c` `55a77c2e` `e93f8d1f` `d5d0bea1` `861683cc` `c9f64d0d` `18d611f2`
- 主题 8：`59912ea9` `35072257` `e7b47ee5` `6ec32892` `997b4331` `414b3797` `c17b80ab` `a6db8f15`
  `91ed3ef8` `6d2cd3a1` `3607ea7b` `d1f5d1ec` `bb855ef9`

（合计 70；`8f4d035c` 属于板卡但归在交互组，避免重复。）

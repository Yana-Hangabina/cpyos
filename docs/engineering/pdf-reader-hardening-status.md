# PDF 阅读器加固：需求 → 实现 → 证据（2026-11-21）

一页对照表，给接手的人。**“设备”列空的条目就是还欠真机验收的**。

| # | 用户要求 | 实现 | 主机证据 | 设备证据 |
|---|---|---|---|---|
| 1 | 碎片时渲染大图页不静默变慢/失败 | 整页档按**连续块**选（`PdfRenderBudget` + `largest > pixmap+slack`）；逐段 `render step:` 日志；预取 20 s / 按需 75 s 上限 | `test/pdf_render_budget`(3 变异)、`test/pdf_job_wait`(4 变异)；镜像自证字符串 | 待：`hard_scan.pdf`+`FRAG`（失败路径） |
| 2 | 彻底失败也要好提示 + 自动退出 | 失败卡两个可点胶囊；连续 3 页失败升级为「此 PDF 无法显示」+「返回」 | `test/pdf_fail_card`(3 变异)、`test/pdf_fail_streak`(3 变异)、`test/pdf_job_slot`(4 变异) | 待：两张卡实拍 |
| 3 | 长但可读的页要有波浪进度 | `status_card::showWait`（鸭子波浪 + n/m），每 10 s 刷新已用秒数 | `test/pdf_job_wait` | 待：`slow_scan.pdf` |
| 4 | 读 PDF 时自动关 Wi-Fi（注意连带） | `onEnter` → `releaseRadio()`（**只断关联**，驱动保留）。★ 2026-11-22 真机实测：深度版（`WIFI_OFF`+`deinit`）在“射频开着→打开 PDF”时产生可复现的 `Cache error` 崩溃，且把拆除推迟到内部栈后依旧，故回退到本仓已长期验证的轻量释放；AP 不动 | 实测 free 26,843→51,907 / largest 7,156→10,228（`CMD:WIFIDEINIT` 两次，同一函数）；`AGENTS §85.13` | 机制已设备验证 ×2；待：矩阵 1e 的**阅读器进入路径**（`mode=0` + ΔRAM） |
| 5 | 首次读 PDF 提示一次 | 一次性卡（`pdfWifiHintSeen`），由 `renderBook` 在页面帧上绘制；文案是**专用两键**（「WiFi off」/「Pages need the memory; WiFi returns when you leave.」），不再复用无关字符串 | `renderBook` 顺序（提示不压失败卡） | 待：实拍 + 矩阵 1e「只出现一次」 |
| 6 | 会失败的页直接显示、可翻页跳过 | 降级阶梯的 text-only 级做**墨量检查**（空白即判失败，不放白纸）；超时**返回失败**（不重启）→ 卡；失败态只在**页面真的改变**时清除（`pageTurn` 的成功分支 + `pdf_reader_ui::afterSkip`）；末页/单页上跳过＝留在卡上并记日志，误触空白＝不动作 | `test/pdf_ink`（3 变异）、`test/pdf_reader_ui`（5 变异，含「跳过忽略是否移动」「误触即重试」）、`test/pdf_job_slot`、矩阵断言「跳过不复活」 | 待：矩阵 1e |
| 7 | 阅读器只在阅读时工作（引擎映射保留，见备注） | `onExit`：等探针 → 停 worker（≤5 s，超时泄漏而非中途删任务）→ 释放文档 → **卸载引擎模块**（1.8 MB 映射 + ~54 KB PSRAM；worker 被泄漏时跳过）→ 恢复字体/断词 | `test/pdf_job_slot` 的 `mayStopWorker` | 待：矩阵「退出后 free ≥ 20 KB」 |
| 8 | 串口命令不被键盘/弹窗吞掉 | `pdfForeground` 走被测路由：Forward / **DismissAndResend** / Ignore；脚本自动重发一次 | `test/pdf_bench_route`(3 变异) | 待：矩阵（键盘态下翻页） |
| 9 | 下载页别被 80 MHz 拖慢 | 15 处 UI 任务传输 + **KOReader 同步/认证 4 个入口**补 `HalPowerManager::Lock`（含 pluginhttp 的订阅/列表索引/API 跳转/插件事件与网页中转） | `scripts/tests/test_download_power_locks.py`（9→15 处 + KOReader 显式清单；删任一锁即红，已变异验证）；`Lock` 构造即 `setPowerSaving(false)`（HalPowerManager.cpp）⇒ 传输全程 240 MHz | 待：一次长下载的秒数对比 |
| 10 | 换 PSRAM 大小也要鲁棒 | `PdfRenderBudget::forPsram(total)`：≥6 MB 用实测常数，更小按比例 | `test/pdf_render_budget`（8 MB 数值不许漂 + 随堆单调） | 本机行为零变化（测试钉住） |
| 11 | 解决不了就合理兜底 | 超时返回/泄漏任务/跳过/升级返回，均有日志与测试 | 上述各测试 | — |
| 12 | 清理缓存 | 本会话回收 ~4.9 GB；`exp/pdf_chain.sh` 低于 4 GB 自清缓存 | — | — |
| 13 | 打不开的文件别静默回首页 | `handleLoadFailure` → `loadFailed_` 卡（「PDF cannot be shown.」+ 任意输入退出） | 构建通过；复用现有文案 | 待：损坏 PDF |
| 14 | 一次性提示是模态 | 提示挂起时 `loop()` 吞掉本帧输入（点一下只关提示，不顺手翻页） | 代码路径 | 待：实拍 |
| 15 | 超时泄漏的 worker 不得复活 | worker 从任务参数取队列（不读全局）；泄漏任务此后永远阻塞在无人发送的队列上 | 代码审查 + `docs/engineering/pdf-xip.md`；构建自证 | — |
| 16 | 超时后槽位仍归 worker | 四处等待统一 `releaseSlotAfterWait()`（渲路径曾无条件清除，会让下一次提交覆写 worker 正在用的作业） | `test/pdf_job_slot` + `scripts/tests/test_pdf_slot_release.py`（在旧代码上必红） | — |

## 主机回归（2026-11-21 最后一次全量）

`test/*/run_*.sh`（**44 套**，含本轮新增 8 套：job_wait / job_slot / fail_card / fail_streak / reader_ui / render_budget / pdf_ink / pdf_deadline）**44/44 通过**；
`scripts/tests/test_matrix_parsers.py`（真机日志夹具）、`test_download_power_locks.py`、
`test_render_path_no_flash.py` 全绿。Python `unittest discover` 的 73 项失败均为本树既有问题（见上）。

## 失败会在什么时候发生（2026-11-21 修正一条早期假设）

降级阶梯后几级会调 `fz_set_image_budget()` 让 MuPDF **把源图降采样**，因此"PSRAM 很碎 + 巨图"通常
得到的是**粗页面**而不是失败卡——这是阶梯在按设计工作，不是漏报。真正会落到失败卡的是：
① 单次渲染超过 **75 s** 应用侧上限（慢且仍在走阶梯）；② 连 text-only 都画不出东西（本轮的墨量闸门会把
"跳掉图片后一片空白"从"成功"改成"失败"，继续阶梯或最终出卡）；③ 结构性错误（损坏文件/加密/非法页）。
"打不开的文件"走另一张卡（`loadFailed_`，任意输入退出）。

## 仍然只知道"大致位置"的地方

原始 180 s 事件当时的现象是「Render 作业不返回、日志零行」，本轮把它定位到三件事并分别处理：
等待无截止（现 75 s + 每 10 s 报状态）、作业对象在栈上（现静态槽 + 所有权规则）、
整页档按总空闲而不是连续块选（现按 `largest`）。**但 MuPDF 内部究竟停在哪一行仍未取到证据**——
`render step:` 分段日志就是为它准备的：下次再发生，日志会点名到
`header / pixmap alloc / draw device / run_page start / run_page done` 中的某一段，而不是只有沉默。
★ 仍属**有界而非可中断**：`awaitSyncSlot()` 会在超时后等旧作业归还槽位（最多再等一个预算），
阶梯在 worker 里不可被外部打断（MuPDF 调用无法取消，要真中断得改引擎 ABI，暂不做）——
但阶梯现在有 **90 s 墙钟预算**，所以 75 s 出卡之后 worker 最多再跑十几秒就释放槽位。
对用户的可观察后果只有「等待卡多显示一段时间」，不会死机、不会静默。

## 本轮提交（按时间，便于回溯）

`09ed8c48` 失败卡可点/文案 · `975ed933` 作业槽规则+测试 · `eea44acd` 卡片几何+测试 ·
`ecb0e887` 不中途删任务 · `5e2e8777` flash 安全拆除 · `4babfb31` 三连失败升级 ·
`c7aecc93` 跳页清失败态 · `a73cbcbf` 提示不压失败卡/退出 5 s · `ee981dac` 预取移到渲染任务 ·
`1eac4b16` 卡片推屏+工具栏守卫 · `a6fe395c` PSRAM 自适应余量 · `9ce3c3d2` 下载锁+审计 ·
`72ad06e3` 注释纠正 · `4209f6ef` core 判据补充 · `19539fe5` 设备恢复手册 ·
`2502a9e6` 镜像自证关卡 · `f5280ab4` 本表 · `06b5a165` 矩阵 1e 先连网 ·
`5b15fa01` 打不开的卡片 · `5872b061` 渲染路径禁写 flash 测试 · `663f2fe5` 数据包同源 ·
`226f896f` 提示模态 · `3492e51b` 输入判定表 · `788d296a` 语言包匹配断言 ·
`8c132df0` 正常页不读输入（修吞点击） · `7457ddb9` 卡片只在真翻页时清除 · `de0d7fa9` plugin-http 传输锁 · `9720fe17` 空白 text-only 判失败 · `0688f147` 退出即卸载引擎 · `ee22fce6` 超时后等槽位 · `1bfcbc10` unload 进 XIP 守卫 · `711a2968` worker 绑定自己的队列 · `b77d1982` 场景断言改为“有界结论”（MuPDF 会降采样） · `2beae077` 一条命令验收 + 91 KB 夹具 · `4d3cdbd1` 截断页不算成功 + PdfDeadline · `879adf0d` 等待文档纠错 · `c4959b01…` 慢重排出等待卡 · `352db5b0` 槽位所有权（render 路径）

★ i18n 现有 **1868 键**（新增 2 个提示键；为腾出 EN blob 的 15-bit 空间，缩短了三条冗长英文）⇒ 设备必须重刷数据分区，验收脚本的完整刷写会做。

**整轮验收一条命令**：`bash exp/pdf_accept_all.sh --wait=3300`（等端口 → 刷写 → PUTFILE 三个夹具（重扫/慢扫/非 PDF）→ UX 三场景 → 矩阵；夹具走串口是因为无人值守时没人开文件传输，`hard_scan_tiny.pdf` 只有 91 KB 但解码仍是 7.4 MB）。单独跑 UX 场景时：夹具先经设备网页上传（`exp/put_on_card.sh <IP> exp/fixtures/hard_scan_tiny.pdf`），再 `~/.platformio/penv/bin/python exp/pdf_ux_scenarios.py --coarse /hard_scan.pdf --slow /slow_scan.pdf`。它自己施加/解除 PSRAM 碎片（`CMD:FRAG`/`UNFRAG`）、判失败卡、按跳过、判等待卡，并截 `artifacts/pdfux_*.png`。

## 完成审计（2026-11-21 收尾时的状态）

**已经用可复跑的证据证明的**（主机/静态/构建自证）：
- 渲染档位按**连续块**选择、PSRAM 余量按总量派生（`test/pdf_render_budget`）；
- 等待规则（10 s 起报、按调用方预算超时）与槽位所有权（`test/pdf_job_wait`、`test/pdf_job_slot`、
  `scripts/tests/test_pdf_slot_release.py`，后者在旧代码上验证必红）；
- 失败卡几何/连败升级/输入判定表/跳过语义（`test/pdf_fail_card`、`test/pdf_fail_streak`、`test/pdf_reader_ui`）；
- 空白 text-only 判失败与时间比较（`test/pdf_ink`、`test/pdf_deadline`，均带变异）；
- 15 处传输持 CPU 锁、9 个渲染函数不写内部 flash（两个静态扫描，删改即红）；
- 引擎镜像与 ELF 的 appTag 配对、镜像内自证字符串、数据包与 manifest 同源（`pdf_chain.sh` 每次构建都查）；
- 44 套主机回归在**本轮最后一次改动之后**全跑过（44/44，含 `WIFI_STORAGE_FLASH` 恢复、8 s 预取窗口、串口助手的 `write_timeout`/去 `tcdrain`）。

**只有设备能证明的**（矩阵 1e 已写成断言，等一次上机）：
`deep radio release`（mode=0 + ΔRAM）、一次性提示只出现一次、失败卡两胶囊与跳过、
`engine module unmapped`、键盘遮挡后重发、`CMD:LOCK` 下 `cpu=240`、UX 场景两张截图。
**阻塞条件**：设备自上次中断的刷写后串口无响应（USB 枚举在），本机无复位键，只能物理断电重启。

**镜像自证（本轮已核）**：验收用到的每条台架命令都在刷写的镜像里——`PUTFILE_READY`/`LS_START`/`NOSLEEP`/`FRAG kb=`/`FRAG released`/`LOCK held for`/`engine unmap deferred` 在 app ELF，`ladder budget` 在 pdfmod；**命令不在镜像里的话，验收会在第一步就静默失败**，所以这条和 appTag 配对一样每次都查。

**收口命令**：`bash exp/pdf_accept_all.sh --wait=3300`（它自己刷写、上传夹具、跑矩阵与场景）。
跑完后用 `~/.platformio/penv/bin/python exp/pdf_fill_evidence.py /tmp/matrix_full.log /tmp/accept_ux.log` 生成设备列——未匹配到检查的行会写 **missing**，不会被静默跳过。
刷写前的配对检查失败会**在任何写入之前**中止（`set -euo pipefail`），成功后按 **3 个 `Hash of data verified.`** 计数判真——这两条都是上机事故换来的，不是形式。

## 设备实测结果（2026-11-23，最终构建 `fc320116`）

矩阵：**28 通过 / 0 失败 / 4 跳过**；UX 场景 **8/8**（粗页、慢页、非 PDF）；截图
`artifacts/fail_card.png`、`artifacts/wifi_hint.png`、`artifacts/pdfux_{coarse_page,slow_page,load_card}.png`。

| 行 | 设备证据 | 结论 |
|---|---|---|
| 1 碎内存渲染 | 粗页在时限内给出结论（页面） | PASS |
| 2 失败卡 | 两胶囊绘制；Confirm 离开 | PASS |
| 3 慢页提示 | 慢页渲染或报等待 | PASS |
| 4 射频 | 深度释放：free 25,983→51,043，`mode=0 status=254` | PASS |
| 5 一次性提示 | 首次 armed；下次不重复 | PASS |
| 6 跳过 | 卡片不复活；不重启 | PASS |
| 7 退出内存 | free≈35 KB；引擎按设计保持映射 | PASS |
| 8 键盘路由 | `dismissed KeyboardEntry … resend` 且重发送达 | PASS |
| 9 240 MHz | 空闲对照 80 → 持锁 240 → 自行释放 | PASS |
| 13 非 PDF | 停在不可读卡；一次输入离开 | PASS |

4 项跳过均有明确理由（会话中途接入的启动检查、需要文字页夹具的选择/批注两段、需要真引擎镜像的 Busy 分支）。

## 本轮设备验证揪出的三个真缺陷（已修）

1. **`hasStackedActivity` 用 `==` 比较 `const char*`** → 恒为 false；键盘盖住阅读器时串口命令永远被忽略。改为 `strcmp`。
2. **`CMD:LOCK` 台架栈 4 KiB** → `Stack canary (PwrLockBench)` 崩溃重启，污染其后的所有段落。改为 16 KiB。
3. **退出卸载引擎 → 下次打开 `Cache error`**（mmu 卸载/重映射，2/2 vs 0/2 A/B）。卸载关闭；
   深度射频释放改为“阅读器调度、主循环在内部栈执行”，实测 `mode=0`、+25 KB、连续两次打开不崩。

## 验收时每行要看什么（设备列怎么补）

| 行 | 证据（日志 grep / 产物） |
|---|---|
| 1 | `render step: bmp header written` → `pixmap alloc` → `draw device created, run_page start` → `run_page done`；或 `pdf worker slow: kind=1 ... exceeded`（必然失败场景） |
| 2 | `render-failure card: retry at`（卡片画出）、`page N failed (3 in a row, this file cannot be shown here)`（升级）；截图 `artifacts/fail_card.png` |
| 3 | `pdf worker waiting: kind=1 page=N at 10000ms ...`（10 秒后仍慢）；`slow_scan.pdf` 的等待卡截图 |
| 4 | `deep radio release: mode=0 free X->Y`（Y−X ≥ 8 KB）；`CMD:NET` → `mode=0 status=254` |
| 5 | `wifi-off hint armed`（一次）；第二次开书**不得**再出现；截图 `artifacts/wifi_hint.png` |
| 6 | 跳过之后 `leaving the failure card for page change`，且不再出现第二条 `render-failure card: retry at`；在**最后一页**按跳过应出现 `failure card: no page after to skip to` 且卡片仍在（不得重渲染该页；超时后翻页不应立刻出现 `render refused`，应是 `waiting … to release the sync slot` 之后正常渲染 |
| 7 | 矩阵 `free internal RAM after exit is a healthy number`（≥20 KB）；无 `worker not stopped`；干净退出应有 `engine module unmapped …`（onExit 在内部栈上直接卸，或在渲染任务上先 `engine unmap deferred` 再由主循环 `engine module unmapped by the main loop` —— **绝不在 PSRAM 栈上卸**，那会刷缓存并毁掉该栈），泄漏路径则是 `engine module kept mapped: …` |
| 8 | 矩阵自动执行：`CMD:KEYBOARD` 盖住阅读器 → `CMD:PDFNEXT` 应打 `dismissed KeyboardEntry to reach the reader; resend the command`，脚本重发后出现 `serial CMD:PDFNEXT` |
| 9 | 矩阵自动执行：先量**空闲对照**（静置 6 s → `cpu=80`，证明探针有区分力），再 `CMD:LOCK 15000` 持锁并静置 6 s → `cpu=240`；随后出现 `LOCK released (bench)`。对照若不是 80，本项直接判**探针无效**而不是假通过；真实下载的秒数对比仍可另测（本项证明锁的机制，静态测试证明 15 处传输都持锁） |
| 13 | 场景自动执行：PUTFILE 一个非 PDF（约 60 B）→ `CMD:PDF /bad.pdf` 应打 `document did not load; showing the unreadable card`，随后一次 `CMD:CONFIRM` 必须离开阅读器（`Popped from activity stack`）；截图 `artifacts/pdfux_load_card.png` |

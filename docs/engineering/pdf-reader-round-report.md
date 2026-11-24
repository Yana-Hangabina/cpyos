# PDF 阅读器加固：本轮结论（2026-11-21）

一句话：三处根因已定位并各有工程化对策；本轮另外挖出并修掉 **7 个真缺陷**（其中 3 个会直接毁掉堆或栈：槽位提前交还、泄漏 worker 复活、PSRAM 栈上解映射）；
所有能脱离设备验证的部分都有可复跑测试；**设备验收唯一未完成，阻塞在设备需要一次物理断电重启**。

## 根因（原始症状：碎 PSRAM 下大图页"翻页没反应"、最终 180 s 不返回）

1. **等待侧没有截止时间**：应用无界等待同步作业 → 界面永久冻结、日志零行。
2. **作业对象在栈上**：等待方放弃后作业仍写调用者栈 → 只能"重启"式兜底。
3. **整页档按总空闲而不是连续块选择**：`free 1.42 MB / largest 573 KB` 却选了 1.09 MB 的整页 pixmap。

对策：`PdfJobWait`（渲染 75 s / 打开与抽取 180 s，10 s 起报）、静态作业槽 + 所有权规则、
按 `largest` 选档 + 按 PSRAM 总量派生的余量（`PdfRenderBudget`）。

## 本轮新修（都不是"整理"，是缺陷）

| 缺陷 | 后果 | 对策 |
|---|---|---|
| `renderPdfPage` 超时后**无条件**交还槽位 | 下次提交覆写 worker 正在用的作业（含 `std::string`）→ 堆损坏 | 四处等待统一 `releaseSlotAfterWait()`；`test_pdf_slot_release` 钉住（旧代码必红） |
| 泄漏的 worker 读**全局**队列 | 复活后接走新会话的作业 → 两个 worker 跑同一文档 | 队列句柄来自任务参数 |
| text-only 级"成功"画出白纸 | 扫描件显示空白且无提示 | 墨量闸门（`PdfInk`，5 变异） |
| 阶梯预算 `break` 后 `ok=true` | 半张 BMP 当成功 | `allBands` 全写完才算 |
| `onExit` 在 **PSRAM 栈**上解映射引擎 | cache flush 毁掉该栈（本分支吃过 `Double exception`） | 延迟到主循环（内部栈）执行；`test_pdf_unmap_stack` 钉住 |
| 慢重排无任何提示 | "翻了没反应" | 上次构建 >800 ms 时下次先出波浪卡 |
| 首次提示复用无关文案 | "WiFi 空闲关闭 + 内存不足"，等于没解释 | 专用两键（中英），为腾 EN blob 缩短三条冗长英文 |

另：KOReader 同步/认证与 plugin-http 传输补 CPU 锁（静态扫描 15+4 处，删任一即红）；
下载页的 240 MHz 用 `CMD:LOCK` 台架做**带空闲对照**的设备判据（旧检查会假通过）。

## 现在用户会看到什么

- 慢页：鸭子波浪卡 + 已用秒数；能出图就出（阶梯可降到 60% 或 text-only）。
- 真出不来：失败卡「重试 / 下一页」，连败 3 页升级「此 PDF 无法显示」+「返回」；**不重启、不乱跳**。
- 打不开的文件：停在「无法显示」卡，任意输入离开。
- 进阅读器**断开无线关联**（不再做 `WIFI_OFF+deinit`：真机上它产生了可复现的 `Cache error`，见证据页第 4 行），
  首次弹一次说明「WiFi off / Pages need the memory; WiFi returns when you leave.」；
  退出后引擎镜像卸载（常驻只在阅读时）。

## 设备实测（2026-11-22/23，第一次真正上机）

- **矩阵：27 通过 / 26 失败 / 4 跳过**（`/texttest.pdf`）。失败归因已逐条查明：
  1. `CMD:LOCK` 台架任务栈只有 4 KB → `Stack canary (PwrLockBench)` 崩溃并重启，
     **其后所有段落都在一个刚重启、阅读器已被关掉的设备上跑**（大量 “no PDF reader is open” 由此而来）。
     已改为 16 KB。
  2. 退出时**卸载引擎**会产生可复现的 `Cache error`（下次打开时 mmu 卸载/重映射周期，2/2 vs 0/2 A/B），
     已关闭卸载（“只在阅读时常驻”让位于“不崩”，引擎留在会话内）。
  3. 深度关射频（`WIFI_OFF`+`deinit`）同样触发 `Cache error`，且推迟到内部栈无效 ⇒ 回退到只断关联的
     `releaseRadio()`。
  4. 选择/批注/合并类失败多为**夹具**：`/texttest.pdf` 与 `/testcomplex.pdf` 的页面走位图路径，抽不出文字；
     需要真正的文字页（卡上 `pdf_english.pdf` 等可试）。
  5. 射频“关”是异步的：12 s 后仍 `status=6`，矩阵已改为多次采样。
- **已通过且与用户要求直接相关**：射频释放、一次性提示（首次 armed / 下次不重复）、失败卡两胶囊、
  Confirm 离开、跳过不复活、跳过不重启、退出内部 RAM 健康、引擎保持映射、语言包匹配、引擎状态/ABI/清单配对。
- **待完成**：刷入 `229b239b` 后重跑矩阵（修掉 LOCK 崩溃的连带污染），并把选择类夹具换成文字页。

## 证据

- 主机 **44/44** 套件（新增 8 套，全部含变异步骤）；静态扫描三件（槽位释放、传输锁、卸载栈）+
  `test_matrix_parsers`；镜像自证字符串与 appTag 配对在每次构建时校验。
- 设备：**未验证**。一次物理断电重启后跑
  `bash exp/pdf_accept_all.sh --wait=3300`
  ——自带四个夹具（含中文文字书）、刷写按 3 个 `Hash of data verified.` 判真、跑两张卡截图、
  矩阵 1e（射频释放/一次性提示/失败卡/跳过/卸载/键盘重发/`cpu=240` 对照）与三个 UX 场景
  （碎内存重扫、慢扫、非 PDF）。

## 如果下次会话开始时设备仍静默

1. 先给设备**物理断电**（拔插 USB ≥2 秒或长按电源关机再开机）：USB 枚举与 `ioreg` 的 matched/active 都
   不能证明 CPU 在跑，DTR/RTS 三种复位组合也实测无效（`docs/engineering/device-recovery.md`）。
2. 上电后在本 worktree 执行 `bash exp/pdf_accept_all.sh --wait=3300`；它自带夹具、刷写判真、截图、
   矩阵与三个场景，并自动把设备列写到 `/tmp/device_column.md`。
3. 若矩阵有红：`/tmp/matrix_full.log` 是完整输出，`artifacts/pdfux_*.png` 是屏上证据；
   证据页的"验收时每行要看什么"逐行写明判据。
4. 设备若在刷写中途失联，**不要连续重试**——按第一条断电，再从头跑一次。

## 仍未做（诚实）

- 设备列（上条）全部未回填。
- MuPDF 单次调用不可中断：靠 75 s 等待 + 90 s 阶梯预算兜底，不是真取消（要真取消需改引擎 ABI）。
- 首次慢重排仍会先静默一小会儿（自适应出卡只对"上次也慢"生效）。
- **打开文档本身没有等待卡**（通常 <1 s；加一张卡等于每次打开多一次面板扫描，得不偿失），大文件首次打开若明显变慢，会表现为"停在书架一小会儿"。
- 后台预取占用渲染任务期间，翻页最多干等 **8 s**（该窗口无法画卡——渲染任务持有帧），已从 20 s 收紧。
- `readpico_nightly`/C3 未重编（PDF 仅 PSRAM 目标；C3 构建会作废 readpico_pdf 的构建产物）。

## 设备验证结论（2026-11-23，最终构建 `fc320116`）

- 矩阵 **28/28 通过、0 失败、4 跳过**（跳过均带理由）；UX 场景 **8/8**；截图 5 张在 `artifacts/`。
- 三条用户要求逐一在真机成立：失败卡可重试/跳过、不重启；慢页有等待卡、粗页能出；进阅读器深度关射频
  （`mode=0 status=254`、free +25 KB）并首次提示一次；键盘/弹窗遮挡时串口命令会被转交而不是吞掉。
- 设备上发现并修复的三个新缺陷：`hasStackedActivity` 的指针比较、LOCK 台架栈溢出、退出卸载引擎的
  `Cache error`（卸载已关闭；深度射频释放改为调度到主循环执行）。

## 提交索引（本轮收尾，按时间；由 git log 生成）

- 4d3cdbd1 fix(pdf): a budget break must not bless a truncated page, and pin the deadline rule
- 879adf0d docs: the wait section described the superseded restart-on-timeout design
- 69bb88af fix(pdf): a slow reflow rebuild shows the wait card on the next turn
- 67ee2249 test(pdf): note that the acceptance wait blocks the chain's serial-user check
- 459a1ed6 fix(pdf): a timed-out render must keep the job slot owned
- 7d199de1 docs: record the slot-ownership defect and its static guard
- deb95cb2 test(pdf): assert the keyboard-dismissal route on device
- 16e3a99e docs: row 8's device evidence is now asserted by the matrix itself
- c63fd7f2 test(pdf): prove the download CPU lock on device with CMD:LOCK
- 2e94c7c6 docs: row 9's device evidence is the LOCK bench, not an unscripted download
- 63b490c7 docs: the completion audit — what is proven, what only the device can prove, and the blocker
- 8372ce00 docs: 44/44 host suites on the final tree
- 838095e5 test(pdf): the scenario copes with a pending one-shot hint and a load card
- 7a4b0bc0 test(pdf): the scenario proves the fixture exists before judging the page
- 39da9e6d fix(net): hold the CPU across KOReader sync/auth requests too
- 56c8d471 docs: row 9 includes the KOReader sync/auth locks
- 65e16e4c docs: inventory every blocking wait on the PDF path (the original hang was a wait-side bug)
- e9f69256 test(pdf): automate row 13 (a non-PDF stays and says so, one input leaves)
- 33df9482 fix(pdf): the first-run Wi-Fi notice says what happened and that it comes back
- 7b4d0893 docs: the hint's own copy and the 1868-key data-partition note
- 68a47bab docs: the acceptance preflight aborts before writing if the pairs are wrong
- 469760f2 test(pdf): the ink ratio itself is host-tested, not just its threshold
- fc844b33 fix(pdf): the open path never waits 180 s for a leaked slot
- 7a4a17c0 docs: the open path's slot wait is 30 s, not the 180 s default
- b54e3436 test(pdf): the 240 MHz check could pass without the lock
- 7bc43dc2 docs: row 9's check has an idle control (the old one could not fail)
- 3058a8ac fix(pdf): unmap the engine from an internal stack only
- fe3e9892 docs: the unmap must run on an internal stack (and how it is deferred)
- aaa7a554 fix(pdf): the deferred unmap waits for the reader to be gone
- c3509974 test(pdf): the unmap guard's regex accepts flushScheduledUnload(bool)
- bacacd22 fix(pdf): the deferred flush does not claim an unmap it did not do
- 809f9753 test(pdf): the acceptance brings its own bench book and wakes the device
- eabd670e test(pdf): the bench book is CJK (reflow is CJK-oriented), subset to 69 KB
- 6b2707a7 docs: one-page round report (root causes, the six defects fixed, what the user sees, what is still unverified)
- 2750dfab docs: the report counts the seven fixed defects, not five
- 07b7c16d docs: USB enumeration is not proof the CPU runs (three reset patterns, no sync)
- bcaf2123 test(pdf): the LOCK release assertion needs pump(), not sleep()
- d0f40e63 test(pdf): fill the evidence table from the logs, and stop truncating the matrix
- 7cb13fd7 fix(pdf): the prefetch stall is bounded at 8 s, not 20
- 48ba43f5 docs: the round report states the 8 s prefetch window

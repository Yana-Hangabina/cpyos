# Metalio E-INK4 Plus 移植分析（CPYOS）

> 状态：**BSP + CPYOS 渲染器/字体/i18n + FT6336U/盖板键 + BQ27220 电量 + 书籍引擎 + 阅读器 demo + 完整 Activity 壳均已构建通过；16 级文字已实现；窗口局部刷新暂时降级为整屏推（厂商面板的区段扫描有锁死风险，待实屏验证后再开）；无真机验证。**
> 依据：kiikoread `e893b1f`（`metalio-s31-v0.3.3-rc110`）、厂商 MIT 仓库
> `CloudZao/Metalio-E-INK4-Plus` `2b2c36a`（v2.0.74）、CPYOS dev 树（`feat/jjwxc`）。
> 本文是工程分析，不构成法律意见；许可证结论见第 3 节与 `THIRD_PARTY_NOTICES.md`。

## 0. 结论

1. **Metalio E-INK4 Plus 不是现有 `metalio_eink4` 的变体。** 现有目标是 ESP32-S3 +
   800×480 SSD1677 SPI；Plus 是 **ESP32-S31 + 4.7″ 1216×684 并口 EPD + TPS65185**。
2. **当前 CPYOS 构建栈无法直接编译 Plus。** 本机 PlatformIO 使用 pioarduino
   `55.03.311`，其 Arduino/ESP32 目标里没有 `esp32s31`；ESP-IDF **v6.1** 才提供
   `esp32s31` 支持。
3. **推荐路线：独立的 ESP-IDF 6.1 目标**，以厂商 MIT 工程为 BSP 参考，把 CPYOS 的
   纯 C++ 上层逐步移植过来；**不复制** kiikoread 的 Personal-Study 文件。
4. **没有真机时只能做到“可构建 + 主机测试”**，不能声称硬件已验证。发布时必须标注
   `UNTESTED ON HARDWARE`，且不进入稳定通道。

### 0.1 当前构建结果（2026-11-07）

- ESP-IDF **6.1** + `esp32s31`（preview target）构建成功。
- 默认 BSP 构建：已链接 `MetalioEpd_Init`（E0470A14-AF、GC16 测试图案）、
  TPS65185/TCA9555、FT6336U、4-bit SD 挂载、BQ27220 电量计；bin **393,952 B**，
  CPYOS app 槽剩余约 97%。
- `CPYOS_UI_PROBE=1` 构建：在 BSP 之上链接了 **CPYOS 渲染/配置/字体/书库栈**：
  `GfxRenderer` + CPY 主题（`UITheme`/`CpyTheme`/`BaseTheme`，bring-up 页会画卡片框与
  主题状态栏）、内置 12pt 中文字体、`I18n`（端口只有 EN+ZH，配置期生成）、`CrossPointSettings` + 持久化、
  `KOReaderCredentialStore`、ArduinoJson、`FreeInkUI`、FreeType + `TtfEpdFont` +
  `SdCardFontSystem`，以及**完整的书库/书籍引擎**（ZIP/miniz、EPUB、TXT、FB2、XTC、CBZ、
  expat、JPEG/PNG/GIF 解码、`library::buildLibraryIndex()`）；bring-up 页会**真实扫描 SD 卡
  并写出 `/.crosspoint/library.idx`**，显示书/目录/解析计数；**盖板 home 键会执行真实 Wi-Fi
  扫描**（`esp_wifi` station + `esp_tls` HTTPS 客户端）。bin **3,318,064 B**，app 槽剩余约 78%。
  这是 bring-up 页，不是首页。
- `sdkconfig.defaults` 已对齐参考固件/厂商要点：`IDF_EXPERIMENTAL_FEATURES`、
  `FREERTOS_HZ=1000`（TPS PGOOD 毫秒轮询）、`FATFS_LFN_HEAP` +
  `FATFS_API_ENCODING_UTF_8`（中文书名）、`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`、
  `COMPILER_OPTIMIZATION_PERF`、主任务栈 12 KB。
- `CPYOS_READER_DEMO=1` 构建：**可用的阅读 demo**——先显示 SD 卡上的 `.epub` 书架
  （最多 50 本，点行/上下键选择），打开后 ZIP/OPF 解析 → `Section` 排版 → `Page` 渲染
  （文本/CSS/图片，内置中文字体）→ 上屏；点击左右区域/滑动/盖板键翻页并跨章，
  `Back` 回书架。跳过 Activity 层，用于无真机时的阅读核心验证；bin **2,443,936 B**，
  app 槽剩余 69%。
- `CPYOS_SHELL_DEMO=1`（配合 `CPYOS_ACTIVITY_PROBE=1 CPYOS_UI_PROBE=1`）：**完整 CPYOS 活动壳**已在
  Plus 上链接通过——`ActivityManager` + CPY 主题 + 首页/最近、书架、设置、应用、阅读器、文件浏览、
  图片查看、睡眠/待机；Wi-Fi（STA + SoftAP）/esp_tls/EPUB/书库引擎、IMU、受保护 EPUB
  （前光当前是 stub，未驱动 LED，见下）
  均为真实实现。**完整安装**采用单槽 14.375 MiB app + 1.5 MiB spiffs（`--layout port --full-install`，
  破坏性改表；OTA 在实机验证前硬关闭）；**实机首测沿用厂商 5+5 MiB 槽**，只刷 bring-up/reader
  （3.3/2.4 MB），完整 shell 9.28 MB 放不进 5 MiB 槽。因此中文 UI 使用真实 **14pt / 16pt** i18n 字面（12pt GB2312 宽字面兜底）；
  其余 **33 种语言**在 `build/i18n_pack.dat`（345,372 B）里，运行时从 spiffs 分区
  `+0x100000`（raw `0xF70000`）读入 PSRAM；没刷包时回落 EN/ZH，其他语言显示为不可用、不崩溃。
  内置 Noto Emoji 子集（`build/emoji.dat`，126,544 B）从同一分区 `+0x160000`（raw `0xFD0000`）
  读取；缺包只让 emoji 退回系统字面的方框，不崩溃。
  全部游戏、计算器、头像、Buddy、AirPage、PixelSwitch **均已编入**；完整 CPYOS
  网页服务 UI 与 Calibre 页面仍通过 `CROSSPOINT_PORT_NO_NETWORK_SHELL` 排除；端口另有
  **最小 HTTP 文件传输**（`main/web_transfer.cpp`：list/download/upload/delete，路径限制在
  `/sdcard` 且屏蔽 `/.crosspoint`，上传走 `.part` 再改名，无鉴权、仅建议在自建 SoftAP 上使用）
  和 **Calibre WebSocket 上传**（`main/ws_upload.cpp`，端口 81，实现文档协议的
  START/READY/PROGRESS/DONE，单上传、`.part` 后改名）。
  bin **9,276,080 B**，app 槽剩余 39%（约 5.9 MB）。
- 共享源码的三处小改动（对 Arduino 构建无行为变化）：`ActivityManager` 的两个 port guard、
  `lib/I18n` 的头文件改用尖括号包含（让端口生成的键表生效）、`CatalogActivity` 用 `WIFI_OFF`。
- ★ **工具链**：ESP-IDF 6.1 + CMake 4.x 组合下，IDF 的属性写入器不转义值里的引号
  （`IDF_VER="v6.1"`），首次全新配置会失败；运行
  `python3 ports/metalio_plus/scripts/patch_idf_cmake_quotes.py $IDF_PATH`（幂等）修复。
- `esp_app_desc`：project `cpyos_metalio_plus`，version `0.0.0-untested`。
- 已实现但**未硬件验证**：16 级文字（4bpp 画布 → GC16 整屏）。**窗口/区段局部刷新暂时关闭**：
  `HalDisplay::displayWindow` 现在降级为整屏 GL16 推（厂商固件从不使用区段扫描，Read Pico 侧有过
  面板锁死史）；`cleanWindow` 仍是整屏 GC16。只有加上有界扫描看门狗并在实屏验证后才重新开启。
- 未移植：OTA 发布通道（单槽布局）、完整 CPYOS 网页服务 UI（需 Arduino WebServer 兼容层）与
  Calibre Activity。其余（USB MSC、BLE HID、最小 HTTP 文件传输、Calibre 端口 81 的
  WebSocket 上传协议、14/16pt CJK）均已实现。
- 多语言已接入：EN/ZH 内嵌，其余 33 种从数据分区语言包加载（未刷包时优雅回落）。
- 字体扩展包已编入真实实现并带默认清单 URL，安装/校验/回退链路尚未在真机走通。
- 受保护 EPUB 已接入：共享胶水以 `FREEINK_CONTENT_PORT_PSA` 选用 PSA 后端（AES-128-CBC + SHA-1/SHA-256），
  旧权利方案的 RSA/PKCS#12 客户端辅助函数在端口上明确拒绝；启动时跑 AES-128-CBC 往返 + SHA-256 已知向量
  自检（日志 `PSA self-test ok/failed`），尚未实机验证。
- **无真机验证**；这是 build-only bring-up，不是可发布的 CPYOS 固件。

### 0.2 已完成的移植件（2026-11-06）

| 件 | 位置 | 说明 |
|---|---|---|
| 厂商 BSP 子集 | `components/cpyos_metalio_bsp/vendor/board/` | MIT，仅保留 E0470A14-AF 显示、TPS65185/TCA9555/I2C1 与触摸；`PROVENANCE.md` 记录改动（上电失败改为 return，不再 `esp_restart()`） |
| `HalDisplay` | `components/cpyos_ui/HalDisplay.cpp` | CPYOS 1-bit 帧缓冲 → epdiy 4-bit；首帧 `from_white` GC16，之后整屏 GC16；16 级 4bpp 画布；`displayWindow` **降级为整屏 GL16**（区段刷新关闭），`cleanWindow` 整屏 GC16 回退 |
| 充电器状态 | `components/cpyos_metalio_bsp/plus_charger.c` | 只读 CX25601N `STATUS1(0x1E)`：VBUS 类型 + 充电阶段；作为 USB 在位第三来源并出现在 `batt`；**不写充电参数**（电芯额定电压本仓库无据，改 VREG 属于猜测） |
| BLE HID 主机 | `components/cpyos_ui/ble_keyboard_host.cpp`（纯解码在 `ble_hid_decode.h`） | IDF NimBLE 中央：扫描/连接（Just Works 绑定）/订阅 HID Report/键盘与消费者报告解码；接入共享 `BleInput`/`MappedInputManager` 翻页映射，入口是共享的**设置 → 控制 → 蓝牙**页（扫描/已配对/忘记/断开）；开启后 shell 复刻参考主循环的自动保活（阅读器内按内存闸门启动、失败 2 s 重试）；绑定写 NVS（4 槽、最旧淘汰）、按住键 500 ms 后自动重复；带 Report ID 前缀的报告也已处理；原始按键边沿与 passkey 未实现；控制台 `bt` 诊断；宿主回归 `bash test/run_ble_hid_decode_test.sh` |
| USB 大容量存储 | `main/usb_msc.cpp`（`esp_tinyusb` 2.4） | 单个 LUN 暴露 microSD；`usbmsc on` 先脱开 app FAT/VFS（不反初始化 SDMMC host），冻结 Activities，只保留控制台循环；无回挂路径，需断电重启恢复；OTG 占用 PHY 时串口可能消失 |
| 文件传输 | `main/web_transfer.cpp` | IDF `esp_http_server`：网页列表/下载/上传/删除，SD 根、路径校验、`.part` 原子替换；控制台 `web [ssid] [pass]` / `weboff` |
| 首次上机清单 | `docs/BRINGUP.md` | 备份/VCOM/刷写/期望日志/逐项 selftest/touchraw/前光/IMU/存储/恢复路径 |
| 串口诊断台 | `main/shell_demo.cpp` | USB-Serial/JTAG stdin 的零超时 select 控制台：`info/batt/rtc/sd/imu/touch/crypto/wifi/ap/web/light`，供首次上机逐件确认，不阻塞 UI |
| 前光 | `main/shell_demo.cpp` + `lib/hal/HalFrontlight.cpp` | **本构建未启用**：`FREEINK_CAP_FRONTLIGHT` 为 0，`FrontlightManager` 是 stub，GPIO6/7 不输出；`light`/`/api/frontlight` 只改设置。配置仍是厂商的 20 kHz/10-bit 冷 GPIO6 + 暖 GPIO7；启用前必须在 `FrontlightManager`/`HalFrontlight` 把 UI 100% 限到 PWM 满量程的 **10%**（厂商 `FL_OUTPUT_MAX_PERCENT`），否则可能过驱 LED/驱动 |
| USB 在位 | `components/cpyos_ui/HalGPIO.cpp` | 读 USB-Serial/JTAG SOF 帧计数（与参考板同一探针），1.5 s 保持；驱动充电图标与 `wasUsbStateChanged()`；纯充电源由 BQ27220 电流符号兜底（1 Hz 采样），满电+哑充电器仍可能读成未连接 |
| 电源键 | `components/cpyos_ui/HalGPIO.cpp` | TCA9555 P1.7（低有效）映射为 `BTN_POWER`；shell 短按进入待机覆盖层；深度睡眠仍未实现（电源轨由 PMU 掌握） |
| `Wire` / IMU | `components/cpyos_ui/Wire.cpp` | Arduino `Wire` 事务映射到 BSP 共享 I2C1（SDA0/SCL1）；共享 `Imu` 驱动因此可探测 SC7A20H@0x19，`HalTiltSensor` 在 shell 里启动并每轮 `update()`（倾斜/摇晃翻页）；未实机验证 |
| `HalGPIO` | `components/cpyos_ui/HalGPIO.cpp` | FT6336U + 盖板三键；tap 28 px / swipe 60 px / 长按 500 ms；竖屏逻辑坐标 → 面板归一化坐标（`tapToLogical` 的逆变换）；**双指捏合**读取第二触点（0x07..0x0A），累计比例、每手势一次事件（数学有宿主测试，真机未验证） |
| `HalStorage` | 既有 simulator POSIX 实现 + `/sdcard` | 4-bit SDMMC + FATFS + UTF-8 LFN；`statvfs`/rename/目录遍历均走 VFS |
| `HalClock` | 改造 simulator 实现 | 开机从 PCF8563 读 UTC 并 `settimeofday`；`setUtcTime` 同时写回 RTC |
| `HalPowerManager` | 改造 simulator 实现 | BQ27220 读数（mV/mA/SOC）；电压→百分比；DVFS 尚未接 |
| `HalSystem` | 改造 simulator 实现 | `restart`→`esp_restart`；设备名/`esp_read_mac` |
| 电量计 / RTC / SD 辅助 | `plus_battery.c`、`plus_rtc.c`、`cpyos_sd.c` | 共用 I2C1 总线锁；SD 挂载后暴露 `cpyos_sd_is_mounted()` |
| 书库/书籍引擎 | `components/cpyos_books/` | 共享 `lib/Epub`、`lib/Txt`、`lib/Fb2`、`lib/Xtc`、`lib/Cbz`、`lib/ZipFile`、`lib/miniz`、`lib/LibraryIndex` 等；expat/uzlib 为独立组件 |
| 阅读 demo | `main/reader_demo.cpp` | SD 书架、真实 `Epub`/`Section`/`Page` 渲染、触摸/盖板键翻页与跨章；不含 Activity/设置/网络 UI |
| 活动壳 | `main/shell_demo.cpp` + `components/cpyos_app/` | 完整 `ActivityManager` 与共享 activities；游戏全部编入；网页服务/Calibre 以 port guard 排除；BLE HID 为主机真实实现 |
| 图像解码 | `components/imgcodecs/` | JPEGDEC / PNGdec / AnimatedGIF（Apache-2.0，来自 CPYOS 依赖树） |
| 压缩/字体 | `components/uzlib_lib/`、`components/freeink_font/` | uzlib；FreeInkFont + FreeType（含 CFF 包装源） |
| 兼容垫片 | `components/cpyos_ui/compat/` | Arduino/Preferences/MD5/base64/WiFi/Client、wolfCrypt→PSA 的 AES-GCM/SHA-256 |


### 0.4 目标完成度审计（2026-11-07；软件证据，不等于实机验证）

| 要求 | 证据 | 状态 |
|---|---|---|
| 项目级自定义 LICENSE：晋江组件闭源、禁商用、非开源的边界 | `LICENSE`（CPYOS Project License 1.0，§6.4 明确 copyleft 优先：epdiy LGPL-3.0、wolfSSL GPLv2、MuPDF AGPL-3.0 按其自身许可）、`NOTICE-JJWXC.md`、`LICENSES/*`、`scripts/tests/test_license_scope.py` **11/11**（含端口无 Personal-Study 头、未逐字节复制其 11 个文件、每个第三方 vendored 组件都有署名条目的断言） | 完成 |
| kiikoread 文件级许可与复制范围 | `ports/metalio_plus/scripts/inventory_kiikoread.py` 复跑 rc110：140 个文件 = 87 Apache-2.0 + 11 Personal-Study + 4 LGPL(epdiy) + 3 MIT(厂商) + 35 无头；111 个共享分支代码文件（main 67 / components 22 / tools 12 / boards 10）。详见 §2.6 | 完成（分析） |
| 不过度防御、按中国法律说明对晋江的支持（不侵入、加密维护正版） | `NOTICE-JJWXC.md` 中「不侵入 / 著作权法 / 设备绑定密钥 / 未购买」断言 | 完成 |
| 第三方许可与来源标注 | `THIRD_PARTY_NOTICES.md`（vendor MIT、esp_tinyusb、TinyUSB、NimBLE、esp_http_server、FreeType、Noto OFL、Lucide ISC…）；端口 README「Provenance」；新测试 `test_plus_managed_components_are_attributed` | 完成 |
| kiikoread readpico ↔ E-INK4 Plus 差异分析 | 本文 §1（硬件矩阵）与 §2（rc92→rc110 逐项差异）；Personal-Study 文件明确「不复制」 | 完成（分析） |
| 厂商固件注意事项 | README「Hardware cautions」（VCOM、TPS 时序、面板波形、SD、USB PHY）＋ `docs/BRINGUP.md` | 已记录；需实机确认 |
| 厂商应用层逐项处置 | `inventory_vendor_app.py` 对 `vendor/main` 46 个顶层条目全部给出处置（ported 3 / reference 2 / replaced 13 / not-ported 5 / not-applicable 20 / build 3），新增条目未分类即失败；逐项附录 `docs/engineering/metalio-eink4-plus-vendor-app.md` | 完成（分析） |
| kiikoread 应用层逐项处置 | `inventory_reference_app.py` 对 rc110 的 66 个区域给出处置（ported 2 / reference 32 / replaced 17 / not-ported 2 / not-applicable 9 / build 4），未分类即失败；附录 `docs/engineering/metalio-eink4-plus-rc110-app.md` | 完成（分析） |
| 完整支持的软件实现 | BSP、渲染/字体/i18n、书籍引擎、Activity 全量（含游戏/头像/Buddy/AirPage/PixelSwitch）、Wi-Fi STA+SoftAP、TLS、HTTP 文件传输与文件管理 API、**官方 `FilesPage.html`（中英，含 JSZip 文件夹上传）**、WebDAV、Calibre WebSocket 上传、USB MSC（含页面内两击进入）、BLE HID、受保护 EPUB、IMU（含**双指捏合**调字号）、马达、音量键、电源键、充电状态；**前光当前是 stub（未驱动 LED，待机关灯逻辑随之无效）**；**文件传输/Calibre 由端口原生 `PlusTransferActivity` 页面提供**（HTTP/WebDAV + 81 端口上传、实时进度、USB 入口、退出释放射频）；`/api/diag` 与 `/diag` 提供远程自检；**离线拼音与断词数据包在 port 全量布局中随刷写写入**（0xE70000 / 0xF00000；vendor 布局不写数据包），控制台 `pinyin`/`hyphen` 与 `/api/diag` 可验证；**晋江私有组件**通过 `CPYOS_PLUS_JJWXC=1` + `src/jjwxc` 选入，实测 27 个源文件、ELF 345 个 `JjwxcActivity` 符号、镜像 9,106,848 B（+376,288 B），缺目录时 configure 直接失败 | 软件完成（见下两项例外；晋江组件不在公共 bundle 中） |
| 未移植/明确阻断 | **CPYOS 网页服务页的其余部分**（`/files` 与 `/fonts` 已直接复用官方 `FilesPage.html` / `FontsPage.html`（中英双版，内联后由端口 HTTP 服务；字体页支持列出/上传/删除 SD 字体，下一次进入字体设置即重扫生效（`markRegistryDirty`，无需重启））；官方 `HomePage.html` 也已直接服务于 `/`；`/settings` 为只读页（展示当前设置并可调前光）；其余设置写接口（Wi-Fi/OPDS/排版）仍未移植；Calibre 插件使用的 WebSocket 上传协议已按文档实现（端口 81），但未与真实插件联调；**OTA**（单槽布局，只能串口刷写）；**浅睡/关机**（序列已按厂商源码记录，待真机验证，见 §2.7）；**电池保护**（厂商 CX25601N 欠压 3.1 V BATFET 关断与 P1.3 关机脉冲未移植；首测请保持 USB 供电）；**音频/AI 语音**（厂商 I2S 编解码器与云端）；**4G/Wi-Fi 双模**；**双色温前光**（当前 capability=0，启用前需 10% duty 限幅） | 未完成，已记录并说明原因 |
| 发布物标注「未经实机测试」 | `esp_app_desc.version = 0.0.0-untested`；`scripts/package_metalio_plus.py` 强制校验（镜像标签、app 槽、语言包键签名、emoji 包头/CRC）；README 顶部横幅；包内 `UNTESTED-ON-HARDWARE.txt` | 完成 |
| 构建与宿主回归 | 四个 Plus 变体 SUCCESS（BSP 394,256 / bring-up 3,321,904 / reader 2,446,144 / shell 9,283,936 B，`esp_app_desc` 均为 `cpyos_metalio_plus` / `0.0.0-untested`）；`readpico` SUCCESS（7,118,448 B，`crossmux` / `640b9069-dirty`，共享改动未破坏 Arduino 构建）；受保护 EPUB 宿主全过；`test_license_scope` **11/11**（含 rc110 文件级许可清单、未复制断言与 vendored 组件署名；带参考树运行时同时验证逐字节未复制）；`verify_port_features.py` 证明 HTTP API/WebDAV/WebSocket/USB MSC/受保护 EPUB/官方网页/字体接口的标记与符号真的进了镜像，**公开镜像不含私有 JJWXC 符号与字节标记**，且 reader/BSP 不含 shell 专属服务（含变异测试 `test_verify_features`）；`test_docs_artifacts` 保证 README 尺寸/哈希与产物一致、包内成员与本地产物逐字节相同；**端口源码零警告**；`test_transfer_paths`、`test_multipart`、`test_form_fields`、`test_bmp_export`、`test_webdav_format`、`test_touch_map`、`test_ble_hid_decode`、`test_ws_protocol`、`test_package_metalio_plus`、`test_flash_metalio_plus`、`test_verify_bundle` 全过 | 完成 |
| 刷写与备份安全 | `scripts/flash_metalio_plus.py` 有**两套互斥布局**：`--layout vendor`（默认，读 `partitions_vendor.csv`：`ota_0@0x80000`/`ota_1@0x580000`，只写 app，拒绝 >5 MiB 的 shell）与 `--layout port --full-install`（`partitions.csv`：写 0x8000 分区表 + app@0x10000 + 四个包，强制 `--backup`，`otadata` 已初始化时拒绝）。写前读真实分区表并校验芯片/16 MiB flash/eFuse（secure boot 或 flash encryption 开则拒绝）；拒绝非 `cpyos_metalio_plus`/非 `untested` 镜像；全量安装把分区表**最后**写并在写后回读 app 头；强制 16 MiB 备份且校验长度；主机回归 **17/17**（含 vendor 槽位、shell 超限、数据包拒绝、过期分区表拒绝、两种 esptool verify 文案、二进制往返） | 完成（已实机刷写 bring-up 两槽，备份与回读校验通过） |
| 发布包独立校验 | `scripts/verify_metalio_bundle.py`：重读 zip，校验四镜像 app_desc/槽位、PYDB/HYPN/JILP/EMJ1 头、SHA256SUMS 全覆盖与逐项摘要、UNTESTED 通知与法律文件；**17 个哈希成员，包内镜像/数据包与本地产物逐字节相同**；`test_verify_bundle` 3/3 | 完成 |
| 一键主机回归 | `ports/metalio_plus/scripts/run_host_tests.sh`：license/rc110 **11** + packaging/flash/bundle/feature/console-docs/sleep-docs/vendor-inventory/jjwxc-wiring **86** + BLE + transfer + WS + multipart + form + BMP + WebDAV + touch + 镜像特性校验 + inventory；全部通过 | 完成 |
| 资源基线（2026-11-07） | `idf.py --preview size`：DIRAM **238,556 / 503,744 B（47.36 %）**，余 **265,188 B**；LP SRAM 4,148 / 32,768 B；Flash 9,122,514 B（15.07 MB app 槽）；bootloader 余 10 %。**内部 RAM 才是联网的稀缺资源**，加子系统后必须重测 | 完成（构建口径） |
| 实机验证（VCOM、面板、触摸、IMU 轴向、前光极性、SD、USB MSC/BLE、充电） | `docs/BRINGUP.md` §1a 首台记录：TCA 0x20、TPS 0x68、`TPS PowerOn ok`、48 帧/568 ms、FT6336 fw v03、SD 4-bit 40 MHz、BQ27220、PCF8563、I2C 七个地址均通过 | **部分完成（2026-11-09）**：VCOM 未测；面板光学、触摸/盖板键、USB MSC/BLE、前光（stub）、充电写入仍未验 |

★ 本表刻意不把「构建通过」当作「设备可用」。在任何一项实机检查完成之前，目标不能标记为完成。

### 0.3 应用层现状与剩余阻塞（2026-11-06 更新）

活动层**已经能链接并通过构建**：单槽容量允许后，游戏、计算器、头像、Buddy、AirPage、
PixelSwitch 全部参与编译（`CROSSPOINT_PORT_MINIMAL_APPS` 已移除）；只剩
`CROSSPOINT_PORT_NO_NETWORK_SHELL` 排除网页服务/Calibre 页面。
`CPYOS_SHELL_DEMO=1` 会注册 UI 字体、字体缓存、SD 字体系统、主题、时钟与 `ActivityManager`，
从首页/最近进入完整界面。

**已具备**（构建通过，均未实机验证）：

- 真实 `esp_wifi` station + lwIP socket + `esp_tls`（证书 bundle）；`Settings → Wi-Fi` 打开
  真实的 `WifiSelectionActivity`（扫描/密码输入/已保存网络）。
- 书籍引擎（EPUB/TXT/FB2/XTC/CBZ + 库索引）、阅读器活动、16 级文字（窗口局部刷新暂时降级为整屏推）、
  受保护 EPUB 读取路径（PSA 后端）。
- 设置/持久化/i18n（EN+ZH）/主题/FreeType 矢量字体；14/16pt CJK 已编入，33 种语言走数据分区包。
- 待机：`设置 → 系统 → 自动休眠时间` 现在驱动**待机覆盖层**（无输入且 Wi-Fi 关闭时进入，电源键唤醒）；
  **浅睡/深睡仍未实现**，不会切断射频或存储。
- PCF8563 RTC + **SNTP 对时**（`esp_netif_sntp`，本构建 `CONFIG_LWIP_SNTP_MAX_SERVERS=1` 故单服务器；
  按内容区域选 `ntp.aliyun.com` 或 `pool.ntp.org`，成功后写回 PCF8563）、BQ27220 电量、FT6336U 触摸与盖板键、4-bit SD。
- USB MSC（`main/usb_msc.cpp`）与 BLE HID 主机（`components/cpyos_ui/ble_keyboard_host.cpp`，
  设置 → 控制 → 蓝牙；解码抽到 `ble_hid_decode.h` 并有宿主回归）。
- 输入与反馈：shell 循环按参考主循环的顺序执行 `gpio.beginFrame()` → `mappedInputManager.update()` →
  `ActivityManager::loop()`；**修复了此前 shell 从不轮询输入、触摸/按键在活动壳内不可用的问题**。
  音量键（TCA P1.0/P1.1 → Up/Down）与马达（TCA P0.3，设置里的振动档）已接入；
  串口新增 `keys`（直接打印扩展口 P0/P1）与 `haptic [ms]`（直接驱动马达）用于确认电平与极性。
  马达/按键极性仍需实机确认。

**仍未完成 / 明确阻断**（按价值排序）：

1. **OTA**：`OtaUpdateActivity` 参与编译，但端口定义 `CPYOS_OTA_DISABLED`，`checkForUpdate`/`installUpdate`/
   `installDataPacks` 一律返回 `WRONG_DEVICE_ERROR`——网络上现存的清单都指向 Read Pico（ESP32-S3），
   装到本 SoC 上会变砖。`esp_ota` 写内部 flash，将来实现时必须放主任务（不能放 PSRAM 栈）。
2. **完整网页服务 UI / Calibre Activity**：排除在外，前者需要 Arduino
   `WebServer`/`AsyncWebServer` 兼容层。Calibre 插件所依赖的 **WebSocket 上传协议**
   （端口 81）已按 `docs/webserver-endpoints.md` 实现（`main/ws_upload.cpp`），
   用 `web ...` 启动服务后可直接把插件指向设备；真实插件联调仍未做。
3. **字体扩展包的真机链路**：代码与默认清单 URL 已编入，安装/校验/回退尚未在真机走通。
   **水波纹翻页**（`e0470_page_turn`）同样未移植；端口定义 `CROSSPOINT_PORT_NO_WATER_TURN=1`，
   阅读器设置里不出现该条目，避免“选了没反应”。浅睡/深睡的样式与图片行同样由
   `CROSSPOINT_PORT_NO_SLEEP_SCREENS=1` 隐藏；待机样式/图片保留。
4. **硬件验证**：VCOM、TPS65185 时序、面板波形、触摸坐标、SD 4-bit 时序、USB HS/BLE 都只能在
   真机上确认。发布物一律标 `UNTESTED ON HARDWARE`，不进入稳定通道。

完整验收矩阵（构建/宿主/打包/刷写/实机）见
[`ports/metalio_plus/docs/ACCEPTANCE.md`](../../ports/metalio_plus/docs/ACCEPTANCE.md)；
实机各项在打勾之前，发布物一律保留 `0.0.0-untested`。

**不要把 `src/` 复制进 `ports/`**：那是分叉，会在两套代码间产生无法验证的漂移。
正确做法是让端口**引用**共享源码，缺的平台件以 IDF component/桩补齐。

## 1. 硬件矩阵

| 项 | Read Pico | Metalio E-Ink 4（现有） | Metalio E-INK4 Plus |
|---|---|---|---|
| SoC | ESP32-S3，Xtensa 240 MHz | ESP32-S3，Xtensa 240 MHz | **ESP32-S31，RISC-V 320 MHz** |
| Flash / PSRAM | 16 MB / 8 MB | 16 MB / 8 MB | 16 MB / **16 MB Octal 200 MHz** |
| 面板 | E0470A01 1216×684 | SSD1677 800×480 SPI | **ED047TC2 / E0470A14-AF 1216×684 并口 8-bit** |
| 显示电源 | SY7636A（Read Pico 板） | TCA9555 直接控轨 | **TPS65185 + TCA9555** |
| 触摸 | CST836U | CST816S | **FT6336U @0x38，INT GPIO5** |
| 扩展 IO | FCA9555 | TCA9555 | **TCA9555 @0x20（I²C 0/1，100 kHz）** |
| RTC / 电量计 / IMU | 无 / PMU / 无 | BQ27220 + PCF8563 / 有 | **PCF8563 + BQ27220 + SC7A20H** |
| 充电 | PMU | CX25601N | **CX25601N @0x6B** |
| SD | 1-bit SDMMC | 1-bit SDMMC | **4-bit SDMMC（20–25）** |
| USB | USB-Serial/JTAG | USB-Serial/JTAG + MSC | **HS UTMI + USJ，ANA_SW GPIO36** |
| 前光 / 马达 / 音频 | 无 / 无 / 无 | 无 / 无 / 无 | **冷 GPIO6 + 暖 GPIO7 / TCA P0.3 / I²S + 外置 BT** |
| 按键 | 触摸 + 3 电容键 | 触摸 + 扩展键 | **POWER P1.7、Vol± P1.0/P1.1、AI/BOOT GPIO61、玻璃虚拟键** |
| 分区（rpr） | 4M+4M app，5M storage | 无专属表 | **5M+5M app，5.875M storage** |
| 分区（厂商） | — | — | **model 400K、5M+5M app、resources 400K、font_data 5M** |

### 1.1 两套 Plus 分区表不能混用

- kiikoread `boards/metalio47/partitions.csv`：`ota_0@0x10000(5M)`、
  `ota_1@0x510000(5M)`、`storage@0xa10000(0x5e0000 FAT)`、`coredump@0xff0000`。
- 厂商 `partitions.csv`：`model@0x10000(400K)`、`ota_0@0x80000(5M)`、
  `ota_1@0x580000(5M)`、`resources@0xa80000(400K)`、`font_data@0xae4000(5M)`、
  `coredump@0xfe4000`。
- 两者的 app 偏移和资源布局不同。CPYOS 必须选定一套并生成自己的 manifest，
  **绝不能把 Read Pico 的 `partitions_readpico.csv` 或现有 `partitions.csv` 刷到 Plus**。
- **实机首测必须沿用厂商表**（`partitions_vendor.csv`，与 `partitions/v1/16m.csv` 一致）：
  `ota_0@0x80000(5M)` / `ota_1@0x580000(5M)`。bring-up（3.3 MB）与 reader（2.4 MB）
  都能放进 5 MiB 槽；`flash_metalio_plus.py --layout vendor` 会在写前读 0x8000 的真实分区表、
  校验芯片/16 MiB flash，只写 app 槽，不碰 bootloader/otadata/NVS/`model`/`resources`/`font_data`。
  **完整 shell 9.28 MB 放不进 5 MiB 槽**，不能按旧文档写到 `0x10000`。
- **CPYOS 的完整安装布局**（`partitions.csv`，仅 `--layout port --full-install`）：
  16 MB flash 上 `1 × 14.375 MiB app + 1.5 MiB spiffs`
  （`ota_0@0x10000(0xE60000)`、`spiffs@0xE70000`）。它必须**重写 0x8000 的分区表**，
  因此是破坏性全量安装：先整片 16 MiB 备份；若 `otadata` 已初始化，规划器拒绝执行
  （该表没有 `ota_1`，bootloader 可能选中新 app 的中段）。spiffs 基址沿用 Read Pico，
  语言包在 raw `0xF70000`、emoji 在 `0xFD0000`。bootloader 仍是厂商的（`0x2000`），
  端口从不写 bootloader；不保留厂商 `model`/`resources`/`font_data`。

### 1.2 厂商固件注意事项 → 端口处理

来源：厂商 `Metalio-E-INK4-Plus` 的 `README_zn.md`、`boards/common/` 与 BSP 代码；逐条对照本端口：

| 厂商注意事项 | 端口状态 |
|---|---|
| **VCOM** 厂商 `config.h` 默认 **1200 mV**（另有 1600/2000 mV 的“中/强”黑度档） | 端口 BSP 同样写 **1200 mV**，与出厂默认一致；未做标定，也不要擅自调高。改 VCOM 前必须实测，写错可能永久损坏面板 |
| **TPS65185** 经 TCA P0.0/P0.1/P0.2/P0.7 上电，等待 PGOOD（毫秒级） | vendor BSP 原样使用；`CONFIG_FREERTOS_HZ=1000` 保证毫秒等待 |
| 面板 E0470A14-AF **48 相**波形、`my_waveform_du14.h` 为厂商材料 | 端口随 vendor BSP 一起编入 `MY_WAVEFORM`（GC16/GL16 各 48 相、DU 20 相），不是 epdiy 默认表；LUT 数据与厂商出厂 LUT 是否一致仍需实屏对比 |
| microSD **4-bit**，挂卡最多八次：40/40/20/10/5/1 MHz 再单线 5/1 MHz | 端口实现同样的回退阶梯（`cpyos_sd.c` 4-bit 40→1 MHz，再 1-bit 5/1 MHz），不做 RAM 复用/RTC hint |
| **USB HS UTMI** 与 USB-Serial/JTAG 共用 PHY，`ANA_SW`（GPIO36）选路；虚拟 U 盘前要安全弹出 | 端口 `usbmsc on` 先拉低 ANA_SW，TinyUSB 用 S31 单 OTG 端口 0（HS UTMI），16 KiB 任务栈；FAT 单向后不重挂，重启恢复 |
| 充电 CX25601N（0x6B），电流 0.5/1/1.5 A，默认 1 A | 端口**只读** VBUS/充电阶段，不写充电参数（电芯额定电压无据） |
| 前光冷/暖两路 PWM（GPIO6/7） | 端口配置了 20 kHz/10-bit 的冷/暖通道，但 **capability=0，当前不驱动 LED**；启用前必须把 UI 100% 限到满量程 10%，极性/频率仍待实机确认 |
| 震动马达在 TCA **P0.3**，参考脉冲 25 ms | 端口按设置 20/35/60 ms 脉冲、≥100 ms 间隔，使用 BSP 的影子输出；极性待确认 |
| 音量键 TCA **P1.0/P1.1**、AI 键 **GPIO61** | 音量键映射 Up/Down；AI 键未绑定功能，`keys` 命令可读其电平 |
| I²S 麦克风/扬声器经外置蓝牙音频芯片 | **未移植**（需音频编解码与蓝牙音频协议） |
| 浅睡保留 RAM/索引，P17/GPIO2 + EXT1、RTC hint，默认浅睡 10 分钟后断电 | **未移植**（依赖 S31 PMU/MPLL 寄存器序列；必须实机验证，不盲写） |
| `model`/`resources`/`font_data` 分区与 5+5 MiB OTA | **实机首测沿用**厂商 5+5 MiB 槽（只写 app，bring-up/reader 均 <5 MiB）；完整 shell 需 `--layout port --full-install` 的单 14.375 MiB app + 1.5 MiB spiffs（§1.1），该路径改表且要求整片备份 |
| 烧录前整片备份、虚拟 U 盘安全弹出、不要混刷分区表 | `scripts/flash_metalio_plus.py` 提供 `--backup`、分段校验与表/保留字节保护；BRINGUP 要求整片备份 |

## 2. kiikoread 的代码差异

### 2.1 rc92 → rc110

| 项 | 事实 |
|---|---|
| 本地参考 | `.scratch/rpr`，`4bd2b9e`（rc92），无 Metalio；rc110 复核树 `.scratch/metalio-research/rpr-rc110` |
| 上游 HEAD | `e893b1f`，`metalio-s31-v0.3.3-rc110` |
| 总差异 | 1,204 个文件，+197,166 / −9,606 |
| 引入提交 | `688b08e`（设备选择/OTA）→ `dc9c26c`（把 Metalio-S31 树并入主树）→ rc98…rc110 |
| 新板级包 | `boards/metalio47/` 约 127 文件 / 34k 行 |
| 新硬件组件 | `components/metalio_hw/` 13 文件 / 1,923 行 |
| 共享分支 | 111 个 `.c/.h/.cpp` 文件带 `KIIKO_BOARD_METALIO47` 分支（main 67、components 22、tools 12、boards 10） |

### 2.2 `boards/pico` 与 `boards/metalio47` 的板级差异

| 文件 | Pico | Metalio47 |
|---|---|---|
| `board.json` | `esp32s3`，chip 9，bootloader 0，project `Read_Pico` | `esp32s31`，chip 32，bootloader 8192，project `Kiikoread_Metalio_S31` |
| `kiiko_board_config.h` | VCOM 在 PMU/NVS 只读；无前光/马达/音量/TPS/USB route；有 PMU；盲刷修复；rails idle 8s；GL48 裁剪；文字相序调序 | VCOM 在 NVS（默认 3000 mV）；前光/马达/音量/TPS/USB route/anti-fade/cover cache；无 PMU；一次白底基线；rails 每次输出后立即断电；厂家完整 GL48；不做文字相序调序 |
| `board_hw.c` | Read Pico 板级门面 | Metalio S31 门面（TPS65185/FCA9555/前光/马达） |
| `sdkconfig.defaults` | S3、120M flash/PSRAM、240 MHz、Wi-Fi 4/8/4/8、TinyUSB MSC 8K | S31、16M flash、200M PSRAM、320 MHz、Wi-Fi 6/32/6/4（目标值，见 §2.5）、TinyUSB MSC 16K、I²C port1/SDA0/SCL1、IOE INT 2、TP INT 5、100 kHz |
| `partitions.csv` | 4M+4M app，5M storage | 5M+5M app，5.875M storage |
| `src/components` | `epdiy`、`e0470_*`、`read_pico` | 同上 + `epdiy/output_lcd`、`pie_pixels`、`metalio_hw`、`s31_power_key` |
| `src/main` | 有 `app_book`、`book`、`font`、`ui`、`settings`、`lock_screen`、`ota_release` | 有 `metalio_text_ripple`、`idf_component.yml`，并复用/覆盖共享 main 文件 |

### 2.3 共享代码分支（111 文件）

按目录分组：

- `main/ui/`：首页时钟、头像、快捷菜单、菜单、玻璃效果、壁纸、电源对话框、导航、键盘。
- `main/app/`、`main/book/`、`main/font/`、`main/apps/`、`main/settings.c`、
  `main/display*.c`、`main/sleep.c`、`main/ota_update.c`、`main/time_service.c`、
  `main/factory/vcom_setup.c`。
- `components/epdiy/`：`epd_lcd.h`、`epd_board.h`、`highlevel.c`、
  `output_lcd/lcd_driver.h`、`render_context.h`、`render_method.h`。
- `components/e0470_page_turn/`、`components/e0470_epaper_waveform/`。
- `components/cst836u/`、`components/sc7a20h/`、`components/read_pico/`、
  `components/read_pico_transfer/`、`components/read_pico_pmu/`、
  `components/ble_page_turner/`。

### 2.4 关键驱动差异

1. **显示输出**：Plus 不走 S3 的 LCD_CAM，而走 S31 的 LCD/IOMUX + 8-bit 并口；
   `boards/metalio47/src/components/epdiy/src/output_lcd/` 与 `pie_pixels.*` 是新增路径。
   Pico 的 `render_lcd.c` 不能直接复用。
2. **面板时序**：Plus 的厂商波形为 **GC16/GL16 各 48 相、DU 20 相**，帧周期约 12 ms
   （约 83.3 Hz）；实际时序以 vendor BSP 的 `my_waveform`/`EpdTiming` 为准，必须上机验收。
   不要把 Read Pico 的 30 相/85 Hz 参数套到 Plus。
3. **电源**：TPS65185 通过 TCA9555 的 P0.0/P0.1/P0.2/P0.7 控制 VCOM/PWRUP/WAKEUP/PWR_GOOD；
   Pico 的 SY7636A/CW32 PMU 路径不适用。
4. **触摸**：FT6336U @0x38；现有 `cst836u` 驱动地址/协议不同。
5. **SD**：4-bit SDMMC（D0–D3=20–23，CLK/CMD=24/25），不是 Pico 的 1-bit。
6. **USB**：S31 HS UTMI + USJ，ANA_SW GPIO36；MSC 16 KiB，烧录前要安全弹出。
7. **传感器/外设**：RTC PCF8563、电量计 BQ27220、IMU SC7A20H、前光、马达、音量键、AI 键。


### 2.5 rc110 本地证据与 CPYOS 端口对照（2026-11-07 复核）

本地参考树：`.scratch/metalio-research/rpr-rc110`（`metalio-s31-v0.3.3-rc110`）。以下每条都指向该树内的文件，
「端口状态」只描述本仓库实际做了什么，**不把未移植的行为当作已支持**。

| 主题 | rc110 的实现（证据） | CPYOS Plus 端口 |
|---|---|---|
| 波形 | `components/e0470_epaper_waveform/include/e0470_epaper_waveform.h`：GC16/GL16 **各 48 相**（0–50 °C 单温度档）、DU **20 相**、gray8 30 相；`e0470_waveform_trim.h` 可按参数裁剪相数 | 端口随 vendor BSP 编入厂商 `MY_WAVEFORM`（GC16/GL16 各 48 相、DU 20 相），**不是** epdiy 默认表；LUT 数据是否等于厂商出厂 LUT 仍需实屏对比 |
| 分区 | `boards/metalio47/partitions.csv`：`ota_0/ota_1` 各 **5 MiB**、`storage` FAT **5.875 MiB** | 实机首测用厂商 5+5 MiB 槽（bring-up/reader）；完整 shell 用 port 全量安装的单槽 **14.375 MiB** + spiffs 1.5 MiB（改表、需整片备份，见 §1.1） |
| SD 挂载 | `docs/BOARDS.md`「挂卡配置最多八次」：四线 40/40/20/10/5/1 MHz，然后单线 5/1 MHz；成功后 RAM 复用、RTC 提示仅在深睡后接受 | **端口已实现同样的回退阶梯**（`cpyos_sd.c`：4 线 40/20/10/5/1 MHz → 单线 5/1 MHz），但不做 RAM 复用/RTC hint（每次冷启动从高速开始） |
| 睡眠/关机 | `docs/BOARDS.md`「S31关机与锁屏省电」：浅睡保留 RAM/索引，P17/GPIO2 + EXT1、RTC hint；默认浅睡 10 分钟后断电。vendor BSP 已提供全部原语（见 §2.7） | 端口只有待机覆盖层与电源键短按，**浅睡/深睡/定时断电均未实现**：厂商应用层 `power_policy` 源码已取得并逐条对照（§2.7）；端口未移植浅睡，因为 S31 必须回填 MPLL 初始化/偏置，且无真机无法验证（`HalGPIO::startDeepSleep()` 明确拒绝并打日志） |
| 显示加速 | `docs/BOARDS.md`：S31 PIE（Core1）、DMA 双缓冲按 64 B 边界拆描述符、Core1 刷屏协处理、内部 RAM 行队列 | 未移植（走 vendored epdiy 同步扫描）。**但刷新模式已接通**：FULL→GC16、HALF/FAST→GL16 差分；`panel` / `panel fast` 可在真机 A/B |
| JPEG | `docs/BOARDS.md`「S31 JPEG混合解码」：SDK 硬解 + libjpeg-turbo 渐进式回退 | 未移植；端口使用 CPYOS 原有流式解码 |
| 前光/马达/音量 | `boards/metalio47/kiiko_board_config.h`：前光、马达、音量、TPS/USB route、anti-fade；`components/metalio_hw/` | 马达（TCA P0.3，20/35/60 ms 脉冲、≥100 ms 间隔）、音量键（P1.0/P1.1 → Up/Down）已实现；TPS/USB route 走 vendor BSP。**前光当前未启用（capability=0）**；启用前必须加 10% duty 上限。马达与按键极性未实机验证 |
| USB 虚拟盘 | 厂商文档：HS UTMI + **ANA_SW GPIO36** 选路、TinyUSB MSC **16 KiB** 栈、烧录前安全弹出 | 端口 `usbmsc on` 先拉低 ANA_SW，TinyUSB 用 S31 单 OTG 端口 0（该端口本身即 HS UTMI），任务栈 16 KiB；FAT 单向后不重挂，重启恢复。未实机验证 |
| BLE 翻页器 | `components/ble_page_turner/` | 端口以 IDF NimBLE **重新实现**中央/报告解码，未复制该组件 |
| 充电/PMU | `components/read_pico_pmu/`、`components/metalio_hw/metalio_charger.c` | 端口只**读** CX25601N 状态（VBUS/CHG_STAT），不改充电参数（电芯额定电压无据） |
| 文件传输/Calibre | `components/read_pico_transfer/`、CrossPoint Web 插件 | 端口提供最小 HTTP 文件页、文件管理 API、**WebDAV 子集**（`webdav.cpp`：PROPFIND/GET/PUT/DELETE/MKCOL/MOVE/COPY/LOCK）与 **端口 81 的 Calibre WebSocket 上传协议**；完整网页服务 UI 与 Calibre Activity 未编译（见 §0.4） |
| 搜索/WebP | `components/read_pico_search/`、`components/read_pico_webp/` | 未移植（搜索用 CPYOS 自有实现，WebP 无） |

**许可证范围（rc110 `LICENSE` 原文要点）**：截至 `a0573252…`（v0.3.3-rc93）已发布内容继续适用
**Apache-2.0 或各组件原许可**；之后**明确标注** `LicenseRef-Kiikoread-Personal-Study-1.0` 的新增原创内容
才受「个人学习/研究/测试、禁止商用」约束；混合文件只有显式列出的新增部分可能适用该许可。
本项目**没有复制** kiikoread 的任何文件（包含新许可与 Apache 部分），上表的实现均为独立重写或来自
vendor MIT/IDF；rc110 只作为差异与行为参考。因此 `ports/metalio_plus` 不继承 Personal-Study 条款。

**结论**：rc110 与 Read Pico 的差异不止驱动替换，还包括波形表、分区、SD 回退、睡眠策略与显示加速；
本端口的软件面覆盖已按上表逐项标注，**未移植项不得在发布文案中声称支持**。

### 2.6 rc110 文件级许可清单（可复跑）

```bash
python3 ports/metalio_plus/scripts/inventory_kiikoread.py \
  --tree /path/to/read-pico-reader        # --json 供机器读取
```

对 `metalio-s31-v0.3.3-rc110` 的实测结果（2026-11-07）：

| 目录 | 文件数 | Apache-2.0 | Personal-Study | 其他 |
|---|---:|---:|---:|---|
| `boards/metalio47` | 127 | 80 | **10** | LGPL-3.0（epdiy）4 + 无 SPDX 头 33 |
| `components/metalio_hw` | 13 | 7 | **1** | MIT（厂商）3 + 无头 2 |
| 合计 | 140 | **87** | **11** | 42 |

- 11 个 Personal-Study 文件与 §3.1 的列表逐条一致（`metalio_text_ripple.*`、
  `s31_power_key/hooks.c`、6 个 `tests/tools/*`、`metalio_early_key_record.h`）。
- **逐文件清单**（140 个板级/硬件文件 + 111 个共享分支代码文件，含许可与字节数）：
  [`metalio-eink4-plus-rc110-files.md`](metalio-eink4-plus-rc110-files.md)，可用
  `--markdown` 重新生成；它只记录文件名与许可事实，不包含上游代码。
- 共享分支：**111 个代码文件**提到 `KIIKO_BOARD_METALIO47`（main 67 / components 22 /
  tools 12 / boards 10），另有 46 个非代码文件（tools 35、components 7 等）。
  这是 §2.1「111 个文件」的来源，可随时用上面的脚本复核。
- 无 SPDX 头的 35 个文件多为波形/资源/生成物或厂商材料；本项目**没有复制**其中任何一个。
  合规断言：`scripts/tests/test_license_scope.py`
  ① 端口代码里不得出现 `LicenseRef-Kiikoread-Personal-Study` 头；
  ② 若本机存在 rc110 参考树，则按内容哈希断言端口没有任何一个与 11 个 Personal-Study
  文件逐字节相同的文件（离线可跳过）。

### 2.7 厂商电源策略（power_policy）与浅睡序列 —— 源码级对照

**源码位置（本地参考树，MIT）**：`.scratch/metalio-research/vendor` @ `2b2c36a`（v2.0.74），
`main/boards/common/power_policy/`：`power_policy.cc`（何时进入待机）、`power_policy_standby_lp.cc`（清理与分片）、
`power_hw.cc`（硬件动作）、`s31_sleep/`（S31 兼容回填与复位诊断），行为说明见同目录 `README.en.md`。
它**不是** vendor BSP 子集的一部分，也**没有被复制**进端口（MIT；若移植必须在 `THIRD_PARTY_NOTICES.md` 记录）。

**浅睡一拍的准确顺序**（`power_hw_light_sleep_once()`，与 README「Entering light sleep」逐条对应）：

0. 准入：SD 已 quiesce 且 idle、boot work 空闲、无待退出请求；持 SD transition guard。
1. `power_hw_sd_park()`：卸载 SD，释放 SDMMC/CNNT 路由，数据/时钟/命令脚设方向、清 pull、驱动 0 并 `gpio_hold_en()`（幂等）。
2. `tps_enter_sleep()` + `bus_hold_low()`（仅首次进入待机时做一次）。
3. `MetalioAccel_ReleaseIntLine()`：先释放共享 INT 上的加速度计中断。
4. `MetalioKeys_MaskForSleep()`：停止侧键扫描，按键脚驱空闲高；随后再确认没有待处理的 POWER。
5. `ioexp_enable_power_wake_pin()`：P1.7 改输入并重采样；`WaitIntLineHigh()` 确认共享 INT 已释放，否则放弃这一拍。
6. 关掉所有 wake source → `esp_sleep_enable_ext1_wakeup_io(GPIO2, ESP_EXT1_WAKEUP_ANY_LOW)`
   → `esp_sleep_enable_timer_wakeup(slice)`；**每片 ≤60 s**，临近自动关机时限会缩短。
7. `sleep_cut_rails()`（P5 → P14）+ `bus_latch_for_sleep()`；马达输出高 0.5 s 再拉低（与 UI 震动设置无关）。
8. `s31_sleep::LightSleep()` → `esp_light_sleep_start()`。

**唤醒路径**（同一函数后半）：唤醒瞬间先用 `ReadPowerPinLow()` 锁存 P1.7（此时 USB 控制台可能不可用，
用 `esp_rom_printf`），关闭 EXT1/TIMER 源，再 `MetalioKeys_ResumeAfterSleep()`。
**定时器或共享 INT 唤醒不退出待机**，只恢复按键采样；只有确认退出（POWER/用户操作）才
`restore_rails()` → `sd_restore()` → `bus_restore()`，前光保持 0 直到底层画面完成。

**S31 专有坑（这才是端口拒绝睡眠的真正原因，不是"缺厂商源码"）**：`s31_sleep.cc` 不是普通包装——
- `__wrap_pmu_sleep_start` / `__wrap_esp_sleep_execute_event_callbacks` 把睡眠阶段记进 RTC；
- `__wrap_rtc_clk_mpll_configure` 在 PMU 已屏蔽中断、另一核停转的条件下，回填 ESP-IDF 上游提交
  `9726078b55430ef8d2ce4a460ebf21d889bb5c12` 的 MPLL 模拟初始化，并在**每次唤醒**重写
  I2C_BIAS `DREG_1P1=12 / DREG_1P1_PVT=12`（注释实测：PMU 唤醒覆盖了启动偏置，7/7 次），
  之后才调用真实 `rtc_clk_mpll_configure`；
- 因此**直接调 `esp_light_sleep_start()` 在这颗芯片上可能带着错误的 MPLL/PSRAM 时钟返回**。
  没有真机无法验证这条路径，也不能用"看起来睡了/醒了"来判断成功。

**端口的移植清单（有真机后按序做，不得跳步）**：
1. **SD park/restore**：端口是 FATFS/VFS（`cpyos_sd.c`），需要卸载 VFS、SDMMC 引脚低电平 + `gpio_hold_en`，
   唤醒后重挂；vendor 的 `power_hw_sd_park/restore` 可以对照，但不能直接调用（它绑定 vendor 的 `sd_access` 与 USB 控制台）。
2. **按键遮罩/恢复、P1.7 输入与 INT 检查**：vendor BSP 已有 `ioexp_mask_for_sleep` / `ioexp_enable_power_wake_pin` /
   `ioexp_restore_keys`，可直接用。
3. **TPS / bus / rails**：vendor BSP 已有 `tps_enter_sleep` / `bus_latch_for_sleep` / `sleep_cut_rails`，可直接用。
4. **`s31_sleep` 回填**：必须连同两个 wrapper 与 MPLL 代码一起移植（vendor MIT + 上游 IDF Apache-2.0，
   需在 NOTICES 标注），保留 RTC 诊断；先用 `slpinfo` 记录 `esp_sleep_get_wakeup_cause()`。
5. **待机画面停帧**：浅睡前必须完成一次整屏推送并等待扫描结束（当前 shell 的 `StandbyActivity` 同步推帧可满足），再关前光。
6. **分片循环与退出判定**：POWER 短按退出、定时器唤醒继续睡；Wi-Fi 必须在待机前释放（shell 已要求），
   BLE 需新增释放路径，否则射频在浅睡期间的状态未定义。

**在此之前**：`HalGPIO::startDeepSleep()` 继续拒绝并打日志，自动休眠只显示待机覆盖层；shell 在待机期间关闭前光（唤醒后按 `frontlightOn` 恢复），自动待机仍要求 Wi-Fi 已释放；
`slpinfo`（wake cause / GPIO2 / P1.7 / USB）用于收集真机证据。发布文案不得声称支持浅睡/关机/定时断电。

### 2.8 厂商应用层逐项处置（可复跑）

脚本 `ports/metalio_plus/scripts/inventory_vendor_app.py` 遍历厂商 `vendor/main` 的 **46 个顶层条目**
（目录与根文件），逐项记录处置与理由；**新增条目未分类即退出非零**，厂商升级不会静默漏掉新子系统。
汇总（2026-11-07，vendor `2b2c36a`）：

| 处置 | 条目 | 代码文件 |
|---|---:|---:|
| ported（代码已进入本端口） | 3 | 150 |
| reference（只读参考，未复制） | 2 | 2 |
| replaced（CPYOS 自有实现取代） | 13 | 424 |
| not-ported（明确不做，原因已记录） | 5 | 37 |
| not-applicable（厂商云/UI/资源栈） | 20 | 41 |
| build | 3 | 0 |

逐项清单（文件数、状态、备注）见
[`metalio-eink4-plus-vendor-app.md`](metalio-eink4-plus-vendor-app.md)，可用 `--markdown` 重新生成。
关键结论：厂商 `main/` 的 654 个代码文件里，本端口只直接复用 `boards` 子集与马达脉冲语义；
UI / reader / settings 由 CPYOS 对应实现取代；**音频与 OTA 是仅有的两处真实功能缺口**
（原因分别见 README 硬件注意与 §2.7）；云/AI/蜂窝协议栈属厂商服务，不构成本地阅读器的缺失。

### 2.9 kiikoread 应用层逐项处置（可复跑）

`ports/metalio_plus/scripts/inventory_reference_app.py` 把 rc110 的 **66 个区域**
（`boards/*`、`components/*`、`main/` 子目录与根文件、`tools/`、`assets/`、`docs/`、`cmake/`、`flash/`）
逐项映射到本端口的处置；**出现未分类条目即退出非零**。汇总（2026-11-07）：

| 处置 | 区域 | 代码文件 |
|---|---:|---:|
| ported（代码已进入本端口） | 2 | 12 |
| reference（只读参考，未复制） | 32 | 845 |
| replaced（CPYOS 自有实现取代） | 17 | 213 |
| not-ported（明确不做） | 2 | 77 |
| not-applicable（锁屏/资产等） | 9 | 127 |
| build | 4 | 48 |

逐项理由见 [`metalio-eink4-plus-rc110-app.md`](metalio-eink4-plus-rc110-app.md)。
`ported` 只有 `e0470_epaper_waveform` 与 `e0470_page_turn`（水波纹与波形/伽马，均带 Apache 署名）；
`read_pico_pmu`、`main/sleep.h` 等属于参考（§2.7 的源码级对照），`pwm_audio`/`read_pico_webp`
明确未移植。这份处置表与 §2.6 的许可清单互补：前者回答「怎么处理」，后者回答「许可是什么」。

## 3. 许可证与来源

### 3.1 上游许可事实

- 根 `LICENSE` 是分范围声明（2026-10-15）；新文件可能标
  `LicenseRef-Kiikoread-Personal-Study-1.0`，**非 OSI 开源**。
- v0.3.3-rc93 及更早、以及显式标 `SPDX-License-Identifier: Apache-2.0` 的文件仍是
  Apache-2.0。
- 在 `boards/metalio47`（127 文件）+ `components/metalio_hw`（13 文件）中，
  按 §2.6 的可复跑清单：
  - **87 个文件标 Apache-2.0**；
  - **11 个文件标 Personal-Study**（`boards/metalio47/src/` 下）：
    `bootloader_components/s31_power_key/hooks.c`、`main/metalio_text_ripple.c/.h`、
    `tests/tools/app_loop_stubs/display_main_waveform.h`、
    `tests/tools/boot_panel_actions_host_test.py`、
    `tests/tools/metalio_key_layout_host_test.py`、
    `tests/tools/metalio_volume_reader_host_test.py`、
    `tests/tools/pico_wake_option_host_test.py`、
    `tests/tools/reader_layout_controls_host_test.py`、
    `tests/tools/wake_resources_host_test.py`，
    以及 `components/metalio_hw/include/metalio_early_key_record.h`；
  - 其余 42 个：**4 个 LGPL-3.0**（epdiy 输出后端）、**3 个 MIT**（厂商文件）、
    **35 个无 SPDX 头**（波形/资源/生成物与厂商材料）。
- `epdiy` 为 **LGPL-3.0-or-later**；`my_waveform_du14.h` 是厂商材料，**无明确再分发许可**。
- 厂商 `CloudZao/Metalio-E-INK4-Plus` 为 **MIT**，包含完整 ESP-IDF 工程与数据手册引用。

### 3.2 取用规则

| 来源 | 可做什么 | 不可做什么 |
|---|---|---|
| 厂商 MIT 工程 | 复制/修改/再分发，保留 MIT 声明与版权 | 移除声明；把它说成 CPYOS 原创 |
| kiikoread Apache-2.0 文件 | 复制/修改/再分发，保留 Apache 声明、NOTICE、修改说明 | 改标为 CPYOS/MIT；移除声明 |
| kiikoread Personal-Study 文件 | 仅在其条款下个人非商业使用；保留许可与来源 | 商用、预装/捆绑、收费刷机；与 MIT 公共核心混合后仍须遵守 |
| epdiy（LGPL） | 修改并再分发，但须提供源码/可重链接方式 | 只发二进制而不履行 LGPL |
| 厂商波形材料 | 先确认再分发许可 | 默认不要把 `my_waveform_du14.h` 纳入发布 |

**CPYOS 的选择**：Plus BSP 以厂商 MIT 工程为主参考；kiikoread 仅用于行为对照，
不复制 Personal-Study 文件；所有导入文件保留 SPDX/版权，并在
`THIRD_PARTY_NOTICES.md` 记录来源与 commit。若未来必须使用 Personal-Study 文件，
项目整体已是非商业许可，但仍需逐文件保留声明并评估条款。

## 4. 厂商固件注意事项

1. **VCOM 不能猜，而且两份参考互相矛盾。** 厂商 `main/boards/metalio-e-ink4-plus/config.h`
   写的是 `EPD_VCOM_MV 1200`；kiikoread 的 `boards/metalio47/kiiko_board_config.h`
   默认是 **3000 mV**，并注明“来自 Moku 对比测试，不是本台实测”。端口按厂商值写 **1200 mV**，
   没有做成 NVS 可配置项；两者都不是本机实测值，错误 VCOM 可能造成永久性显示损伤。
   **不要把 rpr 的 3000 mV 直接套到本机**；首测保持短时，若画面明显异常先测量再持续使用。
2. **TPS65185 上电时序**：经 TCA9555 P0.0/P0.1/P0.2/P0.7 控制；上电后等待 PWR_GOOD，
   再开始扫描。掉电/复位顺序同样重要。
3. **屏轨策略**：rpr 在每次输出后立即断电（`RAILS_IDLE_TIMEOUT_MS=0`），注释说明
   “轨常上电会让白底在强光下发灰”。不要照搬 Pico 的 8 秒空闲掉轨。
4. **波形**：Plus 使用厂家 2.0.76 完整 GL48，不做 Pico 的裁剪/文字相序调序；
   GC16/GL16 各 **48 相**、DU 20 相，帧周期约 **12 ms（≈83.3 Hz）**。
   `my_waveform_du14.h` 来源许可不明，默认不纳入发布。
5. **面板修复**：Plus 用一次 `epd_clear_checked` 白底基线，不做 Pico 的盲刷多轮。
6. **分区/启动**：厂商与 rpr 的 app 偏移不同；rpr `board.json` 的 bootloader offset 是
   **8192 (0x2000)**；厂商 `CONFIG_PARTITION_TABLE_OFFSET=0x8000`。必须选一套并固定。
7. **USB**：S31 的 USB Serial/JTAG 与 HS MSC 使用不同 PHY；开启虚拟 U 盘前先安全弹出，
   否则烧录/文件系统可能异常。
8. **SD**：4-bit SDMMC；FAT32；厂商资源在 `metalio/e-ink/`；卸载/切换 USB/待机恢复要
   先停新访问并等待事务结束。
9. **NVS/OTA**：rpr 使用独立命名空间 `metalio_s31` / `metalio_lock` / `metalio_start`；
   OTA 必须用 Plus 自己的 board tag/chip id（32），不能接受 Read Pico 的升级包。
10. **不要复用 3.97″ Metalio E-Ink 4 的 SPI 引脚表。** 厂商 README 明确警告。

## 5. CPYOS 架构选择

| 方案 | 可行性 | 代价 | 结论 |
|---|---|---|---|
| A. 继续用 PlatformIO/Arduino | **不可行**：pioarduino/Arduino 无 `esp32s31` | — | 放弃 |
| B. 独立 ESP-IDF 6.1 目标 | **可行**：IDF v6.1 支持 S31 | 需要新 HAL/构建/发布链 | **推荐** |
| C. Fork kiikoread metalio47 | 快，但代码库不同、许可混合 | 不是 CPYOS 代码；维护分叉 | 仅作行为参考 |
| D. Fork 厂商 MIT 固件 | BSP 完整，但 App 是 LVGL/AI 云 | 需要重写 CPYOS 阅读/晋江 | BSP 参考 |

**推荐架构**：新增 `ports/metalio_plus/`（ESP-IDF 6.1 工程），分层：

- `components/cpyos_bsp_metalio_plus/`：板级常量、TPS65185/TCA9555/FT6336U/SD/USB/RTC/
  IMU/前光/马达初始化（以厂商 MIT 代码为参考）。
- `components/cpyos_display_s31/`：S31 LCD/IOMUX 并口输出、E0470A14-AF 初始化、波形、
  16 级适配（epdiy LGPL 合规）；区段刷新暂时关闭，降级为整屏推。
- `components/cpyos_hal/`：把 CPYOS 的 `GfxRenderer`/`HalDisplay`/`Storage`/`HalGPIO`/
  `HalPowerManager` 接口映射到 ESP-IDF。
- `components/cpyos_core/`：纯 C++ 上层（EPUB、TXT、JJWXC 协议、字体、排版、WebBrowser）。
- `main/`：CPYOS UI/Activity 层。
- `partitions_metalio_plus.csv`、`sdkconfig.defaults`、`CMakeLists.txt`、`README.md`。

### 5.1 可复用资产：crosspoint-simulator

`0x1abin/crosspoint-simulator`（MIT，dev 树 `.pio/libdeps/simulator_readpico/simulator/`）
已经提供了一套宿主侧 HAL 和 Arduino 兼容层：`Arduino.h`、`BoardConfig.h`、
`EInkDisplay.h`、`HalDisplay`、`HalGPIO`、`HalStorage`、`HalClock`、`HalPowerManager`、
`InputManager.h` 等。`GfxRenderer.cpp` 本身几乎不直接调用 Arduino API，但它的头文件依赖
这些类型。

**移植策略**：复用它的头文件/接口作为 CPYOS 上层的编译环境，把后端逐个替换：

- `HalDisplay` 的 SDL/像素缓冲后端 → Plus epdiy（`epd_hl`）后端；
- `HalGPIO`/`InputManager` → FT6336U + TCA9555 按键；
- `HalStorage` → FATFS/POSIX；
- 时间/延时/字符串 → IDF 的 `esp_timer`/FreeRTOS/标准 C++。

不要复用 SDL 窗口、主机网络和主机文件系统实现；它们只是接口参考。

## 6. 分层移植清单

| 层 | 内容 | CPYOS 现有代码 | 移植动作 | 无真机可验证 |
|---|---|---|---|---|
| L0 | 工具链/启动/串口 | `main.cpp` | 新建 IDF 工程、board profile、日志 | 构建 + 串口启动日志（需真机） |
| L1 | 显示 | `GfxRenderer`、`EpdiyLcd`、16 级/列带 | 新 `output_lcd` 后端、面板初始化、波形 | 主机波形/几何测试；实屏需真机 |
| L2 | 输入/电源/存储 | `MappedInputManager`、`HalPowerManager`、`HalStorage` | FT6336U、TPS65185、TCA9555、4-bit SD、USB | 驱动单测/构建；实机需真机 |
| L3 | 传感器/外设 | 电池、RTC、IMU、前光、马达 | BQ27220、PCF8563、SC7A20H、LEDC、GPIO | 构建 + 日志；实机需真机 |
| L4 | 阅读/排版/字体 | `Epub`、`Section`、`TtfEpdFont`、`FreeInkUI` | 纯 C++ 可复用；HAL 替换 | 主机回归（现有测试） |
| L5 | 晋江/微信/浏览器 | `src/jjwxc`、WeRead、WebBrowser | 纯 C++/HTTP 可复用；网络层替换 | 主机测试；联网需真机 |
| L6 | OTA/发布 | `OtaUpdater`、`build_cpyos_release.py` | 新分区/board tag/manifest/校验 | 打包 + 校验；OTA 需真机 |

## 7. 分阶段计划（无真机版）

### Phase 0：工具链（进行中）
- 安装 ESP-IDF 6.1 + `esp32s31`；构建厂商 MIT 工程验证工具链。
- **Gate**：`idf.py build` 成功、产物 chip id 32。失败则停止并报告。

### Phase 1：BSP 骨架
- 新建 `ports/metalio_plus/`；实现启动、串口、TCA9555、TPS65185、SD 4-bit、FT6336U。
- **无真机验证**：编译、静态检查、主机单测；烧录/上电需真机。

### Phase 2：显示
- 接 epdiy S31 输出；初始化 E0470A14-AF；核对厂商 **48 相/约 12 ms** 波形；16 级已实现；区段刷新暂时降级为整屏推（防面板锁死），待实屏验证。
- **无真机验证**：波形表/几何/掩码主机测试；实屏需真机。

### Phase 3：HAL 适配
- 把 CPYOS 的 HAL 接口映射到 IDF；先让 UI 能渲染一页文字、触摸能翻页。
- **无真机验证**：构建 + 主机渲染测试；触摸/翻页需真机。

### Phase 4：应用层
- 阅读器、字体、晋江、浏览器、设置、电源策略；按 684×1216 逻辑尺寸适配。
- **无真机验证**：现有主机回归；联网/性能需真机。

### Phase 5：发布
- 新分区表、board tag、OTA manifest、`UNTESTED ON HARDWARE` 标注、回滚说明。
- **Gate**：构建 + 产物校验 + 主机测试；不进入稳定通道。

## 8. 风险

- **工具链**：S31 仅在 IDF 6.1；IDF 6.1 与现有 Arduino/freeink-sdk 不兼容。
- **无真机**：VCOM、波形、电源时序、触摸、SD 4-bit、USB PHY 都只能在硬件上最终确认。
- **许可**：Personal-Study 文件不能随意复制；epdiy LGPL 需要合规；厂商波形材料许可不明。
- **双栈维护**：Read Pico（Arduino）与 Plus（IDF）两套构建/发布/CI，成本显著。
- **面板损伤**：错误 VCOM/上电时序可能永久损坏屏幕；没有真机时不要提供“可直接刷”的承诺。

## 9. 来源标注规则

1. 每个导入文件保留原始 SPDX 头；没有头的补上 `SPDX-FileCopyrightText` 与来源 URL/commit。
2. 在 `THIRD_PARTY_NOTICES.md` 增加条目：项目、版本/commit、文件范围、许可证。
3. 不把 Apache-2.0 文件改标为 CPYOS；不把 Personal-Study 文件混入 MIT 公共核心。
4. 发布前跑一次 SPDX 扫描（`grep -rho "SPDX-License-Identifier: .*"`）作为门禁。
5. 厂商 MIT 代码的版权声明随文件保留；CPYOS 的修改在同一文件注明。

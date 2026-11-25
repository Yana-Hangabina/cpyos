# 脆皮鸭 CPYOS

面向 **Read Pico（小纸 Pico，RDP-G01-W）** 与 **Metalio E-INK4 Plus（无名 4.7″，ESP32-S31）** 两款墨水屏的阅读系统，基于 [CrossMux](https://github.com/0x1abin/crossmux) 开发。

官网与网页刷机：<https://yana-hangabina.github.io/cpyos/><br>
发布、完整安装包与校验和：<https://github.com/Yana-Hangabina/cpyos/releases>

## 这是什么

CPYOS 面向 Read Pico 与 Metalio E-INK4 Plus，目标是让墨水屏能舒服地读中文网络小说、本地书和网页。

它在 CrossMux 的基础上做了这些事：

- 重做界面，首页、书架、找书、我的和阅读器按墨水屏重新排版；
- 内置晋江文学城客户端和网页阅读；
- 统一中文字体、拼音输入、断词和 emoji 回退；
- 针对两块面板做局部刷新、抗锯齿和翻页效果；
- 提供设备内 OTA、网页刷机（Read Pico）与完整安装脚本（Plus）。

> **Metalio E-INK4 Plus（4.7″，ESP32-S31，1216×684 并口墨水屏）自 0.8.0-e4p 起正式支持。**
> 它与 Read Pico 共用同一套 Activity、阅读器、字体与数据包，但 SoC 与显示/电源拓扑不同，
> 固件和分区表**不可互换**。独立工程在 `ports/metalio_plus/`（ESP-IDF 6.1，`esp32s31` target），
> 源码、构建与刷写说明见 [`ports/metalio_plus/README.md`](ports/metalio_plus/README.md) 与
> [`docs/engineering/metalio-eink4-plus.md`](docs/engineering/metalio-eink4-plus.md)。
>
> **安装方式**（详见下文「安装」）：
> - 在官网刷机页选择设备后执行完整安装；Plus 会写入分区表、两个应用槽和六个数据包（含阅读字库）；
> - 也可以从发布页下载 `cpyos-plus-0.8.0-e4p.bin`、分区表与六个数据包，用仓库脚本
>   `ports/metalio_plus/scripts/flash_metalio_plus.py --layout port-ota --full-install` 手动写入；
>   不要只写 app 分区，否则会缺少语言包/emoji/阅读字库；
> - **CloudZao 固件市场只写一个 app 镜像**，只能在已完成完整安装的设备上做后续应用更新，
>   不能把原厂设备变成 CPYOS（它不写分区表和数据分区）。两条更新通道不要在同一次更新中混用。
>
> `metalio_eink4` 环境对应的是 3.97″ 的 Metalio E-Ink 4（ESP32-S3 + SSD1677 SPI），
> 与 Plus 不是同一台设备。**请勿把 Read Pico 固件刷到 Plus 上，反之亦然。**

## 相对 CrossMux 上游的新增与改进

### 界面与交互

- 首页只保留上次读到、最近阅读、近 7 天阅读和晋江更新；书库、找书、我的共用底部导航。
- 设置按分类子页组织；应用统一收进「我的 → 工具」，不再有单独的旧应用页。
- 阅读器使用单行标题栏，进度百分比放在页脚波浪条上，正文多出一行高度。
- 弹窗、等待卡、消息卡、Toast 和底部抽屉统一为同一套表面；弹窗打开时底层页面变淡，而不是压暗。
- 书籍封面使用 8-bit 灰阶缩略图，主界面之间切换不再整屏闪烁。

### 晋江文学城

- 书架、发现与榜单、书籍 / 作者 / 高级搜索（中文关键词按 GBK 发送）、收藏夹管理。
- 章节列表窗口化与跳章、章节购买（价格确认 + 6 位支付密码）、未购章占位与重试。
- 免费章节和已购买的 VIP 章节都可以缓存、离线阅读并同步进度。
- 段评与章评：读取、点赞、回复、发表段评；段评正文可离线缓存，并支持前台阅读时后台预取。
- 「作者有话说」随正文缓存；阅读进度双向同步；书架追更快照；读到末页可继续下载新章。
- 成品 EPUB 与章节缓存使用设备绑定密钥加密。

### 网页阅读

输入网址后抓取网页并提取正文，按当前阅读字号重新排版。支持页面内链接、翻页、页面菜单、书签和访问历史；中文页面自动识别 GBK / GB18030 / UTF-16，HTTPS 与明文 HTTP 都可打开。不支持 JavaScript、表单和图片。

### 中文与字体

- 一套覆盖 8,105 个通用规范汉字 + GB2312 的可缩放中文字体，界面、书名、晋江与正文共用。
- 界面字号与阅读字号分开设置（阅读 10–34pt）；生僻字可放 SD 卡完整字体。
- 离线拼音书名搜索与连续拼音输入；11 种语言断词按需加载。
- 内置部分 emoji 字形；Wi-Fi 名称、书名和正文里的常见 emoji 不再显示方框。
- 可选扩展字体包（完整 Emoji、扩展中文、多语言文字）按需下载到 SD 卡，缺字时自动提示；字体不占固件空间。

### 墨水屏显示与翻页

- 16 级文字与界面抗锯齿；列带局部刷新与局部残影清理。
- 水波纹翻页（移植自 kiikoread，按本机内存调整）与 DU 快速翻页。
- 异步推屏、前向预绘、整页扫描看门狗、状态栏时钟局部刷新、屏幕修复。
- 倾斜 / 摇晃体感翻页。

### 系统与连接

- 设备内 OTA（优先 GitHub、失败自动换镜像 + 双 app 槽升级清单）、SD 卡固件更新。
- 数据分区按需加载：拼音、断词、语言包、网页资源、emoji。
- 33 种界面语言；崩溃报告页记录重启原因与上次启动阶段。
- 可选 U 盘模式、浏览器传书与设置、WebDAV、OPDS、Calibre 无线（沿用 CrossMux）。

## 安装

预编译包覆盖 **Read Pico** 与 **Metalio E-INK4 Plus**，但两者的安装方式不同，固件与分区表不可互换。

### Read Pico（小纸 Pico）

Read Pico 没有 BOOT/RESET 按钮，第一次安装前先做 16 MiB 整片备份，并把备份放到另一台机器上：

```bash
python -m esptool --chip esp32s3 read_flash 0x0 0x1000000 readpico-backup.bin
```

然后用桌面版 Chrome 或 Edge 打开[官网刷机页](https://yana-hangabina.github.io/cpyos/#install)：先选择「小纸 Pico」，再点完整安装。

- **首次安装 / 恢复**：从出厂固件或 CrossMux 1.6.5 切换时使用，会重建分区表；
- **从 CPYOS 升级**：已经在跑 CPYOS 的设备使用，保留设置和数据分区。

0.8.0 更换了 Read Pico 的分区布局（XIP PDF 引擎 + 2.0 MiB 数据分区），**0.7.0 及更早版本必须用网页
完整安装**，不能用 OTA。设备内 OTA 在 **设置 → 系统 → 检查更新**，只写非活动的 app 槽和需要更新的
数据包，不动 SD 卡、书库和设置。

**0.2.1 及更早版本**的设备 OTA 清单在 GitHub Pages，部分网络无法访问，请先用官网网页刷机升级一次。

### Metalio E-INK4 Plus（无名 4.7″）

Plus 使用 ESP32-S31。在刷机页选择该设备后执行完整安装：

1. 先自行备份整片 16 MiB Flash（网页安装器不自动备份；手动脚本可加 `--backup plus-16m.bin`）；
2. 从发布页下载 `cpyos-plus-0.8.0-e4p.bin`（应用）、`cpyos-plus-0.8.0-e4p-partition-table.bin`
   （分区表）和六个数据包（`pinyin.dat`、`web_pack.dat`、`hyphen.dat`、`i18n_pack.dat`、
   `emoji.dat`、`cjk_core.dat`）；
3. 也可以手动执行同一安装流程（官网一键刷机失败时的备用路径）：

```bash
python3 ports/metalio_plus/scripts/flash_metalio_plus.py --layout port-ota --slot both \
  --full-install --allow-versioned --backup plus-16m.bin \
  --firmware cpyos-plus-0.8.0-e4p.bin \
  --partition-table cpyos-plus-0.8.0-e4p-partition-table.bin \
  --pinyin pinyin.dat --web web_pack.dat --hyphen hyphen.dat \
  --i18n i18n_pack.dat --emoji emoji.dat --core cjk_core.dat
```

若串口里找不到设备：先长按电源彻底断电，再上电重试。CPYOS 没有「设置 → 升级与恢复 → BOOT 刷机」入口；要切换到 kiikoread 等第三方固件，可先执行 `python -m esptool --chip esp32s31 --no-stub --before usb_reset --after no_reset chip-id` 让设备停在 ROM 下载模式，再打开对方的刷机页。第三方刷机失败后设备可能停在下载/Stub 模式，屏幕保持最后一帧且触摸无效，长按电源断电再上电即可。详细参数与恢复方法见 [`ports/metalio_plus/README.md`](ports/metalio_plus/README.md)。

**CloudZao 固件市场**（搜索「脆皮鸭CPYOS」）只能写一个 app 镜像（`0x10000`），
**仅适用于已经完成完整安装的设备**做后续应用更新；它不会写分区表、bootloader 或数据分区，
因此不能让原厂设备变成 CPYOS，也不能用来补齐缺失的语言包/阅读字库。设备内「系统更新」写的是
非活动槽并切换 otadata，与市场更新是两条通道，**不要在同一次更新里混用**。

### 数据包与校验

设备 OTA 会同时更新 app 与数据包；只有分区表或包格式发生破坏性变化时，才需要完整安装或网页刷机。
完整包的偏移量、校验和与手动命令见包内 `FLASH.md` 和 `SHA256SUMS`；Plus 另见
`ports/metalio_plus/README.md`。

## 从源码构建

需要 PlatformIO Core、Python 3 和 Git。`freeink-sdk` 是固定提交的子模块，显示相关补丁放在 `patches/`：

```bash
git clone --recursive https://github.com/Yana-Hangabina/cpyos.git
cd cpyos
git submodule update --init --recursive

bash patches/freeink-sdk/apply.sh
python3 scripts/patch_pioarduino_cache.py --prepare-platform

# 构建 Read Pico
pio run -e readpico

# 生成数据分区内容
python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/ --strip-unused
python3 scripts/build_web_pack.py
python3 scripts/generate_hyphenation_db.py
python3 scripts/generate_pinyin_db.py /path/to/pypinyin-0.55.0-py2.py3-none-any.whl
python3 scripts/generate_data_manifest.py
```

构建成功的判据是日志里出现 `[SUCCESS]`，并且镜像的 `esp_app_desc.project_name` 是 `crossmux` 或 `cpyos`（不是 `arduino-lib-builder`）。

桌面模拟器：

```bash
pio run -e simulator_readpico -t run_simulator
```

## 参与开发

- 默认分支是 `cpyos`；`main` 只镜像上游，不放 CPYOS 提交。
- 功能改动请从 `cpyos` 切分支，构建通过后再合并。
- 不要提交账号 / Cookie / 抓包正文或设备截图。
- 问题反馈请附设备型号、CPYOS 版本、复现步骤和脱敏后的串口日志。

同步上游：

```bash
git remote add upstream https://github.com/0x1abin/crossmux.git
git fetch upstream
git switch main
git merge --ff-only upstream/main
git switch cpyos
git merge main
```

## 致谢

- **CrossMux**、**CrossPoint Reader**、**Inx**、**cpr-vcodex**、**diy-esp32-epub-reader**：上游固件、界面与 EPUB 阅读的早期探索。
- **kiikoread（read-pico-reader）**：水波纹翻页、TEXTTURN 提前回浅、gamma 覆盖表、离线拼音搜索与输入、摇晃翻页判定。
- Noto Sans CJK / Source Han Sans、FreeType、pypinyin、Lucide 与断词词典作者。

## 许可证

CPYOS 原创代码与官方固件采用 [CPYOS Project License 1.0](./LICENSE)：源码可见、仅限个人非商业使用、禁止商用；晋江组件源码不公开，官方固件可包含其二进制。第三方组件保留各自许可（MIT / Apache-2.0 / LGPL-3.0 / OFL / AGPL 等），详见 [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md)。上游 MIT 代码（CrossMux / CrossPoint 等）仍可按其原始 MIT 许可使用；本许可不改变第三方组件的原有授权。晋江支持声明见 [NOTICE-JJWXC.md](./NOTICE-JJWXC.md)。

此前已按 MIT 发布的版本仍按其发布时的许可；本许可适用于 2026-11-06 及之后发布的版本。

## 免责声明

这是**非官方固件**，与 MindReset、晋江文学城、微信读书均无隶属关系。晋江与微信读书使用的是非公开接口，可能随时变化。设备流量经过 TLS，但客户端不校验服务器证书与主机名，请只在可信网络中使用。刷机有风险，第一次安装前务必保留整片备份。

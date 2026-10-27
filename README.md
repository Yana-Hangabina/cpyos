# 脆皮鸭 CPYOS

[English](./README.en.md) | **简体中文**

**脆皮鸭 CPYOS** 是面向 **Read Pico（小纸 Pico，RDP-G01-W）** 墨水屏的原生阅读系统。它基于 [CrossMux](https://github.com/0x1abin/crossmux) 上游，把**新版界面 / refreshed UI**、**晋江文学城**、**微信读书**、中文输入、水波纹翻页和设备 OTA 做成一台真正能日常使用的阅读器。

> 官网与网页刷机：[yana-hangabina.github.io/cpyos](https://yana-hangabina.github.io/cpyos/)<br>
> 发布与完整安装包：[Releases](https://github.com/Yana-Hangabina/cpyos/releases)

## 我们新增、主打的功能

### 新版界面 / Refreshed UI
首页只保留上次读到、最近阅读、近 7 天阅读和晋江更新；书架、找书、我的共用一条水线与底部导航。设置从「我的」进入分类子页，返回路径始终回到「我的」，不再跳到旧的设置界面。找书页把微信读书、晋江、本地书、OPDS 的登录与书架入口直接摊开，每个来源有自己的会话。

### 晋江文学城原生客户端
- 书架翻页、书籍详情、章节列表窗口化与跳章、书籍 / 作者 / 高级搜索、发现与 14 个榜单来源、收藏夹管理。
- 免费章走 App API 明文；VIP 章走 WAP 两段 AES、CSS 位移还原、PUA 字体水印反解，逐章取字体，不缓存跨章字体。
- 章节购买、价格确认、6 位支付密码（可选记住，设备密钥 AES-256-GCM 封装）、未购章占位、重试未购章。
- 段评计数、正文离线缓存、点赞、回复、发表段评；所有写操作只在用户点按时发生。
- 阅读进度双向同步、书架追更快照；读到末页可继续下载新章。
- 成品 EPUB 与章节正文使用设备绑定密钥加密；阅读器退出时清理明文派生缓存。

### 微信读书
扫码登录、书架浏览、EPUB 下载与离线阅读、阅读进度同步。微信读书、晋江和本地书共用书架，并显示来源徽标。

### 中文阅读
- 离线拼音书名搜索与连续拼音输入（349 KB 数据表，随数据分区刷入）。
- 11 种语言断词自动机，按需从数据分区加载。
- GB2312 常用字 12pt 兜底 + 14/16pt UI 字面；生僻字可放 SD 卡完整字体。
- SD 卡 TTF / OTF / TTC；EPUB 内嵌字体；带自有字体的书自动使用书内字体。

### 墨水屏显示与翻页
- 水波纹翻页（移植自 kiikoread，按 Read Pico 内部内存改为 8 KiB 热窗）。
- 黑白快速翻页、异步推屏、前向预绘。
- 状态栏时钟每分钟局部刷新；全刷屏与屏幕修复。
- gamma 覆盖 LUT、TEXTTURN 白→中灰提前回浅（均移植自 kiikoread）。
- 倾斜 / 摇晃体感翻页，阈值移植自 kiikoread。

### 阅读器与本地书库
EPUB、TXT、CBZ、FB2，实验性 PDF；书签、批注 / 笔记、词典、阅读统计与热力图、待机表盘、最近阅读、续读卡。支持 KOReader 进度同步。

### 系统与连接
设备 OTA、SD 卡固件更新、浏览器传书与设置、WebDAV、OPDS、Calibre 无线、可选的 USB 大容量存储；33 种 UI 语言按需加载；崩溃恢复页保护上次打开的书。

## 设备与发布

| 项目 | 说明 |
|---|---|
| 目标设备 | Read Pico（小纸 Pico，RDP-G01-W），ESP32-S3，684×1216 16 灰阶 |
| 预编译包 | **只提供 Read Pico**；上游其它目标仍可从源码构建 |
| 当前版本 | **0.1.0**（CPYOS 独立版本号，从 0.1.0 开始，不接续 CrossMux 1.x） |
| 安装方式 | 浏览器 Web Serial 刷机、完整安装包、设备 OTA、SD 卡固件更新 |
| 分区 | app0 / app1 各 7.19 MiB，数据分区 1.5 MiB（拼音 / 断词 / 语言包 / Web 资源） |

### 第一次安装

1. **先备份整片 Flash（16 MiB）**，并把备份放到另一台机器上：
   ```bash
   python -m esptool --chip esp32s3 read_flash 0x0 0x1000000 readpico-backup.bin
   ```
2. 用桌面版 Chrome 或 Edge 打开 [官网刷机页](https://yana-hangabina.github.io/cpyos/#install)，点「首次安装 / 恢复」。
3. 写入完成后设备自动重启。首次安装会重建分区表并擦除芯片；SD 卡里的书和 `/.crosspoint` 设置不会被网页刷机删除。

从已经运行 CPYOS 的设备升级时用「从 CPYOS 升级」，保留 NVS 与分区表。完整包的偏移量、校验和与手动 `esptool` 命令见包内 `FLASH.md` 与 `SHA256SUMS`。

### 设备内 OTA

已安装 CPYOS 0.1.0 或更新版本后：**设置 → 系统 → 检查更新**。OTA 只写 app0，不动 SD 卡、书库和设置。固件从项目官网的静态 OTA 清单读取更新：

```
https://yana-hangabina.github.io/cpyos/ota/readpico.json
```

首次从 CrossMux 1.6.5 或出厂固件切换请使用完整安装包，因为两边的分区表不同。

## 版本与发布

版本号由 `platformio.ini` 的 `[crosspoint] version` 定义，Git tag 必须与它完全一致（例如 `0.1.0`，不带 `v`）。发布工作流会：

1. 构建 `readpico` 环境，校验 `esp_app_desc.project_name` 是 `crossmux` 或 `cpyos`（不是 `arduino-lib-builder`）、板级 tag、芯片、app0 槽大小；
2. 生成拼音 / 断词 / 语言包 / Web 资源包，并生成 `build/data_manifest.txt`；
3. 产出完整安装包、升级包、应用固件、Web 刷机 manifest、OTA 清单和 `SHA256SUMS`；
4. 创建 GitHub Release，并部署 GitHub Pages（官网 + 网页刷机 + OTA 清单）。

维护者步骤见 [docs/cpyos/release.md](docs/cpyos/release.md)，用户更新说明见 [docs/cpyos/install.md](docs/cpyos/install.md)，版本历史见 [CHANGELOG.md](CHANGELOG.md)。

## 从源码构建

需要 PlatformIO Core（或 `pioarduino`）、Python 3、Git。Read Pico 的显示补丁放在 `patches/`，因为 `freeink-sdk` 是固定提交的 gitlink：

```bash
git clone --recursive https://github.com/Yana-Hangabina/cpyos.git
cd cpyos
git submodule update --init --recursive

# freeink-sdk 的两个本地补丁（显示 / 水波纹 / 字体 CFF）
bash patches/freeink-sdk/apply.sh
bash patches/freeink-sdk-cff/apply.sh

# pioarduino 平台缓存准备
python3 scripts/patch_pioarduino_cache.py --prepare-platform

# 构建 Read Pico（数据包外置）
pio run -e readpico

# 生成数据分区内容
python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/ --strip-unused
python3 scripts/build_web_pack.py
python3 scripts/generate_hyphenation_db.py
python3 scripts/generate_pinyin_db.py /path/to/pypinyin-0.55.0-py2.py3-none-any.whl
python3 scripts/generate_data_manifest.py

# 组装发布包（不发布也能生成）
python3 scripts/build_cpyos_release.py --version 0.1.0
```

产物在 `dist/cpyos/0.1.0/`。构建判据不要只看退出码：日志里要有 `[SUCCESS]`，镜像的 `esp_app_desc.project_name` 必须是 `crossmux` 或 `cpyos`（不能是 `arduino-lib-builder`）。桌面模拟器：

```bash
pio run -e simulator_readpico -t run_simulator
```

## 与 CrossMux 上游同步

本仓库是 [`0x1abin/crossmux`](https://github.com/0x1abin/crossmux) 的 fork。

| 分支 | 用途 |
|---|---|
| `main` | 上游镜像，不做 CPYOS 改动 |
| `cpyos` | **默认分支**，CPYOS 发布分支 |

同步上游：

```bash
git remote add upstream https://github.com/0x1abin/crossmux.git
git fetch upstream
git switch main
git merge --ff-only upstream/main
git switch cpyos
git merge main
```

也可以在 GitHub 上对 `main` 使用「Sync fork」，再把 `main` 合进 `cpyos`。发布始终从 `cpyos` 打 tag。

## 致谢

- **CrossMux**、**CrossPoint Reader**、**Inx**、**cpr-vcodex**、**diy-esp32-epub-reader**：上游固件、界面与 EPUB 阅读的早期探索。
- **[kiikoread / read-pico-reader](https://github.com/wegooo-cell/read-pico-reader)（kiikoread，Apache-2.0）**：水波纹翻页引擎、TEXTTURN 白→中灰提前回浅、gamma 覆盖 LUT、离线拼音搜索与连续拼音输入、摇晃翻页阈值与判定。移植差异记录在 `patches/freeink-sdk/` 与相关源码注释中。
- **MuPDF**（AGPL-3.0，实验性 PDF）、**FreeType**、**Noto Sans CJK / Source Han Sans**（OFL）、**pypinyin**（MIT）、**Lucide**（ISC）与断词词典作者。
- refreshed UI 的设计语言与本文档站点的排版。

完整第三方清单见 [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md)。

## 安全与免责声明

- 本项目是**非官方固件**，与 MindReset、晋江文学城、微信读书均无隶属关系。
- 晋江与微信读书使用的是非公开接口，可能随时变化。设备侧流量经过 TLS，但客户端不验证服务器证书与主机名；请只在可信网络中使用。
- 仓库不包含任何账号、密码、token、Cookie 或抓包正文。登录凭据只保存在设备上，并使用设备密钥加密或混淆。
- 刷机有风险。第一次安装前必须完成整片备份；不要根据本文档手抄偏移量，使用发布包里的 `FLASH.md` 与 `manifest-*.json`。

## 参与贡献

见 [CONTRIBUTING.md](./CONTRIBUTING.md)。问题反馈请附设备型号、CPYOS 版本、复现步骤与脱敏后的串口日志；不要把账号、Cookie 或未脱敏日志贴到 Issue。

## 许可证

本项目代码以 [MIT](./LICENSE) 发布。第三方组件保留各自许可证；MuPDF 相关实验构建按 AGPL-3.0 分发。见 [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md)。

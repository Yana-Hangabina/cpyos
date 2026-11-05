# 安装与更新 / Install and update

本文只面向 **Read Pico（小纸 Pico，RDP-G01-W）**。CPYOS 的预编译包不适用于 Xteink、Sticky、Metalio 或其它 CrossMux 目标。

## 0. 刷机前：整片备份

Read Pico 没有 BOOT / RESET 按钮，第一次安装前必须做一次 16 MiB 整片备份：

```bash
python -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX \
  read_flash 0x0 0x1000000 readpico-backup.bin
```

把备份复制到另一台机器。恢复时：

```bash
python -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX \
  write_flash 0x0 readpico-backup.bin
```

> VCOM 固化在 PMU 中，不在 Flash；备份 / 刷写 Flash 不会碰它。不要写 PMU 或修改 VCOM。

## 1. 网页刷机（推荐）

要求：

- 桌面版 Chrome 或 Edge（Web Serial），页面必须通过 `https://` 打开；
- USB-C **数据线**；设备应显示为 `USB JTAG/serial debug unit`；
- 设备电量充足，刷写过程中不要拔线。

打开 <https://yana-hangabina.github.io/cpyos/#install>：

| 按钮 | 适用场景 | 行为 |
|---|---|---|
| **首次安装 / 恢复** | 出厂固件、CrossMux 1.6.5、分区表不是 CPYOS 布局 | 写 bootloader、分区表、boot_app0、app 和五个数据包；要求擦除芯片 |
| **从 CPYOS 升级** | 已经在运行 CPYOS 0.1.0 或更新版本 | 只写 app 和五个数据包；保留 NVS 与分区表 |

首次安装会重建分区表（7.19 MiB app 槽 + 1.5 MiB 数据分区）。网页刷机不会删除 SD 卡里的书和 `/.crosspoint` 设置，但会擦除芯片上的 NVS（Wi-Fi 凭据等）。

## 2. 完整安装包

发布页提供 `cpyos-readpico-<version>-full.zip`，解压后包含：

| 文件 | Flash 地址 |
|---|---|
| `readpico-bootloader.bin` | `0x0` |
| `readpico-partitions.bin` | `0x8000` |
| `readpico-boot_app0.bin` | `0xE000` |
| `readpico-firmware.bin` | `0x10000` |
| `readpico-app1.bin` | `0x740000`（升级/仅 app 清单会写） |
| `pinyin.dat` | `0xE70000` |
| `web_pack.dat` | `0xEC6000` |
| `hyphen.dat` | `0xF00000` |
| `i18n_pack.dat` | `0xF70000` |
| `emoji.dat` | `0xFD0000` |

偏移量以包内 `FLASH.md` 和 `build/data_manifest.txt` 为准；分区表或数据包布局变化时本文档会过时。校验：

```bash
sha256sum --check SHA256SUMS
```

手动刷写（确认端口与备份之后）：

```bash
python -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX --baud 921600 \
  write_flash \
  0x0 readpico-bootloader.bin 0x8000 readpico-partitions.bin \
  0xE000 readpico-boot_app0.bin 0x10000 readpico-firmware.bin \
  0x740000 readpico-app1.bin \
  0xE70000 pinyin.dat 0xEC6000 web_pack.dat \
  0xF00000 hyphen.dat 0xF70000 i18n_pack.dat \
  0xFD0000 emoji.dat
```

★ **升级时两个 app 槽都要写**：设备做过 OTA 后 otadata 会选中 app1，而网页刷机无法读取 otadata 判断活动槽；只写 app0 会“显示成功但设备仍从 app1 启动旧固件”。`manifest-upgrade.json` / `manifest-app.json` 已经同时写 app0 与 app1，手动刷写也要照做。首次安装由 `boot_app0.bin` 选中 app0，不需要 app1。

成功判据是日志里的 **`Hash of data verified.`**，不是退出码。

## 3. 设备内 OTA

★ **0.2.1 及更早版本请使用官网网页刷机升级到 0.6.2 或更新版本**，不要依赖设备内 OTA：这些版本的清单仍在 GitHub Pages，部分网络无法访问，设备会一直显示检查更新失败或极慢。网页刷机「从 CPYOS 升级」/完整包会同时写 app0、app1 与五个数据包；升级一次后即可正常使用设备 OTA。

已安装 CPYOS 0.2.2 或更新版本后：

1. 连接 Wi-Fi；
2. **设置 → 系统 → 检查更新**；
3. 确认升级，等待下载、校验和重启；不要断电。

OTA 会写非活动的 app 槽，并更新数据分区里的数据包。设备先读取同目录的 `readpico-packs.txt`，只下载与当前分区校验和不同的包；每个包先下载到 SD 临时文件并校验 SHA-256，通过后再擦写，写入后回读校验。下载失败时 app 仍会更新，新版会继续使用旧包并在首页提示重试；写入失败时不会切换到新 app。只有分区表或包格式发生破坏性变化时，才需要完整安装或网页刷机。

固件读取：

```
https://gh-proxy.com/https://github.com/Yana-Hangabina/cpyos/releases/latest/download/readpico-ota.json
```

清单里的 `tag_name` 是语义版本（`0.6.2`），固件按前三段比较。首次从 CrossMux 1.6.5 切换必须使用完整安装包，因为分区表不同；仅刷 app 会让固件在错误的分区基址上找数据包，表现为语言包 / 拼音 / 断词静默失效。

## 4. 数据分区与 SD 卡

数据分区 `spiffs` 当前 1.5 MiB，五个 blob 的偏移由 `scripts/generate_data_manifest.py` 从分区表和 C++ 加载器常量生成：

- `pinyin.dat`：拼音输入与书名搜索；
- `hyphen.dat`：11 种语言断词；
- `i18n_pack.dat`：33 种 UI 语言；
- `web_pack.dat`：文件传输 / 设置网页；
- `emoji.dat`：Noto Emoji 单色子集，正文与段评的 emoji 回退。

SD 卡根目录的同名文件优先于数据分区（`/pinyin.dat`、`/hyphen.dat`），这是免刷写更新数据的通道。数据分区缺失时固件仍能启动，但对应功能会降级：非英文语言回退英文，网页显示恢复页。

## 5. 恢复

- **能进系统**：设置 → 系统 → SD 卡固件更新，或网页刷机「从 CPYOS 升级」。
- **不能进系统**：用整片备份写回 `0x0`，或网页刷机「首次安装 / 恢复」。
- **串口没有出现**：先换数据线，再检查系统驱动 / 浏览器权限；Read Pico 使用原生 USB-Serial/JTAG，复位后端口号会变化。

## 6. 已知注意事项

- 首次安装后 Wi-Fi 需要重新连接。
- 数据分区布局与上游 CrossMux 1.6.5 不同；不要混用两边的分区表或 `firmware.bin`。
- 本固件是非官方项目；刷机风险自担。

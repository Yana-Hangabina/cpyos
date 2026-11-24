# Read Pico 失联恢复（本机型没有 BOOT/RESET 键）

## 症状分级

| 症状 | 含义 | 处理 |
|---|---|---|
| USB 仍枚举为 `USB JTAG/serial debug unit`（`ioreg -p IOUSB` 可见，序列号正常），但 **esptool 报 `Failed to connect … No serial data received`**，串口读 8 s **0 行** | ROM 在，但既不进下载模式也不跑固件（通常是一次刷写被中断/挤掉之后残留的状态） | **断电**：拔插 USB（等 ≥2 s），或长按电源键关机再短按开机。本机型唯一手段。 |
| esptool 能连、串口有 `Starting CrossPoint version …` | 固件在跑，只是没在动 | 正常；用 `CMD:HOME` / `CMD:NET` 确认响应 |
| 串口有输出但反复重启（`Rebooting`/`Guru`） | 固件崩溃循环 | 读最后一段日志；必要时用设备内 SD 固件更新或整片备份回滚 |
| USB 不枚举 | 更底层（供电/线/PMU） | 换线换口；长按电源关机再开 |

## 为什么会进这个状态（2026-11-21 实测）

一次刷写**在写 flash 期间**被另一个进程打开同一串口挤掉，esptool 中断，随后设备就落到
“USB 在、ROM 不答、固件不跑”。**不是砖**：断电即恢复，app/分区表都没坏。

判据与预防（已工程化）：
- 刷写成败**只数日志里的 `Hash of data verified.`（app0+app1+pdfmod = 3）**，不看退出码；
- 任何串口工具（`pdf_shot`、矩阵、复现脚本）在 `esptool` 在跑时**拒绝开串口**；链条启动时也拒绝与它们并发。

## 恢复后第一件事

```bash
bash exp/pdf_verify_round.sh      # 数满 3 个 Hash → repro → 两张卡 → 完整矩阵
```
若设备上的镜像与当前源码不同，这一步会先刷三镜像再验证，不需要手工拼 esptool 参数。

## 不要做的事

- 不要 `erase_flash`，不要写 bootloader / 分区表 / otadata / nvs；
- 不要在刷写还没结束（日志里还没出现 3 个 `Hash of data verified.`）时打开任何串口工具；
- 不要把“进程退出”当作“刷写完成”（父 shell 退出 ≠ esptool 退出）。

## USB 枚举 ≠ CPU 活着（2026-11-21 实测）

中断的刷写之后，设备在 macOS 上仍然以 `USB JTAG/serial debug unit`（Espressif）出现，
`/dev/cu.usbmodem101` 存在、`ioreg` 显示 `registered, matched, active`——但**所有**与 CPU 的交互都失败：

- `esptool chip_id`（默认 `usb_reset`）：`No serial data received`，多次、隔小时各试一遍；
- 手工 DTR/RTS 三种组合（`0/1`、`1/0`、`1/1`）再 `chip_id`：同样无 SYNC；
- 串口 0 行输出（115200），设备端不打印任何东西。

结论：**USB-Serial/JTAG 外设的枚举是硬件行为，不证明 ROM 在服务它**。本机无 BOOT/RESET 键，
所以唯一恢复手段是**物理断电**（拔插 USB 等 ≥2 秒，或长按电源关机再开机）。
不要再花时间尝试"软件复位/进下载模式"的变体——这里已经逐一试过。


## `Cache error` 崩溃后的状态（2026-11-22）

深度关射频（`WIFI_OFF` + `esp_wifi_deinit`）在“射频已连 → 打开 PDF”路径上触发了
`Guru Meditation Error: Core 0 panic'ed (Cache error)`，随后设备与之前那种中断刷写后的状态相同：
**USB 仍枚举、串口无响应、`esptool chip_id` 无 SYNC**，只能物理断电。
把拆除推迟到内部栈执行并没有消除它 ⇒ 结论是**这条拆除本身不该在阅读器进入路径上做**；
阅读器已回退到 `releaseRadio()`（只断关联）。`releaseRadioDeep()` 仍保留给台架（`CMD:WIFIDEINIT`）。

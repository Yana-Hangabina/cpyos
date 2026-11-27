# 脆皮鸭 CPYOS Plus 0.8.5

Metalio E-INK4 Plus `0.8.5-e4p`

固件与 0.8.4 相同。本版补上 Plus 发布材料里一直缺失的**分区表**
（`cpyos-plus-0.8.5-e4p-partition-table.bin`）：官网网页一键安装依赖它生成
`flash-plus.json`，手动完整安装也要用它——从 0.8.1 起它就没有随版本发布，安装页因此一直不可用。

- 官网「选择 Metalio E-INK4 Plus」现在可以一次写入分区表、两个应用槽与六个数据包。
- 手动路径（`flash_metalio_plus.py --layout port-ota --full-install`）可直接使用发布页的分区表。
- 新增门禁：发布了 Plus 固件却没有分区表或数据清单时，站点构建直接失败（不再静默跳过）。
- 含 0.8.4 修复：Plus 数据包以 `plus-` 前缀发布（`i18n` / `web` 两目标字节不同，曾互相覆盖）。

已完整安装的设备照常使用 CloudZao 更新；数据包未变，无需重刷。

# 脆皮鸭 CPYOS Plus 0.8.4

Metalio E-INK4 Plus `0.8.4-e4p`

固件行为与 0.8.3 相同。本次只修**发布资产命名**：0.8.3 的 Plus 与 Read Pico 作业把数据包
上传到同一个 Release、用同一个文件名，其中 `i18n_pack.dat` 与 `web_pack.dat` 两目标的字节
不同（大小相同），后上传的覆盖先上传的 —— Release 上是 Read Pico 的 i18n + Plus 的 web。

- **从现在起 Plus 的每个数据包都以 `plus-` 前缀发布**（`plus-i18n_pack.dat`、
  `plus-web_pack.dat`、`plus-pinyin.dat`、`plus-hyphen.dat`、`plus-emoji.dat`、
  `plus-cjk_core_*.dat`），两份发布不再共用一个命名空间。
- `data_manifest_plus.txt` 随发布包一起更新，指向这些名字；`flash_plus_data.py` 会在
  manifest 旁边找数据包，所以下载整个 `cpyos-plus-*` 资产集合即可
  `python3 flash_plus_data.py --manifest data_manifest_plus.txt`。
- 打包前会逐个数据包与 manifest 的 SHA-256 比对，不一致直接让发布失败。

**如果你在 0.8.3 之后手工刷过数据分区**：语言可能已经回退英文（语言包来自 Read Pico），
或网页落到了恢复页。用本版的数据包（或网页刷机）重刷一次数据分区即可恢复；书籍、设置、
阅读进度都不受影响。CloudZao 的应用更新只写 app 槽，不会补数据分区。

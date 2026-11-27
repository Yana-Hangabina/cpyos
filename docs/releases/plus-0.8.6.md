# 脆皮鸭 CPYOS Plus 0.8.6

Metalio E-INK4 Plus `0.8.6-e4p`

与 Read Pico 同源的稳定性版本。Plus 侧的重点：

- **首次具备崩溃可见性**：C++ 失败钩子、开机故障环、activity/stage/heap 记录；panic/看门狗/欠压重启
  会进入共享崩溃屏并写 `crash_report.txt`。
- **字体策略与 Read Pico 相同**：网络回收配对归还、角色看门狗（1s→60s 退避）、部分建成可重试；
  Plus 此前缺失的"归还"环节已补上。
- 设置页单表构建、活动创建 no-throw、PDF 文本缓冲降级、设置写失败弹窗，均与 Read Pico 一致。
- Wi-Fi 失败文案分类（4G 失败也按原因显示），不再只显示「通用故障」。

数据分区：语言键 1885，请与固件一并更新（网页刷机或完整安装）。

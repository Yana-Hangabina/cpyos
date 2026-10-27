# 安全说明 / Security notes

## 仓库里没有什么

本仓库**不包含**账号、密码、token、Cookie、设备验证码、抓包正文或任何受版权保护的小说内容。测试与文档只使用合成向量或已脱敏的页面结构。

如果你在提交里发现了这类数据，请不要在 Issue 中粘贴内容，直接通过 GitHub Security Advisory 私下报告。

## 设备上保存了什么

| 数据 | 位置 | 保护方式 |
|---|---|---|
| Wi-Fi 凭据 | NVS | 设备本地 |
| 晋江 Cookie / 会话 | `/.crosspoint/jjwxc/cookies` | 设备 MAC 异或 + Base64（**混淆，不是加密**） |
| 晋江设备指纹 | `/.crosspoint/jjwxc/device` | 同上；退出登录不会删除 |
| 晋江支付密码（可选记住） | `/.crosspoint/jjwxc/paykey` | 设备 secret 派生的 AES-256-GCM |
| 成品 EPUB 内容密钥 | 书旁的 `.key` | 设备 secret 派生；`bookkey` |
| 章节正文缓存 | `/.crosspoint/jjwxc/<id>/chapters/*.xhtml` | `JJX1`，AES-128-CTR + 设备 MAC / 盐派生 |
| 阅读器分页派生缓存 | `/.crosspoint/epub_*` | 明文；退出晋江阅读器时删除 |

设备 MAC 是公开硬件信息，因此 cookie 的异或混淆只防止“打开文件直接读”，不防有意的本地攻击者。这是 CrossMux 既有设计，CPYOS 沿用并明确记录。

## 传输

- 晋江与微信读书使用非公开接口，可能随时变化。
- 设备侧 TLS 由 wolfSSL 承载，但现有客户端**不验证服务器证书与主机名**。请只在可信 Wi-Fi 下登录和下载。
- 晋江 WAP 登录响应可能回显账号与密码明文；固件**不得**把登录响应体写入日志、SD 卡或崩溃转储。
- 不要为了开发反复登录：每次失败都可能增加账号风控计数。需要会话时从设备导出，不要在主机上重新登录。

## 日志

分享串口日志前请删除：

- `loginName=` / `loginPassword=` 等登录 URL 参数；
- `sid=`、`Cookie:`、`Authorization:`；
- 任何 8 位 reader id + 下划线 + 长串的会话值。

仓库的 `exp/redact_log.py`（在开发仓库中）提供了脱敏与自检；本仓库不附带真机日志。

## 刷机

- 只写 app0 / 完整安装包定义的分区；**不要** `erase_flash`。
- 数据分区的 `0xEF0000` 起有 12 个出厂字节，用途不明，任何脚本都不得覆盖；`generate_data_manifest.py` 会拒绝这样的布局。
- 第一次安装前做整片 16 MiB 备份。

## 报告问题

使用 GitHub 的 **Security → Report a vulnerability**，不要在公开 Issue 中贴账号、Cookie 或未脱敏日志。

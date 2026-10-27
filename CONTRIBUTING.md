# 参与贡献 / Contributing

感谢你愿意帮脆皮鸭 CPYOS 做得更好。这个项目以 Read Pico 真机行为为准：能构建、能通过主机测试，不等于在 684×1216 的墨水屏上成立。

## 报告问题

请附上：

- 设备型号（Read Pico / RDP-G01-W）与 CPYOS 版本（设置 → 关于本机）；
- 复现步骤，尽量缩到最少；
- 串口日志（**先脱敏**：删除 `loginName=`、`loginPassword=`、`sid=`、`Cookie:`、`Authorization:` 与任何账号数据）；
- 如果是显示问题，先描述区域和现象；**不要上传整屏截图/帧缓冲**。如必须附图，请先遮挡书名、作者、WiFi 名称、账号等私人信息，并且不要把图片提交进仓库。

安全或账号相关问题请用 GitHub 的 **Security → Report a vulnerability**，不要公开粘贴凭据或未脱敏日志。

## 提交代码

1. 从 `cpyos` 分支切出功能分支，PR 目标也是 `cpyos`（不是上游 `main`）：

   ```bash
   git fetch fork --prune
   git switch cpyos
   git merge --ff-only fork/cpyos
   git switch -c work/<topic>
   # 修改、构建、测试、提交
   git push fork work/<topic>
   ```

   维护者把功能分支合回 `cpyos`（`git merge --no-ff work/<topic>`）后再推送 `fork/cpyos`；不要直接改 `main` 或往 `origin` 推。

2. 保持改动聚焦：一个 PR 解决一件事，不要顺手重构无关文件。
3. 不提交账号、密码、token、Cookie、抓包正文或真机日志。
4. 改了 i18n YAML 后重跑 `python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/ --strip-unused`，并说明语言包键数变化（数据分区需要一起刷新）。
5. 改了 `freeink-sdk` 内的文件时，把改动同步进 `patches/freeink-sdk/` 或 `patches/freeink-sdk-cff/`；gitlink 本身不会携带工作树修改。

## 本地检查

```bash
# 固件（gg / 代理会吞掉退出码，只看日志里的关键字）
pio run -e readpico > /tmp/cpyos-build.log 2>&1
grep -E '\[(SUCCESS|FAILED)\]' /tmp/cpyos-build.log | tail -1

# 镜像、板级 tag、app0 槽与数据包
python3 scripts/verify_release_artifacts.py

# 发布包组装（可选）
python3 scripts/build_cpyos_release.py --version 0.1.0
```

构建成败看日志里的 `[SUCCESS]` / `[FAILED]`，并确认镜像的 `esp_app_desc.project_name` 是 `crossmux` 或 `cpyos`（不是 `arduino-lib-builder`）。只看到退出码 0 不能证明 `custom_sdkconfig` 生效。

主机测试集中在 `src/`（晋江协议、WAP 解密、拼音、水波纹、批注、字体清单等）与 `scripts/tests/`（发布、图标、设置映射）。新增协议行为时请优先加离线合成向量，不要依赖真实账号。

## 文档

- 安装 / OTA：[docs/cpyos/install.md](docs/cpyos/install.md)
- 发布：[docs/cpyos/release.md](docs/cpyos/release.md)
- 上游同步：[docs/cpyos/upstream-sync.md](docs/cpyos/upstream-sync.md)
- 安全：[docs/cpyos/security.md](docs/cpyos/security.md)

## 许可

提交即表示你同意以本仓库的 MIT 许可证发布你的改动；从 kiikoread 等 Apache-2.0 项目移植的代码必须保留来源与许可证说明。

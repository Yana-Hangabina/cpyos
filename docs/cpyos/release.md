# 发布维护 / Release handbook

## 版本号

- 版本号唯一来源是 `platformio.ini` 的 `[crosspoint] version`。
- Git tag 使用 `cpyos-<version>`：例如版本 `0.1.1` 的 tag 是 `cpyos-0.1.1`。**上游 CrossMux 已占用裸 `0.x` tag**（`0.1.1` … `0.16.0` 是 2025 年的旧提交），所以 CPYOS 不再使用裸版本号做 tag；`platformio.ini`、OTA 清单和发布说明里的版本仍是 `0.1.1`。
- 0.1.0 是唯一使用裸 tag 的历史版本（当时上游没有 `0.1.0` tag）；不要移动它。
- 使用语义版本：`0.1.0` → `0.1.1`（修复）/ `0.2.0`（功能）/ `1.0.0`（不兼容）。OTA 只比较前三段。
- CPYOS 从 `0.1.0` 开始独立计数，不再接续 CrossMux 的 `1.x`。首次安装用完整包；从 `0.1.0` 起的后续版本才通过 OTA 按 CPYOS 版本比较。

## 日常分支与合并

`main` 是上游镜像，`cpyos` 是默认、集成和发版分支。功能改动不要直接提交到 `main`：

```bash
git fetch fork --prune
git switch cpyos
git merge --ff-only fork/cpyos
git switch -c work/<topic>

# 修改、构建、测试、提交
pio run -e readpico
python3 scripts/generate_data_manifest.py
python3 scripts/verify_release_artifacts.py

git switch cpyos
git merge --no-ff work/<topic>
git push fork cpyos
```

- 只推 `fork`（`Yana-Hangabina/cpyos`）；`origin`/`upstream` 是只读的 CrossMux。
- 多个人/agent 并行时，各自用 `work/<topic>`，不要改写别人的分支。
- 公开树不放设备截图、书名、WiFi、账号；`refreshed UI` 是界面名称，`kiikoread` 是致谢名称。

## 发布前检查

```bash
git status --short                     # 必须干净；CI 会拒绝 -dirty 镜像
git switch cpyos
git merge --ff-only fork/cpyos
pio run -e readpico
python3 scripts/verify_release_artifacts.py --release
python3 scripts/tests/test_release_artifacts.py
```

`--release` 会检查：

- `esp_app_desc.project_name` 是 `crossmux` 或 `cpyos`（取决于检出目录名，关键在于不是 `arduino-lib-builder`）；
- 芯片 ID、板级 tag `readpico`；
- 镜像不超过 7.19 MiB app0 槽；
- `build/data_manifest.txt` 与磁盘上的四个 blob 的大小 / SHA-256 一致；
- 镜像版本不带 `-dirty`。

## 打 tag 与工作流

```bash
# 本仓库的 fork remote；origin 指向上游 CrossMux，不要往那里推 CPYOS tag
git remote add fork git@github.com:Yana-Hangabina/cpyos.git   # 首次

# 1. 先把版本、CHANGELOG 和两份 release notes 提交到 cpyos
#    platformio.ini [crosspoint] version = <v>
#    docs/releases/<v>.md / docs/releases/<v>.ota.json
git add platformio.ini CHANGELOG.md docs/releases/<v>.md docs/releases/<v>.ota.json
git commit -m "release: <v>"

# 2. tag 用 cpyos- 前缀；<v> 必须与 platformio.ini 的 version 完全一致
git tag -a cpyos-<v> -m "脆皮鸭 CPYOS <v>"
git push fork cpyos
git push fork cpyos-<v>
```

已发布的 tag 不再移动；修复发新的 patch 版本。

`.github/workflows/cpyos-release.yml` 会：

1. 检出 `cpyos` 与子模块，应用 `patches/freeink-sdk` 和 `patches/freeink-sdk-cff`；
2. 构建 `readpico`；
3. 生成数据包与 `build/data_manifest.txt`；
4. 运行 `verify_release_artifacts.py --release`；
5. 运行 `scripts/build_cpyos_release.py --version <tag>`；
6. 创建 GitHub Release，上传完整包、应用固件、manifest、OTA 清单和 `SHA256SUMS`；
7. 把站点与二进制部署到 GitHub Pages。

手动复现：

```bash
pio run -e readpico
python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/ --strip-unused
python3 scripts/build_web_pack.py
python3 scripts/generate_hyphenation_db.py
python3 scripts/generate_pinyin_db.py /path/to/pypinyin-0.55.0-py2.py3-none-any.whl
python3 scripts/generate_data_manifest.py
python3 scripts/build_cpyos_release.py --version 0.1.0
python3 scripts/build_cpyos_site.py --release-dir dist/cpyos/0.1.0 --output _site
```

## 产物

| 文件 | 用途 |
|---|---|
| `cpyos-readpico-<ver>-full.zip` | 完整安装包（含 `FLASH.md`、`SHA256SUMS`） |
| `readpico-firmware.bin` | app-only，OTA / 手动升级 |
| `readpico-bootloader.bin` / `readpico-partitions.bin` / `readpico-boot_app0.bin` | 首次安装 |
| `pinyin.dat` / `hyphen.dat` / `i18n_pack.dat` / `web_pack.dat` | 数据分区 |
| `manifest-install.json` | 网页刷机：完整安装 / 恢复（擦除） |
| `manifest-upgrade.json` | 网页刷机：CPYOS 升级（保留 NVS） |
| `manifest-app.json` | 仅 app |
| `readpico-ota.json` | OTA 清单（GitHub Release 附件，扁平名） |
| `latest.json` | 站点版本、下载链接、更新说明 |
| `SHA256SUMS` | 所有发布文件的校验和 |

## OTA 清单

设备只读两个字段：

```json
{
  "tag_name": "0.1.0",
  "assets": [
    {
      "name": "firmware.bin",
      "browser_download_url": "https://yana-hangabina.github.io/cpyos/downloads/0.1.0/readpico-firmware.bin",
      "size": 6525840
    }
  ]
}
```

- `name` 必须是字面量 `firmware.bin`，即使 URL 指向 `readpico-firmware.bin`。
- `tag_name` 不要带 `v`。
- 可选 `release_notes` 数组：2–8 条，每条 < 97 字节且不含 `` `*_#<>[]{}\ ``；中文每条不超过 32 字。
- 清单由 `scripts/build_cpyos_release.py` 生成，发布后自动写到 `/ota/readpico.json`。

## 子模块补丁与 `-dirty`

发布工作流在应用 `freeink-sdk` 的两个补丁后执行：

```bash
git update-index --assume-unchanged freeink-sdk
```

这不是掩盖错误：补丁是发布配方的一部分，但 PlatformIO 用 `git describe --dirty` 生成 `esp_app_desc.version`，会把“子模块工作树被打过补丁”当成脏构建。assume-unchanged 让这个预期内的子模块改动不进入版本串；父仓库里真正的未提交改动仍会被 `verify_release_artifacts.py --release` 拒绝。本地复现干净版本时也可以这样做；只想本地打包、不发布时可以用 `build_cpyos_release.py --allow-dirty`。

## 数据包与 OTA

OTA 只写 app0，不写数据分区。语言包、Web 资源包、拼音和断词表都留在设备原来的数据分区里，所以：

- **0.1.0 是首个公开版本，首次必须用完整安装包**，四个数据包会随安装写入；之后从 0.1.0 做 OTA 是安全的。
- 以后任何版本只要改了 `i18n_pack.dat` 的键数 / 内容签名、`web_pack.dat`、`hyphen.dat` 或 `pinyin.dat`，**不能只发 OTA**：设备的旧包会被固件拒绝（语言包键数不符时回退英文，网页显示恢复页）。这种版本必须要求完整安装，或者在实现数据分区 OTA 之前不要把它标成可通过设备 OTA 更新。
- 发布前比较上一版与本次的四个 blob 头（键数、contentSignature、schema）和 `SHA256SUMS`；不同就按“需要完整安装”发布，并在 release notes 里写明。`build/data_manifest.txt` 是本次包的唯一描述，不要手抄数字。

## Pages 环境与 tag 部署

发布工作流在 tag 上运行，并把同一份产物部署到 GitHub Pages。仓库的 `github-pages` 环境默认只允许 `cpyos` 分支部署；第一次 tag 发布会失败：

```
Tag "0.1.0" is not allowed to deploy to github-pages due to environment protection rules.
```

在 **Settings → Environments → github-pages → Deployment branch and tag rules** 中添加一条 **tag `*`**（或 `cpyos-*`）。分支 `cpyos` 的规则保留，这样 site-only 推送和 release 推送都能部署。这个设置只需要做一次；它不会放宽到其他环境。注意 `[0-9]*` 不再匹配 `cpyos-0.x.y`。

## 发布后验收

1. `https://yana-hangabina.github.io/cpyos/latest.json` 返回新版本；
2. `https://yana-hangabina.github.io/cpyos/ota/readpico.json` 的 `tag_name` 与 `size` 正确；
3. 网页刷机页两个按钮都能加载 manifest，且每个 part 的文件可访问；
4. 在真机上用「设置 → 系统 → 检查更新」走一遍 OTA（至少一次），确认重启后版本变为 `<version>-cpyos`；
5. 首次安装与升级各走一遍，确认数据分区四个包被写入、语言/拼音/断词/网页正常；
6. 确认 Release 附件齐全，`SHA256SUMS` 与 `latest.json` 中的 SHA-256 一致。

## 回滚与热修

- GitHub Release 附件不覆盖；需要回滚时重新指向上一版 OTA 清单，或让用户刷上一版完整包。
- 紧急修复走新的 patch 版本（`1.7.1`），不要重打同一个 tag。
- 如果 `platformio.ini` 的 version 改了但 tag 没改，工作流会拒绝发布。

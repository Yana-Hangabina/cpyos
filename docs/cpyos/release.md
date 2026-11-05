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
- `build/data_manifest.txt` 与磁盘上的五个 blob 的大小 / SHA-256 一致；
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
python3 scripts/build_cpyos_release.py --version 0.6.2
python3 scripts/build_cpyos_site.py --release-dir dist/cpyos/0.6.2 --output _site
```

## 产物

| 文件 | 用途 |
|---|---|
| `cpyos-readpico-<ver>-full.zip` | 完整安装包（含 `FLASH.md`、`SHA256SUMS`） |
| `readpico-firmware.bin` | app-only，OTA / 手动升级（app0） |
| `readpico-app1.bin` | 同一镜像，升级/仅 app 清单写到 app1（避免设备从非活动槽启动旧固件） |
| `readpico-bootloader.bin` / `readpico-partitions.bin` / `readpico-boot_app0.bin` | 首次安装 |
| `pinyin.dat` / `hyphen.dat` / `i18n_pack.dat` / `web_pack.dat` / `emoji.dat` | 数据分区 |
| `readpico-packs.txt` | 设备 OTA 的数据包清单（偏移、大小、SHA-256、擦除上限） |
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
  "tag_name": "0.1.2",
  "assets": [
    {
      "name": "firmware.bin",
      "browser_download_url": "https://gh-proxy.com/https://github.com/Yana-Hangabina/cpyos/releases/download/cpyos-0.6.2/readpico-firmware.bin",
      "size": 6525840
    }
  ]
}
```

- `name` 必须是字面量 `firmware.bin`，即使 URL 指向 `readpico-firmware.bin`。
- `tag_name` 不要带 `v`。
- **固件 URL 是镜像前缀的 GitHub Release 资产**，不是 Pages：Pages 在国内实测约 9 KB/s，6.8 MB 要十几分钟。
  0.1.1 及更早固件会原样下载该 URL；0.1.2+ 会剥离镜像前缀，再在直连与镜像列表间测速择优（0.2.2 起共 7 个镜像）。
  两个镜像常量在 `src/network/CrossMuxEndpoints.h`，清单前缀在 `scripts/build_cpyos_release.py` 的 `OTA_MIRROR_PREFIX`。
- 可选 `release_notes` 数组：2–8 条，每条 < 97 字节且不含 `` `*_#<>[]{}\ ``；中文每条不超过 32 字。
- 清单由 `scripts/build_cpyos_release.py` 生成，发布后自动写到 `/ota/readpico.json`。

## 子模块补丁与 `-dirty`

发布工作流在应用 `freeink-sdk` 的两个补丁后执行：

```bash
git update-index --assume-unchanged freeink-sdk
```

这不是掩盖错误：补丁是发布配方的一部分，但 PlatformIO 用 `git describe --dirty` 生成 `esp_app_desc.version`，会把“子模块工作树被打过补丁”当成脏构建。assume-unchanged 让这个预期内的子模块改动不进入版本串；父仓库里真正的未提交改动仍会被 `verify_release_artifacts.py --release` 拒绝。本地复现干净版本时也可以这样做；只想本地打包、不发布时可以用 `build_cpyos_release.py --allow-dirty`。

## 数据包与 OTA

OTA 会写非活动的 app 槽，并在 `readpico-packs.txt` 存在时更新数据分区。发布脚本会自动生成这个清单并打包；设备只下载与当前分区校验和不同的包，先下载到 SD 临时文件并校验 SHA-256，再擦写并回读校验。下载失败时 app 仍可更新，新固件会继续使用旧包并在首页提示重试；写入失败时不切换新 app。

- **0.1.0 是首个公开版本，首次必须用完整安装包**，数据包会随安装写入。
- 以后语言包新增键或修改译文不再要求完整安装：新固件接受键集合为当前前缀的旧包，新增键回退英文；`readpico-packs.txt` 会把新包一并更新。
- 只有分区表、包格式或数据分区布局发生破坏性变化时才要求完整安装。
- 发布前确认 `readpico-packs.txt` 与五个 blob 的 size/sha256 一致；`build/data_manifest.txt` 是唯一描述，不要手抄数字。

## Pages 环境与 tag 部署

发布工作流在 tag 上运行，并把同一份产物部署到 GitHub Pages。仓库的 `github-pages` 环境默认只允许 `cpyos` 分支部署；第一次 tag 发布会失败：

```
Tag "0.1.0" is not allowed to deploy to github-pages due to environment protection rules.
```

在 **Settings → Environments → github-pages → Deployment branch and tag rules** 中添加一条 **tag `*`**（或 `cpyos-*`）。分支 `cpyos` 的规则保留，这样 site-only 推送和 release 推送都能部署。这个设置只需要做一次；它不会放宽到其他环境。注意 `[0-9]*` 不再匹配 `cpyos-0.x.y`。

## 发布后验收

1. `https://yana-hangabina.github.io/cpyos/latest.json` 返回新版本；
2. 设备实际读取的 `https://gh-proxy.com/https://github.com/Yana-Hangabina/cpyos/releases/latest/download/readpico-ota.json` 的 `tag_name` 与 `size` 正确；Pages 的 `/ota/readpico.json` 仍应同步更新；
3. 网页刷机页两个按钮都能加载 manifest，且每个 part 的文件可访问；
4. 在真机上用「设置 → 系统 → 检查更新」走一遍 OTA（至少一次），确认重启后版本变为 `<version>-cpyos`；
5. 首次安装与升级各走一遍，确认数据分区四个包被写入、语言/拼音/断词/网页正常；
6. 确认 Release 附件齐全，`SHA256SUMS` 与 `latest.json` 中的 SHA-256 一致。

## 回滚与热修

- GitHub Release 附件不覆盖；需要回滚时重新指向上一版 OTA 清单，或让用户刷上一版完整包。
- 紧急修复走新的 patch 版本（`1.7.1`），不要重打同一个 tag。
- 如果 `platformio.ini` 的 version 改了但 tag 没改，工作流会拒绝发布。

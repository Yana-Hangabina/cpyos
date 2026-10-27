# 同步上游 / Syncing with CrossMux

本仓库是 `0x1abin/crossmux` 的 fork，分支职责固定：

| 分支 | 内容 | 默认 |
|---|---|---|
| `main` | 上游镜像，只做 fast-forward | 否 |
| `cpyos` | CPYOS 的发布分支 | **是** |

## 日常同步

```bash
git fetch upstream
git switch main
git merge --ff-only upstream/main
git switch cpyos
git merge main
```

GitHub 的「Sync fork」按钮只更新 `main`，不会改 `cpyos`；同步后仍需把 `main` 合进 `cpyos`。

## 子模块与补丁

`freeink-sdk` 是固定提交的 gitlink，本地显示 / 字体改动不在 gitlink 里，而在：

```
patches/freeink-sdk/96de1be-local.patch
patches/freeink-sdk-cff/cff-otf.patch
```

执行 `git submodule update --init --recursive` 会覆盖工作树，之后必须重新应用：

```bash
bash patches/freeink-sdk/apply.sh
bash patches/freeink-sdk-cff/apply.sh
```

两个补丁路径不相交，顺序无关；`apply.sh` 是幂等的。若上游更新了 gitlink，先确认补丁的基线是否仍是新提交，必要时重生成补丁并在 PR 中说明。

## 冲突处理

- CPYOS 自有文件（`site/`、`scripts/build_cpyos_*`、`.github/workflows/cpyos-*`、`docs/cpyos/`、`docs/releases/`）通常不会与上游冲突。
- 冲突集中在 `platformio.ini`、`src/activities/home/*`、`src/network/CrossMuxEndpoints.h`、`src/network/OtaUpdater.cpp`、i18n YAML、`README*`。
- `platformio.ini`：保留 `version = <CPYOS 版本>` 与 Read Pico 环境里的 `CPYOS_OTA_MANIFEST_URL`；上游新增的环境照常合并。
- `CrossMuxEndpoints.h` / `OtaUpdater.cpp`：CPYOS OTA 分支在 `#ifdef CPYOS_OTA_MANIFEST_URL` 内，其他目标必须保持上游行为。
- i18n：合并后重跑 `python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/ --strip-unused`，并确认语言包键数与数据包一起更新。
- README：保留 CPYOS 的中文主 README 与 `README.en.md`；上游新增的工程说明可以并入 `docs/`。

## 同步后的工作流

本仓库只保留 **CPYOS Release** 与 **CPYOS Pages** 两个上游工作流为启用状态；上游的 CI、Nightly、硬件 CI、字体发布等工作流已通过 Actions 设置手动禁用（它们依赖上游的自托管 runner、COS 凭据或硬件）。同步后如果上游新增了工作流，请在仓库 **Actions → 左侧工作流 → ⋯ → Disable workflow** 中把非 CPYOS 的条目禁用，避免每次推送都出现无意义的失败。

## 同步后的验收

```bash
pio run -e readpico
python3 scripts/verify_release_artifacts.py --release
```

镜像的 `esp_app_desc.project_name` 必须是 `crossmux` 或 `cpyos`（不能是 `arduino-lib-builder`），`version` 是当前 CPYOS 版本。不要因为构建通过就发布；还要在真机上走一遍 OTA 和一次完整安装（见 [release.md](./release.md)）。

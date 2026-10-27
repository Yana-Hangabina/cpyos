# 脆皮鸭 CPYOS

**English** | [简体中文](./README.md)

**脆皮鸭 CPYOS** is a reading-first firmware for the **Read Pico** e-ink reader (MindReset RDP-G01-W, ESP32-S3, 684×1216, 16 grey levels). It is built on [CrossMux](https://github.com/0x1abin/crossmux) and adds a refreshed UI, native JJWXC and WeRead clients, Chinese input, water-ripple page turns and device OTA.

> Project site and browser flasher: [yana-hangabina.github.io/cpyos](https://yana-hangabina.github.io/cpyos/)  
> Releases and full install packages: [Releases](https://github.com/Yana-Hangabina/cpyos/releases)

## Highlights

- **Refreshed UI** — home, library, find-books and settings redesigned around what a Read Pico actually shows. Every source (WeRead, JJWXC, local files, OPDS) exposes its own sign-in and shelf on the find-books screen.
- **Native JJWXC client** — shelf, discover/rankings, book/author/advanced search, favourites, chapter purchase, paragraph comments (read, like, reply, post), reading-progress sync and update snapshots. Free chapters use the App API; VIP chapters use the WAP decryption chain, per-chapter PUA font mapping and CSS displacement.
- **WeRead** — QR sign-in, shelf, EPUB download, offline reading and progress sync, sharing one library with JJWXC and local books.
- **Chinese reading** — offline pinyin search and IME, 11 hyphenation languages, GB2312 fallback, SD-card TTF/OTF/TTC and embedded EPUB fonts.
- **E-ink display** — water-ripple page turn, fast black/white page turn, asynchronous pushes, area-refresh clock, screen repair and kiikoread-derived gamma/TEXTTURN tuning.
- **Reading and library** — EPUB, TXT, CBZ, FB2, experimental PDF; bookmarks, annotations, dictionary, reading statistics, standby faces and KOReader progress sync.
- **System** — device OTA, SD-card firmware update, browser transfer/settings, WebDAV, OPDS, Calibre wireless, optional USB MSC and 33 UI languages loaded from the data partition.
- **Encryption** — the packaged EPUB and cached chapter text use device-bound keys; plaintext reader caches are removed on exit.

## Install

The prebuilt release is **Read Pico only**. Back up the full 16 MiB flash before the first install:

```bash
python -m esptool --chip esp32s3 read_flash 0x0 0x1000000 readpico-backup.bin
```

Then use desktop Chrome/Edge on the [project site](https://yana-hangabina.github.io/cpyos/#install). Use **First install / recovery** when coming from factory firmware or CrossMux 1.6.5 (it rebuilds the partition table), and **Upgrade from CPYOS** for an existing CPYOS installation.

After 0.1.0 is installed, update on the device via **Settings → System → Check for updates**. OTA writes app0 only and keeps the SD card, books and settings. The firmware reads its manifest from:

```
https://yana-hangabina.github.io/cpyos/ota/readpico.json
```

See [docs/cpyos/install.md](docs/cpyos/install.md) for the complete procedure.

## Build from source

```bash
git clone --recursive https://github.com/Yana-Hangabina/cpyos.git
cd cpyos
git submodule update --init --recursive
bash patches/freeink-sdk/apply.sh
bash patches/freeink-sdk-cff/apply.sh
python3 scripts/patch_pioarduino_cache.py --prepare-platform
pio run -e readpico

python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/ --strip-unused
python3 scripts/build_web_pack.py
python3 scripts/generate_hyphenation_db.py
python3 scripts/generate_pinyin_db.py /path/to/pypinyin-0.55.0-py2.py3-none-any.whl
python3 scripts/generate_data_manifest.py
python3 scripts/build_cpyos_release.py --version 0.1.0
```

The release bundle is written to `dist/cpyos/0.1.0/`. Check `[SUCCESS]` in the build log and `esp_app_desc.project_name` is `crossmux` or `cpyos` in the image (not `arduino-lib-builder`), not just the process exit code. See [docs/cpyos/release.md](docs/cpyos/release.md) for the maintainer checklist.

## Upstream sync

This repository is a fork of [`0x1abin/crossmux`](https://github.com/0x1abin/crossmux).

| Branch | Purpose |
|---|---|
| `main` | Upstream mirror |
| `cpyos` | **Default branch**, CPYOS releases |

```bash
git remote add upstream https://github.com/0x1abin/crossmux.git
git fetch upstream
git switch main
git merge --ff-only upstream/main
git switch cpyos
git merge main
```

## Credits

- **CrossMux**, **CrossPoint Reader**, **Inx**, **cpr-vcodex** and **diy-esp32-epub-reader** for the upstream firmware, interface work and early EPUB experiments.
- **kiikoread / read-pico-reader (kiikoread, Apache-2.0)** for the water-ripple page-turn engine, TEXTTURN settle, gamma cover LUT, offline pinyin search/IME and shake page-turn thresholds. Porting differences are recorded in `patches/freeink-sdk/` and the relevant source comments.
- **MuPDF** (AGPL-3.0, experimental PDF), **FreeType**, **Noto Sans CJK / Source Han Sans** (OFL), **pypinyin** (MIT), **Lucide** (ISC) and the hyphenation dictionary authors.
- The refreshed UI design language and the typography of this project site.

See [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md) for the full list.

## Security and disclaimer

This is unofficial firmware, not affiliated with MindReset, JJWXC or WeRead. The JJWXC and WeRead endpoints are private and may change. Device traffic is encrypted, but the clients do not verify the server certificate or hostname; use a trusted network. No account, password, token, cookie or copyrighted chapter text is stored in this repository. Flashing is at your own risk; always keep a full-chip backup.

## License

The project code is released under the [MIT License](./LICENSE). Third-party components keep their own licenses; MuPDF-enabled experimental builds are distributed under AGPL-3.0. See [THIRD_PARTY_NOTICES.md](./THIRD_PARTY_NOTICES.md).

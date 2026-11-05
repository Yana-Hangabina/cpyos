# OTA data-pack updates

The app image and the data partition used to drift apart: app-only OTA left the
language/web/emoji/pinyin/hyphen packs at the previous release, and a language
pack built for a different key set was rejected wholesale, so every packed
language fell back to English until the user did a full flash.

This change makes app-only OTA safe and lets the device update the data packs
itself.

## 1. Language packs are prefix-compatible

`gen_i18n.py` bakes `I18N_KEY_PREFIX_SIGNATURES[]` into `I18nKeys.h`:

```
I18N_KEY_PREFIX_SIGNATURES[n] = FNV-1a over the first n key names
```

The loader accepts a pack when:

- its key count is **greater** than the image → rejected (the pack is newer than
  the app and positional offsets would be wrong);
- its key count equals the image → the full key signature must match;
- its key count is smaller → its signature must equal the prefix signature for
  that count.

When an older pack is accepted, keys the pack does not contain resolve to the
English table at the same index. Existing translations keep working; only the
newly added strings show English until a pack update arrives. `contentSignature`
is logged but no longer rejects a pack: translation text is content, not schema.

**Rule for future development: append new keys at the end of the YAML files.**
Do not delete, rename or reorder existing keys; a reorder changes the prefix and
will fall back to English until a full pack update. (The pack updater below can
also repair that.)

Tests: `src/i18n_pack/run_i18n_pack_test.sh` (host) covers prefix-compatible
headers and edited text with an unchanged key set.

## 2. OTA updates the data packs

`OtaUpdater` now reads a flat manifest next to the release manifest:

```
readpico-packs.txt
# asset offset size sha256 limit
pinyin.dat 0x000000 349500 <sha256> 0x056000
web_pack.dat 0x056000 157876 <sha256> 0x090000
hyphen.dat 0x090000 351756 <sha256> 0x100000
i18n_pack.dat 0x100000 334398 <sha256> 0x160000
emoji.dat 0x160000 126544 <sha256> 0x180000
```

Offsets are relative to the spiffs partition; `limit` is the next pack’s offset
or the partition end. The device rejects a range that is unaligned, overlaps the
factory bytes at `+0x80000`, or whose erase would cross `limit`.

For every changed pack the updater:

1. hashes the current partition bytes and skips unchanged blobs;
2. downloads the new blob to `/.crosspoint/ota_<asset>`;
3. verifies size and SHA-256 before erasing anything;
4. erases/writes in 4 KiB chunks, never past `limit`;
5. reads the range back and compares SHA-256 again.

App OTA writes the app image first, then the packs, and only selects the new app
slot after both succeed. If a pack download fails but the app image is complete,
the app update proceeds (the new app tolerates the old pack); if a pack write
fails, the new app is not selected. A power loss during a pack write leaves a
rejected pack and English fallback, recoverable by running Check updates again.

`OtaUpdateActivity` also offers a **packs-only** update: when the app version is
current but `readpico-packs.txt` has different blobs, Check updates shows
"语言与资源包" and updates only the partition. This is the transition path from
0.5.0, and the retry path after a failed pack write.

## 3. Release pipeline

`scripts/generate_pack_manifest.py` turns `build/data_manifest.txt` into
`build/readpico-packs.txt`. The release bundle must:

1. run it after `generate_data_manifest.py`;
2. copy `readpico-packs.txt` to the release output (so sibling URLs work on
   GitHub and on Pages);
3. copy it to `_site/ota/readpico-packs.txt` in `build_cpyos_site.py` (for the
   Pages-hosted OTA manifest);
4. include it in `SHA256SUMS`.

The device derives the pack manifest URL from the selected firmware URL by
replacing the last path segment with `readpico-packs.txt`, so it uses the same
mirror that served the app image. If the pack manifest is missing (a release
from before this change), app OTA proceeds without touching the data partition.

Concrete release-side changes (in the public tree, at release time):

```python
# scripts/build_cpyos_release.py, after data_rows is populated
import subprocess
subprocess.run([sys.executable, str(root / 'scripts' / 'generate_pack_manifest.py'), '--repo', str(root)], check=True)
copy(root / 'build' / 'readpico-packs.txt', output / 'readpico-packs.txt')
```

```python
# scripts/build_cpyos_site.py, next to the ota/readpico.json copy
shutil.copy2(release_dir / 'readpico-packs.txt', output / 'ota' / 'readpico-packs.txt')
```

`generate_pack_manifest.py` is kept in the dev tree and is copied by the normal
dev → public sync. It validates each blob against `build/data_manifest.txt`.


## 4. Failure and compatibility matrix

| situation | result |
|---|---|
| app OTA, pack manifest absent | app updates; old packs retained; new app accepts prefix |
| app OTA, pack download fails | app updates; packs skipped and retried later |
| app OTA, pack write fails | app not selected; old app remains bootable |
| power loss during pack write | pack rejected; English fallback; Check updates repairs |
| pack has more keys than app | rejected; English fallback (pack newer than app) |
| older pack, append-only keys | accepted; new keys English |
| keys removed/reordered | pack rejected; full pack update or full install |

## 5. Capacity

`i18n_pack.dat` currently ends well before `emoji.dat` (`0x160000`), leaving
~58 KiB for about 250 more keys. The emoji pack ends ~4.5 KiB before the
partition end. Moving emoji/web to SD, or changing the partition table (a
full-install operation), is the next step if i18n outgrows that reserve.

# Pinyin — offline Pinyin search and Chinese composition

Chinese book titles become searchable by Pinyin (`dushu` finds 读书) and text
fields gain a continuous-Pinyin input mode, both from one data file on the SD
card. Ported from [read-pico-reader](https://github.com/wegooo-cell/read-pico-reader)
(kiikoread, Apache-2.0), whose matcher and IME rules were frozen in turn:
per-character polyphonic alternatives, no semantic guessing, phrases ahead of
single characters, and candidate pages that never truncate a reading.

## Where the data lives

The generated database is about 349 KB — several times the app image's
remaining flash — so it never rides in the application. It has two sources, and
the loader takes them in order:

| Source | How it gets there | Role |
|---|---|---|
| `/pinyin.dat` on the SD card | copy it, or upload through the web UI's file manager (`/upload` with `path=/`) | the updatable copy; replace the table without reflashing |
| Bundled in the data partition | written by the flasher (second image, see below) | what makes a freshly flashed device work with no card preparation at all |

| Piece | Where |
|---|---|
| `scripts/generate_pinyin_db.py`, `scripts/pinyin/phrases.tsv` | repo; regenerates the database from the pinned wheel |
| Pinyin dictionary data (pypinyin 0.55.0) | MIT, see `scripts/pinyin/LICENSE.pypinyin` |

**Flashing bundles it.** CrossMux's partition tables carry a data partition
(`data`/`spiffs`, readpico: 1.5 MB at `0xe70000`) that nothing mounts — no
code in the tree registers a filesystem there, and a device dump shows it
erased. The flasher writes the same `pinyin.dat` file into it as a second
image, and the firmware, finding no card copy, reads it into PSRAM exactly like
the card file:

```sh
# readpico; the offset comes from partitions.csv
~/.platformio/penv/bin/python -m esptool --chip esp32s3 --port "$PORT" --baud 921600 \
  write_flash 0x10000 firmware.bin 0xe70000 build/pinyin.dat
```

The firmware only ever *reads* that partition: a board whose data partition
holds a real filesystem, or another board's payload, fails the 96-byte header
check and keeps its old behaviour. Load order means a card file shadows the
bundled copy — that is how the table is updated without a reflash.

Everything degrades cleanly when neither source has the file, or the copy is
unreadable or corrupt: validation refuses the image (magic, section bounds,
whole-file CRC), and both features fall back to their old behaviour. No-PSRAM
builds (the C3 targets) can never load it, so the feature is simply absent
there — the firmware says so in one log line and moves on.

## Layout

| Item | Role |
|---|---|
| `PinyinFormat.h/.cpp` | `pinyin.dat` header, structural validation, CRC-32 |
| `PinyinTables.h/.cpp` | the matcher (literal / full Pinyin / initials) and IME lookups over a validated image; allocation-free and reentrant |
| `PinyinIme.h/.cpp` | bounded composition: DP over phrases and single characters, homophone paging, T9 digit support |
| `PinyinDb.h/.cpp` | lazy SD→PSRAM load, held for the session |

## Regenerating the database

```sh
python3 -m pip download --no-deps pypinyin==0.55.0 -d build/pinyin-deps
python3 scripts/generate_pinyin_db.py build/pinyin-deps/pypinyin-0.55.0-py2.py3-none-any.whl
# -> build/pinyin.dat  (copy to the card root as /pinyin.dat)
```

The generator verifies the wheel's SHA-256 against the pinned hash before
reading anything, uses only the standard library, and is deterministic — the
same wheel produces the same bytes, CRC included. Counts: 41,923 characters,
4,479 reading groups, 425 syllables, 20,924 candidate characters in 27,317
reading mappings, 487 phrases.

## Tests

Host tests live in the cpyos project (`src/PinyinTablesTest.cpp`,
`src/PinyinImeTest.cpp`); both run the real database and print PASS/FAIL.
Richer coverage runs under ASan/UBSan, and the matcher and composer were
cross-checked against read-pico-reader's C implementation on random inputs
(20,000 and 3,836 cases, zero divergences) during the port.

## Known limits

- Pinyin matching is a user-facing search convenience; it has no bearing on
  file names, so books outside the table (no reading recorded) stay reachable
  by their literal text only.
- The composition session is one at a time on the main task and lives in PSRAM
  (about 19 KB); the search side is read-only and safe to call from any task
  that owns the tables' lifetime.
- T9 input has no UI in this firmware yet (no digit-grid layer); the engine
  path is implemented and tested so a future layer only needs the screen.

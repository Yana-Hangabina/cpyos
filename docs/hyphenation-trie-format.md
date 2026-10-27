# Hypher Binary Tries

CrossPoint embeds the exact binary automata produced by
[Typst's `hypher`](https://github.com/typst/hypher). 

## File layout

Each `.bin` blob is a single self-contained automaton:

```
uint32_t root_addr_be;  // big-endian offset of the root node
uint8_t  levels[];      // shared "levels" tape (dist/score pairs)
uint8_t  nodes[];       // node records packed back-to-back
```

The size of the `levels` tape is implicit. Individual nodes reference slices
inside that tape via 12-bit offsets, so no additional pointers are required.

### Node encoding

Every node starts with a single control byte:

- Bit 7 – set when the node stores scores (`levels`).
- Bits 5-6 – stride of the target deltas (1, 2, or 3 bytes, big-endian).
- Bits 0-4 – transition count (values ≥ 31 spill into an extra byte).

If the `levels` flag is set, two more bytes follow. Together they encode a
12-bit offset into the global `levels` tape and a 4-bit length. Each byte in the
levels tape packs a distance/score pair as `dist * 10 + score`, where `dist`
counts how many UTF-8 bytes we advanced since the previous digit.

After the optional levels header come the transition labels (one byte per edge)
followed by the signed target deltas. Targets are stored as relative offsets
from the current node address. Deltas up to ±128 fit in a single byte, larger
distances grow to 2 or 3 bytes. The runtime walks the transitions with a simple
linear scan and materializes the absolute address by adding the decoded delta
to the current node’s base.

## Embedding blobs into the firmware

The helper script `scripts/generate_hyphenation_trie.py` acts as a thin
wrapper: it reads the hypher-generated `.bin` files, formats them as `constexpr`
byte arrays, and emits headers under
`lib/Epub/Epub/hyphenation/generated/`. Each header defines the raw data plus a
`SerializedHyphenationPatterns` descriptor so the reader can keep the automaton
in flash.

A convenient script `update_hyphenation.sh` is used to update all languages.
To use it, run:

```sh
./scripts/update_hyphenation.sh
```

Note that `update_hyphenation.sh` fetches `refs/heads/main` from hypher with no
pin and no checksum, so regenerating the headers is not reproducible over time.
Repacking what is already in the tree (`generate_hyphenation_db.py`, below) is.

### Two details the layout above does not show

Both were found by reading the generator and the walker against this document:

- **The 4-byte root header is stripped from the embedded blob.**
  `generate_hyphenation_trie.py` emits `blob[4:]` and stores `root_addr - 4` as the
  descriptor's `rootOffset`. Consequently a node's 12-bit levels offset is an
  address in hypher's *original* space, and `LiangHyphenation.cpp` maps it back with
  `data + offset - 4`. Well-formed data therefore never has `offset < 4`; the walker
  rejects it explicitly, because the subtraction is unsigned and a corrupt blob
  would otherwise produce a pointer far above the buffer.
- **Stride bits 5-6 can decode to 0**, which the walker coerces to 1. The
  "1, 2, or 3" wording above describes what hypher emits, not what the field can
  hold.

## Loading the tries from outside the image

The eleven tries total 351,458 bytes. On a board whose app partition is close to
full that is the largest single reclaimable block in the image, so builds with
PSRAM do not embed them: `HyphenationEmbedding.h` picks one of two modes, and
exactly one is defined.

| Mode | Defined when | Where the bytes come from |
| --- | --- | --- |
| `CROSSPOINT_EMBED_HYPHENATION` | no PSRAM (C3), the simulator, and any host test | the generated headers, as before |
| `CROSSPOINT_HYPHENATION_FROM_STORAGE` | `BOARD_HAS_PSRAM` (Read Pico) | `/hyphen.dat`, else the data partition |

Either macro can also be forced by the build. `test/hyphenation_eval` and
`test/korean_line_breaking` force the embedded one, because a host test has no card
and no partition and their assertions need real tries.

### The container: `/hyphen.dat`

Produced by `scripts/generate_hyphenation_db.py`, which parses the same generated
headers the firmware used to embed, so the packed bytes are identical to the
`.rodata` they replace by construction. Layout is in
`lib/Epub/Epub/hyphenation/HyphenationDataFormat.h` (HAL-free, host-testable):

```
header   64 bytes   magic "HYPN", version 1, totalSize, indexCrc32, entryCount, blobsOff
index    20 bytes per language: lang[2], rootOffset, blobOff, blobSize, blobCrc32
blobs    the trie bytes, each 4-byte aligned
```

CRC-32 is the reflected IEEE polynomial `zlib.crc32` produces, computed
byte-at-a-time so neither side needs a 1 KiB table. The index CRC covers the header
(with its own CRC field read as zero) plus the index; each blob carries its own CRC,
so a loader that wants one language hashes that language rather than the whole
352 KB. `blobsOff` is *derived* from the entry count and compared, never trusted:
an arbitrary value would let a crafted header overlap the index with a blob.

There was no magic and no version anywhere in this data before, and the runtime
only ever bounds-checked per node. That was adequate while the bytes came from
`.rodata` the firmware was built with. It is not adequate once they can arrive from
a user-writable card or an HTTP upload: a trie parsed against the wrong automaton
layout does not crash, it hyphenates words in the wrong places. Hence a mandatory
magic and version, and a refusal (never a guess) on any mismatch.

### Loader behaviour

`HyphenationStore` (`lib/Epub/Epub/hyphenation/HyphenationStore.{h,cpp}`) reads the
header and index once, then one blob per language into PSRAM on first use. Sources
are tried card-then-partition, the same order `PinyinDb` uses, so a card copy wins
and a freshly flashed device still works with no card at all. Loaded tries are kept
for the session and never evicted: `LanguageRegistry` installs the pointer into a
descriptor that outlives the call, so freeing a blob behind it would leave layout
reading freed PSRAM. Failures are latched per language, so a card with no dictionary
costs one `stat` per section rather than a re-read and a re-hash.

Every failure ends at the `nullptr` hyphenator the code already had: explicit
hyphens, soft hyphens and apostrophe contractions still break, and `ParsedText`'s
emergency fallback still applies. A missing dictionary is therefore not a new kind
of failure, and it needs no new UI string.

`Section.cpp` only resolves a hyphenator when `spec.hyphenationEnabled` is set,
because resolving is what triggers the load and hyphenation is off by default.

### Data partition map

The bundled copy is written into the `spiffs` partition, which nothing in this tree
mounts. The map is **generated**, not remembered: `scripts/generate_data_manifest.py`
writes `build/data_manifest.txt`, taking the partition's base and size from
`partitions.csv` and the hyphenation offset from `HYPN_PARTITION_OFFSET` in
`HyphenationStore.h` -- so the C++ loader stays the only source of truth for where it
reads from, and a flashing tool can read the manifest instead of hardcoding
addresses. The generator refuses a layout whose blobs overlap, that runs past the
partition, or that touches the reserved gap below.

```sh
python3 scripts/generate_hyphenation_db.py       # -> build/hyphen.dat
python3 scripts/generate_data_manifest.py        # -> build/data_manifest.txt
esptool ... write_flash 0x10000 firmware.bin \
                        0xe70000 build/pinyin.dat \
                        0xd20000 build/hyphen.dat     # both addresses from the manifest
```

| Partition offset | Flash address | Contents |
| --- | --- | --- |
| `0x00000` | `0xe70000` | `pinyin.dat` (349,500 bytes) |
| `0x80000` | `0xef0000` | **unused on purpose** -- see below |
| `0x90000` | `0xf00000` | `hyphen.dat` (about 352 KB) |

`0x80000` holds twelve bytes that are not `0xFF` in the factory image
(`01 00 00 00` at +0, `02 00 00 00 9a 98 43 47` at +0x18). They are the only
non-erased bytes in the whole 1.5 MB partition, they predate this project, and
nothing here reads them. The container is placed past them rather than over them:
routing around an unexplained factory artifact costs one line in a flash command,
and overwriting one is not reversible. The manifest generator treats that gap as a
hard constraint, so a future blob cannot be moved onto it by accident.

This is also why the two datasets are **not** merged into one container at partition
offset 0: 701,256 bytes laid out contiguously would span `0x80000`, so a combined
container would either overwrite those bytes or need the same reserved gap encoded in
its format -- the knowledge would move, not disappear. Merging would also couple two
unrelated pipelines (Pinyin needs a pinned wheel download, hyphenation is repacked
offline from headers) behind one version number, and would require changing
`PinyinDb::loadBundled()`, which reads from offset 0 unconditionally.

### Verifying that the move changed nothing

`generate_hyphenation_db.py --verify` re-reads a written container and compares
every blob against the array in its generated header. Beyond that,
`cpyos/src/run_hyphenation_test.sh` builds the equivalence test twice from one
source -- once embedded, once loading the container through a stdio stand-in for
the store that still uses the production validation code -- and fails unless the
two runs produce an identical digest over all 45,000 evaluation words and all
80,494 breaks. Each run also asserts the per-language F1 thresholds from
`test/hyphenation_eval`, because two builds that are identically wrong would still
agree.

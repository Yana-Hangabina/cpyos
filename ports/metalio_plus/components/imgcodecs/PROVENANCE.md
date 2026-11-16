# Image codecs (vendored)

Vendored from CPYOS's PlatformIO dependency tree so the Plus port builds
offline and reproducibly. Only `src/` plus each upstream LICENSE is copied;
examples, demo images and test assets are omitted.

| Library | Upstream | License |
|---|---|---|
| JPEGDEC | https://github.com/bitbank2/JPEGDEC | Apache-2.0 (`JPEGDEC/LICENSE`) |
| PNGdec | https://github.com/bitbank2/PNGdec | Apache-2.0 (`PNGdec/LICENSE`) |
| AnimatedGIF | https://github.com/bitbank2/AnimatedGIF | Apache-2.0 (`AnimatedGIF/LICENSE`) |

The PlatformIO build pins JPEGDEC and AnimatedGIF to specific commits; this
copy matches the resolved versions in `.pio/libdeps/readpico`.

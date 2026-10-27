# CBZ reader

CBZ is a ZIP of page images. CrossMux reads it directly instead of converting it
to EPUB, so a comic costs no second copy of its images on the SD card.

## Behaviour

- `Cbz::load()` scans the ZIP central directory twice: once to size the page
  table, once to fill it. Directory entries, `__MACOSX/`, AppleDouble `._*`, and
  non-JPEG/PNG files are skipped.
- Pages are ordered naturally (`page2` before `page10`, `page1` before `page01`)
  by `CbzPageName.h`, which is shared with the host test.
- The page table (offset array + names) lives in PSRAM when available. Without
  PSRAM it falls back to internal RAM only up to 96 KiB; larger archives are
  refused rather than fragmenting the internal heap.
- The reader extracts one page to a temporary file in the book cache, converts
  it to a 1-bit BMP sized to the screen, and draws it. No per-page image is kept
  in RAM or on the card beyond `page.bmp` and the cache's `cover.bmp`.
- Progress is a 4-byte page index in `/progress.bin`, like XTC. The first page
  is the cover and the thumbnail source.

## Limits

- Only `.cbz` (ZIP). CBR is not supported; the UnRAR licence and decoder size
  are not acceptable for this firmware.
- JPEG and PNG only. GIF/WebP/BMP pages are skipped.
- No zoom/pan, no manga right-to-left ordering, no bookmarks or TOC. These are
  deliberate v1 limits; page-turn and progress work through the normal reader
  shell.
- Page conversion uses the existing JPEGDEC/PNG decoders; the first shelf cover
  may take a moment on a large image.

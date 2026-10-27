# FB2 (FictionBook 2) support

FB2 is streamed into a **store-only EPUB** once, then opened by the normal EPUB
reader. That gives FB2 layout, TOC, footnotes, images, fonts, bookmarks,
progress and stats without a second renderer.

## Cache

```
/.crosspoint/fb2_<hash>/book.epub      the reader-visible package
/.crosspoint/fb2_<hash>/fb2_metadata.txt
/.crosspoint/fb2_<hash>/fb2_package.bin   source fingerprint + chapter count + version
```

`Fb2::ensureEpub()` builds the package only when the source size/sampled hash or
the converter version changes. The intermediate `package/` directory is deleted
after the EPUB is written, so an image-heavy book is not doubled on the card.
The generated EPUB is a real store-only ZIP (mimetype first) and can be opened
by any EPUB reader.

`Fb2::probeMetadata()` reads the first 32 KiB only, for the library index. It
supports UTF-8 and the single-byte encodings in `Fb2Encoding` (windows-1251,
KOI8-R, windows-1252), so Russian FB2 titles/authors appear correctly on the
shelf before the book is opened.

## Reader identity

`Epub` now has a `sourcePath` in addition to its physical ZIP path. For FB2 the
reader is constructed with `(generated.epub, source.fb2)`; progress, bookmarks,
recents and reading stats are keyed by the `.fb2` the user sees, while ZIP,
cover and section caches use the generated EPUB. This is the only change the
EPUB reader needed for converted books.

## Limits

- DRM is not supported and is never circumvented.
- `.fb2.zip` is not handled yet; a plain `.fb2` file is required.
- The converter is a streaming subset of FB2: common sections, paragraphs,
  emphasis/strong/strike/sub/sup, links, empty lines, inline images, cover and
  notes. Unusual layout constructs may be flattened.
- Large books are prepared synchronously in the reader's indexing step with
  task-watchdog yields; the first open can take seconds to minutes. Subsequent
  opens use the cached EPUB.
- The intermediate package is removed after a successful zip; if zip writing
  fails, the package is left in place for the next attempt.

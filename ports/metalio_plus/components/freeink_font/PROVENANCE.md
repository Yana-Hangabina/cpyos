# FreeInkFont (vendored)

- Source: `freeink-sdk/libs/font/FreeInkFont` in this repository (MIT), which
  vendors FreeType and stb_truetype under `third_party/`.
- Used by: `lib/EpdFont/TtfEpdFont.cpp` for runtime SD/embedded TTF-OTF rendering.
- This component only lists the sources and include paths the PlatformIO build
  already compiles; no file is modified here.
- The FreeType modules included are base, sfnt, truetype, smooth, raster,
  autofit, psaux, psnames and cff; gzip/woff support is intentionally out
  (the port only opens .ttf/.otf/.ttc).

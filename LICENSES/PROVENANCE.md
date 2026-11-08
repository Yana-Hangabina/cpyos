# Third-party provenance

脆皮鸭 CPYOS ships third-party code and data inside the firmware, the web UI and
generated font/hyphenation tables. This file records where each item came from
and which notice applies. It complements
[`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md); full license texts stay
in this directory where the project ships them.

The notice texts below are copied from the upstream CrossMux licence set
(`licenses/borrowed-code.txt` and friends), limited to the material CPYOS
actually ships. They do not change the original licenses.

## Inventory

| Component | Where it lives in CPYOS | License (SPDX) | Upstream source |
|---|---|---|---|
| MiniBidi | `lib/MiniBidi/` | MIT | <https://github.com/mintty/mintty/blob/master/src/minibidi.c> |
| Expat | `lib/expat/` | MIT | <https://github.com/libexpat/libexpat> (notice kept in every source header) |
| EPDiy tools | font/image conversion scripts derived from epdiy | MIT | <https://github.com/vroland/epdiy/blob/main/scripts/LICENSE> |
| EPDiy font data layouts | `lib/EpdFont/EpdFontData.h` (layout names/fields changed) | LGPL-3.0 | <https://github.com/vroland/epdiy/blob/c61e9e923ce2418150d54f88cea5d196cdc40c54/src/epd_internals.h> |
| AnimatedGIF | `platformio.ini` dependency `bitbank2/AnimatedGIF#67cfaca` | Apache-2.0 | <https://github.com/bitbank2/AnimatedGIF> |
| atomic14 diy-esp32-epub-reader | early EPUB reader inspiration | MIT | <https://github.com/atomic14/diy-esp32-epub-reader> |
| JSZip | `src/network/html/js/jszip.min.js` | MIT or GPL-3.0 (dual) | <https://github.com/Stuk/jszip> (v3.10.1) |
| Pako zlib port | deflate code carried by JSZip | Zlib | <https://github.com/nodeca/pako> (1.0.11) |
| Unicode data | generated character tables | Unicode License V3 | <https://www.unicode.org/license.txt> |
| Noto Sans CJK / Noto Emoji / Source Han Sans | `assets/fonts/`, `assets/emoji/` | SIL OFL 1.1 | <https://github.com/notofonts/noto-cjk>, <https://github.com/googlefonts/noto-emoji>, <https://github.com/adobe-fonts/source-han-sans> |
| Lucide icons | `src/components/icons/` | ISC | <https://github.com/lucide-icons/lucide> |
| Hyphenation patterns | `lib/Epub/Epub/hyphenation/generated/` | per language (MIT/LPPL) | <https://github.com/typst/hypher> patterns |

Apache-2.0 and OFL-1.1 texts are in this directory. LGPL-3.0 and LPPL-1.3c are
referenced by URL above; CPYOS does not bundle a second copy of those texts.

The fonts keep their upstream copyright lines:

- Noto Sans CJK: Copyright 2014-2021 Adobe (Source Han Sans) and Google Inc.,
  SIL OFL 1.1 without Reserved Font Name in the subset used here.
- Noto Emoji: Copyright 2013 Google Inc., SIL OFL 1.1.
- Lucide: ISC, Copyright (c) for Lucide Contributors.

The hyphenation trie data is generated from the `typst/hypher` patterns
(`hyph-en-us`, `hyph-fr`, `hyph-de-1996`, `hyph-es`, `hyph-ru`, `hyph-it`,
`hyph-uk`, `hyph-pl`, `hyph-pt`, `hyph-sv`, `hyph-fi`). English, French, German,
Spanish, Italian, Polish and Portuguese patterns offer an MIT option, which
CPYOS selects; Russian, Swedish and Ukrainian patterns use LPPL-1.3c. The source
URLs and per-pattern copyright lines are preserved in the upstream notice file
`licenses/hyphenation.txt` of CrossPoint.

## MiniBidi

Source: <https://github.com/mintty/mintty/blob/master/src/minibidi.c>

Original author: Ahmad Khalifa. Mintty changes: Thomas Wolff.
MIT license.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Expat

Source: <https://github.com/libexpat/libexpat>

Copyright (c) 1997-2000 Thai Open Source Software Center Ltd
Copyright (c) 2000 Clark Cooper <coopercc@users.sourceforge.net>
Copyright (c) 2000-2005 Fred L. Drake, Jr. <fdrake@users.sourceforge.net>
Copyright (c) 2001-2002 Greg Stein <gstein@users.sourceforge.net>
Copyright (c) 2002-2016 Karl Waclawek <karl@waclawek.net>
Copyright (c) 2016-2025 Sebastian Pipping <sebastian@pipping.org>
Copyright (c) 2016 Cristian Rodríguez <crrodriguez@opensuse.org>
Copyright (c) 2016 Thomas Beutlich <tc@tbeu.de>
Copyright (c) 2017 Rhodri James <rhodri@wildebeest.org.uk>
Copyright (c) 2022 Thijs Schreijer <thijs@thijsschreijer.nl>
Copyright (c) 2023 Hanno Böck <hanno@gentoo.org>
Copyright (c) 2023 Sony Corporation / Snild Dolkow <snild@sony.com>
Copyright (c) 2024 Taichi Haradaguchi <20001722@ymail.ne.jp>
Copyright (c) 2025 Matthew Fernandez <matthew.fernandez@gmail.com>

Licensed under the MIT license:

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## EPDiy tools

Source: <https://raw.githubusercontent.com/vroland/epdiy/main/scripts/LICENSE>

MIT License

Copyright (c) 2017 Mr. PK

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## EPDiy font data layouts

Source: <https://github.com/vroland/epdiy/blob/c61e9e923ce2418150d54f88cea5d196cdc40c54/src/epd_internals.h>

The EPDiy font data layouts use LGPL-3.0. The copied layouts are in
`lib/EpdFont/EpdFontData.h`; CPYOS changes their names and fields. LGPL-3.0
section 3 covers use of data layouts from library headers.

## AnimatedGIF

Source: <https://github.com/bitbank2/AnimatedGIF>, pinned in `platformio.ini` to
`67cfacafe12ef4926c7713342c57d2a4af040e5e`. Local bound/palette fixes are in
`scripts/patch_animatedgif.py`.

Copyright 2020 BitBank Software, Inc. All rights reserved.
Licensed under the Apache License, Version 2.0 — see
[`Apache-2.0.txt`](./Apache-2.0.txt).

## atomic14 diy-esp32-epub-reader

Source: <https://raw.githubusercontent.com/atomic14/diy-esp32-epub-reader/2c2f57fdd7e2a788d14a0bcb26b9e845a47aac42/LICENSE>

MIT License

Copyright (c) 2021 Chris Greening

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## JSZip

Source: <https://raw.githubusercontent.com/Stuk/jszip/v3.10.1/LICENSE.markdown>

JSZip is dual licensed. At your choice you may use it under the MIT license or
the GPLv3 license.

The MIT License

Copyright (c) 2009-2016 Stuart Knightley, David Duponchel, Franz Buchinger, António Afonso

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.

## Pako zlib port

Source: <https://github.com/nodeca/pako/blob/1.0.11/lib/zlib/deflate.js>

Copyright (C) 1995-2013 Jean-loup Gailly and Mark Adler
Copyright (C) 2014-2017 Vitaly Puzrin and Andrey Tupitsin

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the
use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim
   that you wrote the original software. If you use this software in a
   product, an acknowledgment in the product documentation would be
   appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.

## Unicode data

Source: <https://www.unicode.org/license.txt>

UNICODE LICENSE V3

COPYRIGHT AND PERMISSION NOTICE

Copyright © 1991-2026 Unicode, Inc.

NOTICE TO USER: Carefully read the following legal agreement. BY
DOWNLOADING, INSTALLING, COPYING OR OTHERWISE USING DATA FILES, AND/OR
SOFTWARE, YOU UNEQUIVOCALLY ACCEPT, AND AGREE TO BE BOUND BY, ALL OF THE
TERMS AND CONDITIONS OF THIS AGREEMENT. IF YOU DO NOT AGREE, DO NOT
DOWNLOAD, INSTALL, COPY, DISTRIBUTE OR USE THE DATA FILES OR SOFTWARE.

Permission is hereby granted, free of charge, to any person obtaining a
copy of data files and any associated documentation (the "Data Files") or
software and any associated documentation (the "Software") to deal in the
Data Files or Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, and/or sell
copies of the Data Files or Software, and to permit persons to whom the
Data Files or Software are furnished to do so, provided that either (a)
this copyright and permission notice appear with all copies of the Data
Files or Software, or (b) this copyright and permission notice appear in
associated Documentation.

THE DATA FILES AND SOFTWARE ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY
KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
THIRD PARTY RIGHTS.

IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS INCLUDED IN THIS NOTICE
BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES,
OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THE DATA
FILES OR SOFTWARE.

Except as contained in this notice, the name of a copyright holder shall
not be used in advertising or otherwise to promote the sale, use or other
dealings in these Data Files or Software without prior written
authorization of the copyright holder.

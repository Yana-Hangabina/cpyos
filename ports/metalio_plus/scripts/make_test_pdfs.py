#!/usr/bin/env python3
"""Generate deterministic PDFs for the Plus parity scenarios.

No third-party modules: a small writer emits one content stream per page with
Helvetica text, which is enough to exercise paging, bookmarks, long-document
line breaking and render timing.

    python3 ports/metalio_plus/scripts/make_test_pdfs.py --out-dir artifacts/plus_scenarios
"""
from __future__ import annotations

import argparse
import os


def esc(text: str) -> str:
    return text.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


def page_stream(page: int, lines: list[str]) -> bytes:
    out = ["BT", "/F1 22 Tf", "50 760 Td"]
    for index, line in enumerate(lines):
        if index:
            out.append("0 -32 Td")
        out.append(f"({esc(line)}) Tj")
    out.append("ET")
    return ("\n".join(out) + "\n").encode("latin-1")


def build(path: str, pages: int, title: str) -> None:
    objects: list[bytes] = []

    def add(body: bytes) -> int:
        objects.append(body)
        return len(objects)

    font = add(b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>")
    page_ids: list[int] = []
    content_ids: list[int] = []
    for page in range(1, pages + 1):
        lines = [
            f"{title}  page {page}/{pages}",
            "The quick brown fox jumps over the lazy dog. " * 2,
            "0123456789 " * 6,
            "Bookmark target: paragraph %d." % page,
        ]
        if page % 25 == 0:
            lines.append("MILESTONE %d" % page)
        stream = page_stream(page, lines)
        content_ids.append(add(b"<< /Length %d >>\nstream\n" % len(stream) + stream + b"endstream"))
    pages_id = len(objects) + pages + 1  # reserved below
    for index, content_id in enumerate(content_ids, start=1):
        page_ids.append(add(("<< /Type /Page /Parent %d 0 R /MediaBox [0 0 612 792] "
                            "/Resources << /Font << /F1 %d 0 R >> >> /Contents %d 0 R >>"
                            % (pages_id, font, content_id)).encode()))
    pages_id = add(("<< /Type /Pages /Kids [%s] /Count %d >>"
                    % (" ".join("%d 0 R" % p for p in page_ids), pages)).encode())
    catalog = add(("<< /Type /Catalog /Pages %d 0 R >>" % pages_id).encode())

    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = [0]
    for index, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % index + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n" % (len(objects) + 1)
    out += b"0000000000 65535 f \n"
    for offset in offsets[1:]:
        out += b"%010d 00000 n \n" % offset
    out += (b"trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n"
            % (len(objects) + 1, catalog, xref))
    with open(path, "wb") as fh:
        fh.write(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out-dir", default="artifacts/plus_scenarios")
    args = ap.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)
    long_doc = os.path.join(args.out_dir, "long-300.pdf")
    build(long_doc, 300, "CPYOS long document")
    print(long_doc)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

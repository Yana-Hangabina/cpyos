#!/usr/bin/env python3
"""Never unmap the PDF engine from a PSRAM-stack task.

Unmapping external memory makes IDF flush/invalidate caches, and a task whose
stack lives in PSRAM does not survive that (this branch already produced a
Double exception from exactly that shape, AGENTS 35.6).  PdfReaderActivity's
onExit usually runs on the render task, so its unmap must sit inside an
`esp_ptr_external_ram` branch that schedules the deferred flush instead.

Checks, textual and narrow:
  1. every `pdf_module::unload()` outside the runtime itself mentions the stack
     predicate within the preceding few lines;
  2. the runtime's flush path re-checks the predicate before calling unload().
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
IMPL = "src/pdf/PdfModuleRuntime.cpp"


def main():
    failures = []
    hits = 0
    for path in sorted((ROOT / "src").rglob("*.cpp")):
        rel = str(path.relative_to(ROOT))
        text = path.read_text(errors="replace")
        lines = text.splitlines()
        for i, line in enumerate(lines):
            if "pdf_module::unload()" not in line:
                continue
            if rel == IMPL:
                continue
            hits += 1
            window = "\n".join(lines[max(0, i - 8):i + 1])
            if "esp_ptr_external_ram" not in window:
                failures.append(f"{rel}:{i + 1}: unload() with no PSRAM-stack guard nearby")

    impl = (ROOT / IMPL).read_text(errors="replace")
    m = re.search(r"bool flushScheduledUnload\([^)]*\) \{(.*?)\n\}", impl, re.S)
    if not m:
        failures.append("flushScheduledUnload is gone")
    elif "esp_ptr_external_ram" not in m.group(1):
        failures.append("flushScheduledUnload no longer checks the stack")

    if hits == 0 and not m:
        failures.append("no unload call sites found -- scanner or tree moved")

    # Same class, same lesson: the radio teardown touches the PHY and lwIP and
    # must not run from the reader's onEnter (PSRAM stack).
    act = (ROOT / "src/activities/reader/PdfReaderActivity.cpp").read_text(errors="replace")
    code = "\n".join(line.split("//", 1)[0] for line in act.splitlines())  # comments name the API too
    if "releaseRadioDeep(" in code:
        failures.append("PdfReaderActivity calls releaseRadioDeep(); schedule it instead")

    for f in failures:
        print("FAIL", f, file=sys.stderr)
    print(f"pdf unmap stack guard: {hits} call sites checked, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())

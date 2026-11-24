#!/usr/bin/env python3
"""Every wait on the synchronous PDF job must use the tested release rule.

One shipped bug motivates this: renderPdfPage cleared `gSyncJobInFlight`
unconditionally after its wait, so a TIMED-OUT render handed the slot back while
the worker was still writing into gSyncJob -- the heap corruption the ownership
rule exists to prevent (test/pdf_job_slot pins the rule itself; this pins that
nobody bypasses it).

Checks, all textual and deliberately narrow:
  1. every `waitPdfJob(job...)` call is followed by releaseSlotAfterWait(completed);
  2. `slotReturnedByWait` appears exactly once -- inside releaseSlotAfterWait;
  3. releaseSlotAfterWait does not call itself (the first draft of this very
     refactor did, which is why the check exists).
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "src/activities/reader/PdfReaderActivity.cpp"


def main():
    text = SRC.read_text()
    failures = []

    waits = [m.end() for m in re.finditer(r"waitPdfJob\s*\(\s*job", text)]
    if not waits:
        failures.append("no waitPdfJob calls found -- scanner or file moved")
    for end in waits:
        window = text[end:end + 400]
        if "releaseSlotAfterWait(completed)" not in window:
            line = text.count("\n", 0, end) + 1
            failures.append(f"wait at line {line} does not call releaseSlotAfterWait")

    if text.count("slotReturnedByWait") != 1:
        failures.append(f"slotReturnedByWait used {text.count('slotReturnedByWait')} times; it must live only in releaseSlotAfterWait")

    m = re.search(r"void releaseSlotAfterWait\(const bool completed\) \{(.*?)\n\}", text, re.S)
    if not m:
        failures.append("releaseSlotAfterWait is gone or its signature moved")
    else:
        body = m.group(1)
        if "slotReturnedByWait(completed)" not in body:
            failures.append("releaseSlotAfterWait no longer applies the tested rule")
        if "releaseSlotAfterWait(" in body:
            failures.append("releaseSlotAfterWait calls itself")

    for f in failures:
        print("FAIL", f, file=sys.stderr)
    print(f"pdf slot release: {len(waits)} waits checked, {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())

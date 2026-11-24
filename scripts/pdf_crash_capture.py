#!/usr/bin/env python3
"""Trigger one XIP PDF crash and resolve it against the ELF that is actually flashed.

Why this exists: the 2026-11-19 crash left a perfect register block in the log
(PC in app text, EXCVADDR 0x0000cdcd, a live ROM loop) and an `ELF file SHA256:
5dc4b1652` that matched nothing on the machine, because the build directory had
already been reused.  Every address in that report was therefore unresolvable.
This script closes the loop: it flashes nothing, it *reads* the panic, and it
refuses to symbolize unless the ELF in hand hashes to the tag the device printed.

Usage (with the device on a console port):
  ~/.platformio/penv/bin/python scripts/pdf_crash_capture.py --port /dev/cu.usbmodem1101
  ~/.platformio/penv/bin/python scripts/pdf_crash_capture.py --cmd "PDFBIG 19" --seconds 120
  ~/.platformio/penv/bin/python scripts/pdf_crash_capture.py --self-test

Output goes to artifacts/pdf-crash/<stamp>/ : the raw log, the parsed fields, and
addr2line output for every frame.  Nothing from the PDF itself is ever printed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "build" / "readpico_pdf"
OUT = ROOT / "artifacts" / "pdf-crash"
MODULE_ELF = ROOT / "build" / "pdfmod.elf"
# kPdfModuleCodeVaddr / kPdfModuleRodataVaddr (src/pdf/PdfModuleApi.h).  Hard
# numbers here on purpose: they are the only way to know which ELF an address
# belongs to, and resolving a module address with the app ELF silently returns a
# plausible function name that is simply someone else's code.
CODE_VADDR = 0x42D30000
CODE_SPAN = 0x100000
RODATA_VADDR = 0x3DF00000
TOOLCHAIN = Path.home() / ".platformio/packages/toolchain-xtensa-esp-elf/bin/xtensa-esp32s3-elf-"

FRAME_RE = re.compile(r"^(0x[0-9a-fA-F]{8}):(0x[0-9a-fA-F]{8})\s*$")
REG_RE = re.compile(r"^\s*(PC|EXCVADDR|A\d+|PS|LBEG|LEND|LCOUNT|SAR|EXCCAUSE)\s*:\s*(0x[0-9a-fA-F]+)")
TAG_RE = re.compile(r"ELF file SHA256:\s*([0-9a-fA-F]+)")


def elf_tag(elf: Path) -> str:
    """Full SHA-256 of the ELF -- what esptool stores at image offset 0xb0.

    The panic handler prints only a prefix of it (observed as 9 hex digits), and
    that truncation length is a ROM detail, not a contract.  Keep the whole
    digest and match by prefix instead of assuming a width.
    """
    return hashlib.sha256(elf.read_bytes()).hexdigest()


def tag_matches(elf_full: str, printed: str) -> bool:
    """True when the printed panic tag is a prefix of this ELF's digest.

    Refusing on a real mismatch is the point of this script; refusing on a
    length difference would just hide the evidence.  Six digits is enough to
    rule out coincidence and is shorter than anything observed from the device.
    """
    printed = (printed or "").lower().lstrip("0x") if len(printed or "") < 8 else (printed or "").lower()
    if len(printed) < 6:
        return False
    return elf_full.startswith(printed)


def frame_owner(addr: int) -> str:
    """Which ELF an address belongs to: 'module' or 'app'."""
    if CODE_VADDR <= addr < CODE_VADDR + CODE_SPAN or RODATA_VADDR <= addr < RODATA_VADDR + CODE_SPAN:
        return "module"
    return "app"


def parse_console(text: str) -> dict:
    """Extract the panic block. Deterministic, so it is testable without a device.

    A `Backtrace:` line alone is NOT a crash: the PDF allocator prints one on
    purpose via esp_backtrace_print() when a large PSRAM allocation fails
    (src/pdf/PdfDocument.cpp, "large realloc %u failed; caller backtrace").  Calling
    that a panic would send whoever reads it looking for a bug that is not there,
    so a panic requires the ROM's own preamble.
    """
    out: dict = {"frames": [], "regs": {}, "elfTag": None, "panic": False}
    panicked = bool(re.search(r"Guru Meditation|abort\(\)|assert failed|Unhandled debug exception", text))
    out["panic"] = panicked
    for line in text.splitlines():
        m = REG_RE.match(line)
        if m:
            out["regs"][m.group(1)] = m.group(2)
        m = TAG_RE.search(line)
        if m:
            out["elfTag"] = m.group(1).lower()
        if "Backtrace:" in line:
            for chunk in line.split("Backtrace:")[1].split():
                fm = FRAME_RE.match(chunk.rstrip(","))
                if fm:
                    out["frames"].append({"pc": fm.group(1), "sp": fm.group(2)})
    if out["frames"] and not panicked:
        # Deliberate diagnostic, not a crash.  Keep the frames (they are useful)
        # but mark it, and never let it through the "we caught a panic" path.
        out["diagnosticOnly"] = True
    if out["frames"] and panicked and not out["elfTag"]:
        # A backtrace without a tag is exactly the trap this script exists to avoid.
        raise SystemExit("backtrace found but no 'ELF file SHA256:' line; cannot trust symbolization")
    return out


def resolve(frames: list[dict], app_elf: Path, module_elf: Path | None = None) -> str:
    """addr2line each frame against the ELF that actually owns its address."""
    if not frames:
        return "<no frames>"
    if module_elf is None:
        module_elf = MODULE_ELF
    lines = []
    groups: dict[str, list[str]] = {}
    for f in frames:
        groups.setdefault(frame_owner(int(f["pc"], 16)), []).append(f["pc"])
    if groups.get("app"):
        lines.append(f"-- app frames ({app_elf.name}) --")
        lines.append(_addr2line(app_elf, groups["app"]))
    if groups.get("module"):
        if module_elf.is_file():
            lines.append(f"-- module frames ({module_elf.name}) --")
            lines.append(_addr2line(module_elf, groups["module"]))
        else:
            # Guessing with the app ELF is worse than saying nothing: it prints a
            # confident, wrong function name for every MuPDF frame.
            lines.append("-- module frames: REFUSED, %s missing "
                         "(run scripts/build_pdf_module.py to regenerate it) --" % module_elf)
    return "\n".join(lines)


def _addr2line(elf: Path, addrs: list[str]) -> str:
    # -f inline: MuPDF/host services are heavily inlined; the function chain is the
    # difference between "an app function dereferenced 0xcdcd" and a usable answer.
    cmd = [str(TOOLCHAIN) + "addr2line", "-f", "-C", "-e", str(elf)] + addrs
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout


def self_test() -> int:
    vectors = {
        "panic block parses": (
            "Guru Meditation Error: Core  0 panic'ed (LoadProhibited).\n"
            "Core  0 register dump:\n"
            "PC      : 0x42309ee2  PS      : 0x00060230  A0      : 0x82306fc4  A1      : 0x3c000000\n"
            "A2      : 0x0000cdcd  A3      : 0x00000000  A7      : 0x0000cdcd\n"
            "EXCVADDR: 0x0000cdcd  LBEG    : 0x400570e8  LEND    : 0x400570f3  LCOUNT  : 0xffffffff\n"
            "Backtrace: 0x42309edf:0x3c0000a0 0x42306fc1:0x3c0000c0 0x4224d769:0x3c000110\n"
            "ELF file SHA256: deadbeef1\n"
        ),
    }
    text = vectors["panic block parses"]
    got = parse_console(text)
    assert got["regs"].get("PC") == "0x42309ee2", got["regs"]
    assert got["regs"].get("EXCVADDR") == "0x0000cdcd", got["regs"]
    assert got["elfTag"] == "deadbeef1", got["elfTag"]
    assert [f["pc"] for f in got["frames"]] == ["0x42309edf", "0x42306fc1", "0x4224d769"], got["frames"]

    # Mutation 1: drop the tag line -> the parser must refuse rather than symbolize blind.
    try:
        parse_console("\n".join(l for l in text.splitlines() if "ELF file" not in l))
        print("MUTATION NOT DETECTED: missing tag was accepted")
        return 1
    except SystemExit:
        print("ok  missing 'ELF file SHA256' is refused")

    # Mutation 2: backtrace formatting drift must not silently yield zero frames.
    mangled = text.replace("Backtrace: 0x42309edf:0x3c0000a0", "Backtrace: ???")
    if parse_console(mangled)["frames"] != ["0x42306fc1", "0x4224d769"] and len(
            parse_console(mangled)["frames"]) != 2:
        print("MUTATION NOT DETECTED: frame loss was not observable")
        return 1
    print("ok  a mangled frame drops out instead of poisoning the list")

    # The real 2026-11-20 case: a large-realloc diagnostic prints a backtrace with
    # module addresses.  It must NOT be classified as a panic.
    diag = ("[20672] [ERR] [PDF] PSRAM realloc 463911 failed: free=203520 largest=196596\n"
            "[PDF] large realloc 463911 failed; caller backtrace:\n"
            "Backtrace: 0x42D30C5D:0x3CBBF150 0x42D58C51:0x3CBBF170\n")
    d = parse_console(diag)
    assert d["panic"] is False, "allocation diagnostic mistaken for a panic"
    assert len(d["frames"]) == 2, d["frames"]
    print("ok  an allocation-failure backtrace is not called a panic")

    # Mutation: prefix the same text with the ROM preamble -> now it IS a panic and
    # the parser must demand the ELF tag.
    try:
        parse_console("Guru Meditation Error: Core  0 panic'ed (LoadProhibited).\n" + diag)
        print("MUTATION NOT DETECTED: panic without a tag was accepted")
        return 1
    except SystemExit:
        print("ok  a real panic still refuses to resolve without its ELF tag")

    # Address ownership: module frames belong to pdfmod.elf, and resolving them
    # with the app ELF would print a confident wrong answer.
    assert frame_owner(0x42D30C5D) == "module", frame_owner(0x42D30C5D)
    assert frame_owner(0x42309EE2) == "app", frame_owner(0x42309EE2)
    assert frame_owner(0x3DF00010) == "module", frame_owner(0x3DF00010)
    print("ok  module and app addresses are routed to different ELFs")
    print("self-test: PASS")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--cmd", default="PDFBIG 19",
                    help="serial CMD to send; 'PDFBIG <page>' opens the engine at that page")
    ap.add_argument("--seconds", type=float, default=180.0)
    ap.add_argument("--elf", default=str(BUILD / "firmware.elf"))
    ap.add_argument("--no-send", action="store_true", help="just listen")
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--replay", help="resolve an already captured console.log instead of driving the device")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if args.replay:
        text = Path(args.replay).read_text(errors="replace")
        got = parse_console(text)
        if not got["frames"]:
            print("no frames in that file")
            return 2
        kind = "panic" if got["panic"] else "allocation-failure diagnostic"
        print(f"{args.replay}: {kind}, {len(got['frames'])} frames")
        print(resolve(got["frames"], Path(args.elf)))
        return 0
    if not args.port:
        raise SystemExit("--port is required (or use --self-test / --replay)")

    elf = Path(args.elf)
    if not elf.is_file():
        raise SystemExit(f"missing {elf}; build -e readpico_pdf first")
    tag = elf_tag(elf)  # full digest

    import serial  # provided by the PlatformIO python, not the system one

    stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime()) + "Z"
    out = OUT / stamp
    out.mkdir(parents=True, exist_ok=True)

    collected: list[str] = []
    with serial.Serial(args.port, 115200, timeout=1) as port:
        if not args.no_send:
            port.write((f"CMD:{args.cmd}\n").encode())
            port.flush()
        deadline = time.time() + args.seconds
        while time.time() < deadline:
            raw = port.readline()
            if not raw:
                continue
            line = raw.decode("utf-8", "replace")
            collected.append(line)
            # Only echo what the crash analysis needs. Document contents, paths and
            # any credential-shaped text never leave the device.
            if re.search(r"Guru Meditation|register dump|Backtrace:|ELF file SHA256|\[PDFM\]|\[PDF\]", line):
                print(line.rstrip())
            if "ELF file SHA256" in line:
                break
    text = "".join(collected)
    (out / "console.log").write_text(text)

    got = parse_console(text)
    if not got["frames"]:
        print(f"\nno backtrace captured within {args.seconds:.0f} s; console kept in {out / 'console.log'}")
        return 2
    if not got["panic"]:
        # A large-PSRAM-allocation diagnostic, not a crash.  Report it as what it
        # is and resolve it -- it still names the call site that could not get
        # memory, which is the useful part.
        print("\nnot a panic: this was an allocation-failure backtrace (see "
              "src/pdf/PdfDocument.cpp). Resolving the call site:")
        print(resolve(got["frames"], elf))
        (out / "crash.json").write_text(json.dumps(
            {"panic": False, "diagnosticOnly": True, "frames": got["frames"],
             "resolution": resolve(got["frames"], elf)}, indent=2) + "\n")
        print(f"kept in {out}")
        return 0
    match = tag_matches(tag, got["elfTag"])
    print(f"\npanic printed tag = {got['elfTag']}")
    print(f"ELF on hand        = {tag[:12]}...  -> {'MATCH' if match else 'MISMATCH'}")
    report = {"elf": str(elf), "elfSha256": tag, "printedTag": got["elfTag"],
              "matched": match, "regs": got["regs"], "frames": got["frames"]}
    if not match:
        # This is the whole point of the script: do not hand out addresses resolved
        # against the wrong image -- they look authoritative and are fiction.
        report["resolution"] = "refused: ELF does not match the panic tag"
        (out / "crash.json").write_text(json.dumps(report, indent=2) + "\n")
        print("REFUSING to symbolize: find the ELF whose tag matches (see "
              "artifacts/pdf-xip/flashes/latest/manifest.json)")
        return 3
    report["resolution"] = resolve(got["frames"], elf)
    print("\n" + report["resolution"])
    (out / "crash.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"kept in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

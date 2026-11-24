#!/usr/bin/env python3
"""Every download call site must hold the CPU, itself or through its owner.

The OTA path once measured ~4 KB/s because HalPowerManager drops to 80 MHz after
three idle seconds and a download generates no input of its own.  This test finds
every HttpDownloader transfer call and enforces one of three classifications:

  in-file  : a HalPowerManager::Lock must be in the enclosing function;
  upstream : the transfer runs on a worker/activity that holds it, named here so
             the claim is checkable (the owner file must still contain a lock);
  impl     : the transfer implementation itself.

A new download screen therefore cannot ship unguarded, and removing one of the
existing locks turns this red.  Audited 2026-11-21.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
# HttpDownloader transfers plus the plugin-http ones: the plugin catalog page
# and plugin events fetch feeds/API hops through pluginhttp, which the first
# scan missed entirely (a page was waiting on them at 80 MHz).
CALL_RE = re.compile(
    r"(?:HttpDownloader::(?:downloadToFile|fetchResumable|fetchUrl)|pluginhttp::(?:requestToFile|requestStream|request))\s*\(")
DEF_RE = re.compile(r"^(?:[A-Za-z_][\w:<>,&\* ]*\s+)?(?:[\w:]+::)?[~\w]+\s*\(")

UPSTREAM = {
    "src/FontExtras.cpp": "src/activities/settings/FontExtrasActivity.cpp",
    "src/network/OtaUpdater.cpp": "src/activities/settings/OtaUpdateActivity.cpp",
    "src/pdf/PdfEngineUpdate.cpp": "src/activities/settings/PdfEngineUpdateActivity.cpp",
    "src/plugins/PluginCatalog.cpp": "src/activities/apps/plugins/PluginsActivity.cpp",
}
IMPL = {"src/network/HttpDownloader.cpp", "src/util/PluginHttp.cpp"}

# Transfers that go through freeink::SecureHttpClient directly (the scanner's
# call patterns do not see them), keyed to the functions that must hold a lock.
SECURE_HTTP_ENTRIES = {
    "lib/KOReaderSync/KOReaderSyncClient.cpp": ["authenticate", "createUser", "getProgress", "updateProgress"],
}


def enclosing(lines, index):
    start = index
    while start > 0 and not (DEF_RE.match(lines[start]) and not lines[start].rstrip().endswith(";")):
        start -= 1
    end = index
    while end < len(lines) - 1 and lines[end].rstrip() != "}":
        end += 1
    name = re.sub(r".*?([\w~]+)\s*\(.*", r"\1", lines[start].strip())
    return "\n".join(lines[start : end + 1]), name


def main():
    failures, in_file_ok, upstream_ok, impl_ok = [], 0, 0, 0
    for path in sorted((ROOT / "src").rglob("*.cpp")):
        rel = str(path.relative_to(ROOT))
        lines = path.read_text(errors="replace").splitlines()
        for idx, line in enumerate(lines):
            if not CALL_RE.search(line):
                continue
            body, name = enclosing(lines, idx)
            where = f"{rel}:{name} (line {idx + 1})"
            if rel in IMPL:
                impl_ok += 1
            elif rel in UPSTREAM:
                owner = ROOT / UPSTREAM[rel]
                if "HalPowerManager::Lock" not in owner.read_text(errors="replace"):
                    failures.append(f"{where}: owner {UPSTREAM[rel]} no longer holds a lock")
                upstream_ok += 1
            else:
                if "HalPowerManager::Lock" not in body:
                    failures.append(f"{where}: no HalPowerManager::Lock in scope")
                in_file_ok += 1
    for rel, funcs in SECURE_HTTP_ENTRIES.items():
        path = ROOT / rel
        if not path.is_file():
            failures.append(f"{rel}: file moved")
            continue
        lines = path.read_text(errors="replace").splitlines()
        for func in funcs:
            hit = next((i for i, l in enumerate(lines) if f"KOReaderSyncClient::{func}(" in l), None)
            if hit is None:
                failures.append(f"{rel}: {func}() not found")
                continue
            body = "\n".join(lines[hit:hit + 30])
            if "HalPowerManager::Lock" not in body:
                failures.append(f"{rel}:{func} sends without a HalPowerManager::Lock")
    if not (in_file_ok or upstream_ok or impl_ok):
        failures.append("no download call sites found -- scanner or tree moved")
    for f in failures:
        print("FAIL", f, file=sys.stderr)
    print(f"download power locks: {in_file_ok} in-file, {upstream_ok} upstream, "
          f"{impl_ok} impl sites; {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())

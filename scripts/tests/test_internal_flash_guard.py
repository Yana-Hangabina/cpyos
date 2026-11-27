#!/usr/bin/env python3
"""Machine gate for the PSRAM-stack / internal-flash rule.

The rule (docs/engineering/testing-and-debugging.md, "两类现场崩溃"):
an operation that erases or programs internal SPI flash runs with the cache
disabled.  A task whose stack lives in PSRAM therefore loses its own stack, and
ESP-IDF reacts with a panic, not an error:

    assert failed: spi_flash_disable_interrupts_caches_and_one_cpu
        cache_utils.c:127 (esp_task_stack_is_sane_cache_disabled())

That is one of the two 0.8.0 -> 0.8.2 field crashes.  The render task
(`ActivityManagerRender`, 64 KB PSRAM stack because the CFF interpreter needs a
16 KB frame) executes `Activity::onEnter()`/`onExit()` as well as rendering, so
every lifecycle hook is on that stack too.

The decision belongs to exactly one owner, `memory::guardInternalFlash()`
(lib/Memory/StackSafety.{h,cpp}).  This gate enforces three things over the real
sources:

  1. every internal-flash call site in the shared tree (src/, lib/) sits in a
     function that asks the owner first -- a new read/write site is a field
     crash, so there are no per-site exceptions;
  2. any file-local alias of the predicate (the `stackIsInPsram()` helpers the
     tree already has) delegates to the owner instead of re-deriving the rule --
     a re-forked predicate is how the answer silently diverges per file;
  3. a refused flash *read* must mean "unknown", never "absent" -- the gate
     checks that the sites which cache a probe result ask the guard before
     caching (SystemCoreFont::probe/ready), because "language pack missing" and
     "font carrier missing" are reported to the user from those cached answers.

The checker runs against synthetic fixtures too (one guarded, one not), and the
real-tree scan has a floor, so a regex that quietly stops matching cannot report
a green over zero files.

Ports (ports/metalio_plus) are reported but not gated here: they contain
vendored Arduino wrappers whose whole job is NVS (Preferences, BLE bond
storage), and guarding the wrapper would switch off settings persistence on
that device.  The Plus task-stack audit is tracked as follow-up work; the
reported number is asserted to stay non-zero so this text cannot rot into
"nothing there".

Run: python3 scripts/tests/test_internal_flash_guard.py
"""
import os
import pathlib
import re
import unittest

# A release tree is checked with the same gate before packaging:
#   CPYOS_GUARD_ROOT=../cpyos-public python3 -m unittest scripts.tests.test_internal_flash_guard
# That is what makes "the next release cannot bring the unguarded read back" a
# machine check rather than a promise.
ROOT = pathlib.Path(os.environ.get("CPYOS_GUARD_ROOT") or pathlib.Path(__file__).resolve().parents[2]).resolve()
SHARED_ROOTS = ("src", "lib")
PORT_ROOT = "ports/metalio_plus"

# Operations that erase/program internal flash, or read it through a path IDF
# documents as cache-sensitive on this target.
FLASH_CALL = re.compile(
    r"\b("
    r"esp_partition_read|esp_partition_write|esp_partition_erase_range|"
    r"esp_ota_begin|esp_ota_write|esp_ota_end|esp_ota_abort|"
    r"esp_ota_set_boot_partition|esp_ota_mark_app_valid_cancel_rollback|esp_ota_rollback"
    r")\s*\("
)
# Informational only: the Plus port reaches internal flash through the Arduino
# NVS wrappers, which this gate deliberately does not police (see the port test).
NVS_CALL = re.compile(r"\b(nvs_open\w*|nvs_set_\w+|nvs_get_\w+|nvs_commit|nvs_erase_\w+)\s*\(")

# The owner, or a file-local alias of it (rule 2 keeps aliases honest).
# Every form now carries the site code, so the patterns match a call with any
# argument list: `internalFlashAllowed(fault::Code::SlotReadRefused, length)`.
GUARD_CALL = re.compile(r"internalFlashAllowed\(|guardInternalFlash\(|stackIsInPsram\(")
# A local predicate that must delegate, not reimplement.
ALIAS_DEF = re.compile(r"^\s*(?:static\s+)?bool\s+stackIsInPsram\([^)]*\)\s*(?:\{|\()", re.MULTILINE)
OWNER_TOKEN = re.compile(r"memory::(internalFlashAllowed|guardInternalFlash)\(")

COMMENT_AND_STRINGS = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)

# Excluded: host stubs/tests (they fake these symbols), and the owner itself.
SKIP_PATH_PARTS = ("stubs", "test")
OWNER_FILE = "lib/Memory/StackSafety.cpp"

# Sites whose refusal semantics must not be cached, checked by rule 3.
NO_CACHE_ON_REFUSAL = (
    ("src/SystemCoreFont.cpp", "bool probe() {", "g_probed"),
    ("src/SystemCoreFont.cpp", "bool ready() {", "g_readyChecked"),
)

# Floor for the real-tree scan (measured 42 as of 2026-11-26).
MIN_SHARED_SITES = 30
# Floor for the lifecycle scan (measured 191 `*::onEnter/onExit` definitions).
MIN_LIFECYCLE_BODIES = 150


def blank(match):
    return " " * len(match.group(0))


def strip_noise(text):
    """Comments and literals become spaces, so length and line numbers survive."""
    return COMMENT_AND_STRINGS.sub(blank, text)


def function_spans(text):
    """(start, end) of every top-level brace block, i.e. each function body."""
    spans = []
    depth = 0
    start = None
    for index, char in enumerate(text):
        if char == "{":
            depth += 1
            if depth == 1:
                start = index
        elif char == "}":
            if depth == 1 and start is not None:
                spans.append((start, index + 1))
            depth -= 1
    return spans


def enclosing(spans, position):
    for start, end in spans:
        if start <= position < end:
            return start, end
    return None


def violations(clean):
    """[(line, call)] for flash sites with no guard in their own function body.

    Takes the comment-stripped text: callers strip once per file, because the
    span walk below is linear in the file and this gate runs over the whole tree.
    """
    spans = function_spans(clean)
    found = []
    for match in FLASH_CALL.finditer(clean):
        span = enclosing(spans, match.start())
        body = clean[span[0]:span[1]] if span else ""
        if GUARD_CALL.search(body):
            continue
        found.append((clean[:match.start()].count("\n") + 1, match.group(1)))
    return found


def alias_delegations(text):
    """For each local `stackIsInPsram()` definition: does its body call the owner?"""
    clean = strip_noise(text)
    results = []
    for match in ALIAS_DEF.finditer(clean):
        end = clean.find("}", match.start())
        body = clean[match.start():end if end != -1 else len(clean)]
        results.append((clean[:match.start()].count("\n") + 1, bool(OWNER_TOKEN.search(body))))
    return results


def collect(root_names):
    files = {}
    for name in root_names:
        for path in sorted((ROOT / name).rglob("*.cpp")):
            rel = str(path.relative_to(ROOT))
            if rel == OWNER_FILE:
                continue
            if any(part in SKIP_PATH_PARTS for part in path.parts):
                continue
            files[rel] = path.read_text(errors="ignore")
    return files


# --- lifecycle hooks (Activity::onEnter/onExit) ------------------------------------------
#
# A hook can run on the render task, whose stack is in PSRAM: ActivityManager
# unlocks the render lock and calls onEnter() with the *worker* still holding it
# for the caller (see docs/engineering/testing-and-debugging.md), and onExit()
# runs while the caller holds it.  So the two things a hook must never do are the
# two things that crash that task: touch internal flash, and take a throwing
# allocation (this build is -fno-exceptions, so a failed `new` aborts).
LIFECYCLE_DEF = re.compile(r"\b\w[\w:<>~]*(?:Activity|Registry|Manager|Loader|Host|Controller|Installer|Store)?"
                           r"::(?:onEnter|onExit)\(\)\s*(?:const\s*)?\{")
LIFECYCLE_INLINE = re.compile(r"\b(?:void|override)\s+(onEnter|onExit)\s*\(\)[^;{]*\{")
# Allocations that abort instead of returning null. makeUniqueNoThrow and
# `new (std::nothrow)` are the tree's non-throwing forms and are allowed.
THROWING_ALLOC = re.compile(r"(?<!nothrow\)\s)\bnew\s+(?!.*nothrow)(?:char|uint8_t|int|float|double|auto|"
                            r"[A-Za-z_]\w*\s*\[|[A-Za-z_]\w*\*?\s*\()")
PLAIN_NEW = re.compile(r"\bnew\s+[A-Za-z_][\w:<>,*\s]*[\[(]")
NOTHROW_NEARBY = re.compile(r"nothrow")
THROWING_HELPERS = re.compile(r"\b(std::make_unique|makeUnique)\s*<")
MALLOC_LIKE = re.compile(r"\b(malloc|calloc|realloc)\s*\(")


def body_at(text, brace_index):
    """The text of the brace block opening at `brace_index`, inclusive."""
    depth = 0
    for index in range(brace_index, len(text)):
        char = text[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[brace_index:index + 1]
    return text[brace_index:]


def lifecycle_bodies(texts):
    """[(rel, line, body)] for every Activity lifecycle definition in the tree."""
    found = []
    for rel, raw in texts.items():
        clean = strip_noise(raw)
        for match in LIFECYCLE_DEF.finditer(clean):
            brace = clean.index("{", match.end() - 1)
            found.append((rel, clean[:match.start()].count("\n") + 1, body_at(clean, brace)))
    return found


def alloc_or_flash_in(body):
    """Why a lifecycle body is unsafe, or None."""
    flash = FLASH_CALL.search(body) or NVS_CALL.search(body)
    if flash:
        return f"internal flash ({flash.group(1)})"
    if MALLOC_LIKE.search(body):
        return "malloc/calloc (throwing, and not even a C++ type)"
    for match in PLAIN_NEW.finditer(body):
        window = body[max(0, match.start() - 30):match.end() + 30]
        if NOTHROW_NEARBY.search(window):
            continue
        return "bare operator new (aborts under -fno-exceptions)"
    for match in THROWING_HELPERS.finditer(body):
        window = body[max(0, match.start() - 30):match.end() + 40]
        if "NoThrow" in window:
            continue
        return f"{match.group(1)} (throwing allocation)"
    return None


def helpers_defined_in(body):
    """Names of functions defined right after the hook in the same body text (none today)."""
    return re.findall(r"\b([a-z]\w*)\s*\([^;]*\)\s*(?:const\s*)?\{", body)


class InternalFlashGuardTest(unittest.TestCase):
    def test_checker_itself_is_not_vacuous(self):
        """A gate that matches nothing is worse than no gate: prove both verdicts."""
        guarded = """
void readThing() {
  if (!memory::internalFlashAllowed(fault::Code::SlotReadRefused)) return;
  esp_partition_read(partition, 0, buffer, 10);
}
"""
        unguarded = """
void readThing() {
  esp_partition_read(partition, 0, buffer, 10);
}
"""
        self.assertEqual(violations(strip_noise(guarded)), [], "a guarded site must not be reported")
        reported = violations(strip_noise(unguarded))
        self.assertEqual(len(reported), 1, f"an unguarded site must be reported, got {reported}")
        self.assertEqual(reported[0][1], "esp_partition_read")

        # The guard may sit anywhere in the same function, including after an
        # early-return chain, but not in a *different* function.
        two_functions = unguarded + """
void other() {
  if (!memory::internalFlashAllowed(fault::Code::SlotReadRefused)) return;
}
"""
        self.assertEqual(len(violations(strip_noise(two_functions))), 1, "a guard in another function must not count")

        # Comments and string literals must not be counted as sites.
        commented = "// esp_partition_read(p, 0, b, 1);\nconst char* s = \"esp_partition_read(x);\";\n"
        self.assertEqual(violations(strip_noise(commented)), [], "a commented-out or quoted call is not a site")

        # A file-local alias that reimplements the rule must be caught.
        good_alias = "bool stackIsInPsram(const fault::Code site) {\n  return !memory::internalFlashAllowed(site); }\n"
        bad_alias = "bool stackIsInPsram() { return esp_ptr_external_ram(esp_cpu_get_sp()); }\n"
        self.assertEqual(alias_delegations(good_alias), [(1, True)])
        self.assertEqual(alias_delegations(bad_alias), [(1, False)])

    def test_every_shared_flash_site_asks_the_guard(self):
        files = collect(SHARED_ROOTS)
        total = 0
        problems = []
        for rel, text in files.items():
            clean = strip_noise(text)
            sites = FLASH_CALL.findall(clean)
            total += len(sites)
            if not sites and "stackIsInPsram" not in clean:
                continue  # the common case: nothing in this file can break the rule
            for line, call in violations(clean):
                problems.append(f"{rel}:{line}: {call}() without memory::internalFlashAllowed()")
            for line, delegates in alias_delegations(clean):
                if not delegates:
                    problems.append(f"{rel}:{line}: stackIsInPsram() does not delegate to memory::")
        self.assertEqual(problems, [], "\n".join(problems))
        self.assertGreaterEqual(total, MIN_SHARED_SITES,
                                f"only {total} flash sites found; the checker has stopped matching real code")

    def test_refused_reads_are_not_cached_as_absent(self):
        """Rule 3: a PSRAM-stack refusal must never be stored as "no data"."""
        for rel, function, cache_token in NO_CACHE_ON_REFUSAL:
            text = strip_noise((ROOT / rel).read_text(errors="ignore"))
            start = text.index(function)
            body = text[start:text.index("\n}", start)]
            guard = body.find("memory::internalFlashAllowed")
            self.assertNotEqual(guard, -1, f"{rel}::{function} must consult the guard")
            cache = body.find(cache_token, guard)
            self.assertNotEqual(cache, -1, f"{rel}::{function} must cache something after the guard")
            self.assertGreater(cache, guard,
                               f"{rel}::{function}: the cache write must come after the refusal, "
                               "or a refused read becomes a permanent 'missing' for the whole boot")

    def test_lifecycle_hooks_never_touch_flash_or_allocate(self):
        """`Activity::onEnter()`/`onExit()` may run on the PSRAM-stacked render task."""
        bodies = lifecycle_bodies(collect(list(SHARED_ROOTS) + [PORT_ROOT]))
        self.assertGreaterEqual(len(bodies), MIN_LIFECYCLE_BODIES,
                                f"only {len(bodies)} lifecycle definitions found; the checker stopped matching")
        problems = []
        for rel, line, body in bodies:
            reason = alloc_or_flash_in(body)
            if reason:
                problems.append(f"{rel}:{line}: {reason}")
        self.assertEqual(problems, [], "\n".join(problems))

    def test_lifecycle_classifier_is_not_vacuous(self):
        """Each direction of the rule is proven on its own, so neither can be dead."""
        unsafeAlloc = """
void FontActivity::onEnter() {
  auto buffer = new char[4096];
  std::vector<int> sizes;
  run();
}
"""
        unsafeFlash = """
void StoreActivity::onExit() {
  nvs_open("store", NVS_READWRITE, &handle);
}
"""
        safe = """
void FontActivity::onEnter() {
  auto buffer = makeUniqueNoThrow<char[]>(4096);
  Storage.openFileForRead("FNT", path, file);
  new (std::nothrow) int(3);
}
"""
        guardedFlash = """
void FontActivity::onExit() {
  if (memory::internalFlashAllowed()) esp_partition_read(partition, 0, buffer, 4096);
}
"""

        def reasons(text):
            clean = strip_noise(text)
            out = []
            for match in LIFECYCLE_DEF.finditer(clean):
                reason = alloc_or_flash_in(body_at(clean, clean.index("{", match.end() - 1)))
                if reason:
                    out.append(reason)
            return out

        self.assertEqual(reasons(unsafeAlloc), ["bare operator new (aborts under -fno-exceptions)"],
                         f"a throwing allocation in a hook must be named, got {reasons(unsafeAlloc)}")
        self.assertEqual(reasons(unsafeFlash), ["internal flash (nvs_open)"],
                         f"an NVS write in a hook must be named, got {reasons(unsafeFlash)}")
        self.assertEqual(reasons(safe), [], f"a safe hook was reported: {reasons(safe)}")
        # Deliberately stricter than the call-site rule: a guarded flash read in a
        # hook still answers "unknown" half the time, because which stack runs the
        # hook depends on who opened the Activity. Hooks do their flash work in a
        # named method the caller runs on a task it chose.
        self.assertEqual(reasons(guardedFlash), ["internal flash (esp_partition_read)"],
                         f"a guarded flash call in a hook is still reported, got {reasons(guardedFlash)}")

    def test_plus_port_wrappers_are_reported_not_silently_ignored(self):
        """The Plus NVS wrappers stay a known, counted gap -- not an invisible one."""
        sites = 0
        files = 0
        for path in sorted((ROOT / PORT_ROOT).rglob("*.cpp")):
            text = strip_noise(path.read_text(errors="ignore"))
            count = len(FLASH_CALL.findall(text)) + len(NVS_CALL.findall(text))
            if count:
                files += 1
                sites += count
        self.assertGreater(sites, 0, "the Plus port scan found nothing; this report would be meaningless")
        print(f"[info] {PORT_ROOT}: {sites} internal-flash/NVS sites in {files} files "
              f"(vendored Arduino wrappers; the Plus task-stack audit is follow-up work)")


if __name__ == "__main__":
    unittest.main(verbosity=2)

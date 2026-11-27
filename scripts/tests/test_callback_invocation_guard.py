#!/usr/bin/env python3
"""Machine gate for callback invocation: no bare `std::function` call in shipped code.

The other field crash of the 0.8.0 -> 0.8.2 reports was

    abort() was called at PC 0x423569f3 on core 1

with no backtrace.  In the released image that PC is
`_ZN10__cxxabiv111__terminateEPFvvE` -- `std::terminate`.  The build uses
-fno-exceptions, so libstdc++'s `std::bad_function_call` has no landing pad in any
application frame: calling an empty `std::function` aborts and reboots the device.
It is not catchable, not warnable-at-compile-time here, and it happens on the
normal path -- theme row formatters default to `nullptr`, so a list page that is
drawn before its host wired the callbacks dies on the first row instead of showing
an empty column.

The owner is `callbacks::call` / `callbacks::callOr` (lib/Callbacks/InvokeCallback.h);
the host regression is test/invoke_callback/.  This gate stops new sites:

  * every invocation of a `std::function` object known in that file must either be
    wrapped by the owner, or sit in a scope that tests the same name for null
    first (`if (fn)`, `fn != nullptr`, `fn ? ... : ...`), or be non-null by
    construction (initialised with a lambda in the same scope);
  * exemptions are a named table with a reason each, and the gate refuses to pass
    when an exemption no longer matches anything -- an outdated exemption is a
    hole, not a preference.

Scope resolution is per declaration (a name is only a callback where a
`std::function<...>` (or a local `using` alias of one) declares it), because the
obvious shortcut -- matching known callback *words* anywhere -- reports 1,284
false positives on `requestUpdate()`, `value()` and `title()`, which are member
functions, not callbacks.  The self-test below pins that distinction.

Run: python3 scripts/tests/test_callback_invocation_guard.py
"""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCAN_ROOTS = ("src", "lib")
SKIP_PARTS = ("test", "stubs")

# A word in front of `name()` that means this is a statement, not a prototype.
KEYWORDS_BEFORE_CALL = frozenset({"return", "if", "else", "while", "for", "do", "case", "and", "or", "not", "throw",
                                  "co_await", "co_yield", "delete", "new", "static_cast", "assert"})

COMMENT_AND_STRINGS = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
ALIAS_DECL = re.compile(r"using\s+(\w+)\s*=\s*std::function<")
# `std::function<...> name` / `const std::function<...>& name` / `std::function<...>* name`
DECL_AFTER_FUNCTION = re.compile(r"\s*(?:const\s*)?[&*]?\s*([A-Za-z_]\w*)")

# Where a callback name is legitimately empty-but-checked, or non-null by
# construction, and the owner would only add noise. Keyed (relative path, name),
# each with a reason. An entry that stops matching is a hole, so the last test
# below fails on stale exemptions -- the first draft of this file carried two
# entries for a path that does not exist in the dev tree, and the test caught it.
EXEMPT = {}

# A deliberate crash fixture (the bench-only CMD:CRASHTEST modes that prove the
# crash screen carries a diagnosis) marks its own line instead of asking for a
# file-wide exemption, so the exemption cannot spread.
FIXTURE_MARKER = "cb-fixture"

# Floor: the tree really has this many callback declarations, so a parser that
# stops matching cannot report a green over zero sites.
MIN_KNOWN_CALLBACK_NAMES = 20


def blank(match):
    # Whitespace of the same length, so every offset and line number in the
    # stripped text still addresses the raw file. The first version of this
    # helper deleted comments, which reported real defects at the wrong lines.
    return " " * len(match.group(0))


def strip_noise(text):
    """Comments and literals become spaces: nothing in them is a declaration."""
    return COMMENT_AND_STRINGS.sub(blank, text)


def angle_end(text, index):
    """index points at the '<' of `std::function<`; return just past its match."""
    depth = 0
    while index < len(text):
        char = text[index]
        if char == "<":
            depth += 1
        elif char == ">":
            depth -= 1
            if depth == 0:
                return index + 1
        index += 1
    return -1


def top_level_spans(text):
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


def declaration_names(text):
    """[(name, offset, initialised_with_lambda)] for every std::function declaration."""
    found = []
    for match in re.finditer(r"std::function<", text):
        end = angle_end(text, match.end() - 1)
        if end < 0:
            continue
        declarator = DECL_AFTER_FUNCTION.match(text, end)
        if not declarator:
            continue
        name = declarator.group(1)
        tail = text[declarator.end():declarator.end() + 40]
        found.append((name, match.start(), bool(re.match(r"\s*=\s*\[", tail))))
    for alias in set(ALIAS_DECL.findall(text)):
        for match in re.finditer(r"\b" + alias + r"\b\s*(?:const\s*)?[&*]?\s*([A-Za-z_]\w*)", text):
            tail = text[match.end():match.end() + 40]
            found.append((match.group(1), match.start(), bool(re.match(r"\s*=\s*\[", tail))))
    return found


def resolve_scope(text, decl_offset, spans):
    """The region in which a declaration's name can be invoked.

    * inside a function body   -> that body;
    * in a parameter list      -> the next body, when only the signature separates
      them (no `;` and no `}` in between, which is what distinguishes a definition
      from a declaration-only prototype in a header);
    * otherwise (class member, namespace scope) -> the whole file.
    """
    for start, end in spans:
        if start <= decl_offset < end:
            return (start, end)
    following = [(s, e) for s, e in spans if s > decl_offset]
    if following:
        start = following[0][0]
        between = text[decl_offset:start]
        if start - decl_offset <= 800 and ";" not in between and "}" not in between:
            return following[0]
    return (0, len(text))


def tested_in(body, name):
    return bool(
        re.search(r"if\s*\(\s*!?\s*" + re.escape(name) + r"\b", body)
        or re.search(re.escape(name) + r"\s*(?:!=|==)\s*(?:nullptr|NULL)", body)
        or re.search(re.escape(name) + r"\s*(?:&&|\|\||\?)", body)
        or re.search(r"(&&|\|\||!)\s*" + re.escape(name) + r"\b", body)
    )


def wrapped_in(text, offset):
    """True when `callbacks::call[Or](` opens this very invocation."""
    window = text[max(0, offset - 200):offset]
    return window.rstrip().endswith("callbacks::call") or window.rstrip().endswith("callbacks::callOr") or \
        bool(re.search(r"callbacks::call(?:Or)?(?:<[^;{}]*>)?\s*\(\s*(?:\"[^\"]*\"\s*,)?[^;{}]*$", window))


def balanced_args(text, open_paren):
    """Return (args_text, index_after_close) for the call whose '(' is at open_paren."""
    depth = 0
    for index in range(open_paren, len(text)):
        char = text[index]
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren + 1:index], index + 1
    return None, len(text)


def not_a_call(text, args, args_end, open_paren, name):
    """Reject the two shapes that look like `name(` but never invoke anything.

    * a constructor member-initializer -- `rowTitle(rowTitle)`, `popupFn(popupFn)`
      or `popupFn(std::move(popupFn))`: the argument is the parameter of the same
      name, which cannot be a call of a callback;
    * a function *declaration* whose return type happens to be named like the
      callback -- `Progress progress();`.  This one is easy to over-tighten: the
      first version only looked at the argument list and the following ';', so it
      also swallowed the eight zero-argument calls in OptionPopup.h, which are
      real defects.  A declaration is recognised only when a type token sits
      directly in front of the name on the same line -- and keywords are not type
      tokens, or `return job();` would read as a prototype.
    """
    stripped = args.strip()
    if stripped == name or stripped in (f"std::move({name})", f"std::forward<decltype({name})>({name})"):
        return True
    if stripped:
        return False
    if not re.match(r"\s*(?:const\s*)?;", text[args_end:]):
        return False
    # Only the text of this line, and only what stands in front of the name.
    line_start = text.rfind("\n", 0, open_paren) + 1
    prefix = text[line_start:open_paren].rstrip()
    if not prefix.endswith(name):
        return False
    prefix = prefix[: len(prefix) - len(name)].rstrip()
    match = re.search(r"([A-Za-z_]\w*)$", prefix)
    return bool(match) and match.group(1) not in KEYWORDS_BEFORE_CALL


def scan(texts):
    """[(relpath, line, name)] of bare invocations, plus the name count.

    Deduplicated: the same invocation is reachable from two declarations (a
    parameter and a member of the same name), and a duplicate report reads like
    two defects in the list a reader has to work through.
    """
    found = set()
    name_count = set()
    for rel, raw in texts.items():
        clean = strip_noise(raw)
        spans = top_level_spans(clean)
        for name, decl_offset, initialised in declaration_names(clean):
            name_count.add(name)
            if initialised:
                continue  # non-null by construction: `fn = [](int x){...}`
            scope = resolve_scope(clean, decl_offset, spans)
            body = clean[scope[0]:scope[1]]
            if name in ("function", "std"):
                continue
            for call in re.finditer(r"(?<![\w.>-])" + re.escape(name) + r"\s*\(", body):
                open_paren = body.index("(", call.end() - 1)
                args, args_end = balanced_args(body, open_paren)
                if args is None or not_a_call(body, args, args_end, open_paren, name):
                    continue
                if wrapped_in(body, call.start()):
                    continue
                if tested_in(body, name):
                    continue
                if (rel, name) in EXEMPT:
                    continue
                absolute = scope[0] + call.start()
                line_start = raw.rfind("\n", 0, absolute) + 1
                line_end = raw.find("\n", line_start)
                line_text = raw[line_start:(line_end if line_end != -1 else len(raw))]
                if FIXTURE_MARKER in line_text:
                    continue
                found.add((rel, raw[:absolute].count("\n") + 1, name))
    return sorted(found), name_count


def read_tree():
    texts = {}
    for root in SCAN_ROOTS:
        for base in (root,):
            for extension in ("*.cpp", "*.h"):
                for path in sorted((ROOT / base).rglob(extension)):
                    rel = str(path.relative_to(ROOT))
                    if any(part in SKIP_PARTS for part in path.parts):
                        continue
                    texts[rel] = path.read_text(errors="ignore")
    return texts


class CallbackInvocationGuardTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.texts = read_tree()
        cls.problems, cls.names = scan(cls.texts)

    def test_parser_finds_the_callbacks_the_fix_touched(self):
        """Not vacuous: the names routed through the owner must be known to it."""
        for name in ("rowTitle", "rowValue", "buttonLabel", "storeCoverBuffer", "processLine"):
            self.assertIn(name, self.names, f"the checker no longer recognises `{name}` as a callback")
        self.assertGreaterEqual(len(self.names), MIN_KNOWN_CALLBACK_NAMES,
                                f"only {len(self.names)} callback names found; the checker has stopped matching")

    def test_a_bare_invocation_is_found_and_a_wrapped_one_is_not(self):
        bare = """
void renderRows(const std::function<std::string(int)>& rowTitle) {
  std::string name = rowTitle(i);
}
"""
        wrapped = """
#include <InvokeCallback.h>
void renderRows(const std::function<std::string(int)>& rowTitle) {
  std::string name = callbacks::callOr<std::string>("theme.rowTitle", rowTitle, "", i);
}
"""
        tested = """
void renderRows(const std::function<std::string(int)>& rowTitle) {
  if (!rowTitle) return;
  std::string name = rowTitle(i);
}
"""
        byConstruction = """
void walkTree() {
  std::function<void(int)> walk = [&](int depth) { if (depth) walk(depth - 1); };
  walk(3);
}
"""
        self.assertEqual([p[2] for p in scan({"fixture.cpp": bare})[0]], ["rowTitle"])
        self.assertEqual(scan({"fixture.cpp": wrapped})[0], [], f"wrapped site reported: {scan({'fixture.cpp': wrapped})[0]}")
        self.assertEqual(scan({"fixture.cpp": tested})[0], [])
        self.assertEqual(scan({"fixture.cpp": byConstruction})[0], [])

    def test_member_callbacks_are_tracked_across_the_class_files(self):
        """A callback declared in a header and invoked in the .cpp is still a site."""
        header = """
class Popup {
 public:
  bool handleInput() { onSelect(index); return true; }
 private:
  std::function<void(int)> onSelect;
};
"""
        self.assertEqual([p[2] for p in scan({"popup.h": header})[0]], ["onSelect"])

    def test_member_functions_are_not_mistaken_for_callbacks(self):
        """`requestUpdate()` is an Activity method; it must not be reported."""
        methods = """
class Activity {
 public:
  void requestUpdate();
};
void Activity::loop() {
  requestUpdate();
}
"""
        self.assertEqual(scan({"activity.cpp": methods})[0], [])

    def test_shapes_that_look_like_calls_are_not_calls(self):
        """The two false positives the first run produced, pinned as fixtures."""
        initList = """
class Parser {
  Parser(const std::function<void(int)>& completePageFn, const std::function<void(int)>& popupFn)
      : completePageFn(completePageFn),
        popupFn(popupFn),
        other(other) {}
  std::function<void(int)> completePageFn;
  std::function<void(int)> popupFn;
};
"""
        declaration = """
struct Progress { int percent; };
using ProgressCallback = std::function<void(size_t, size_t)>;
bool startUpdate();
Progress progress();
bool downloadToTemp(int handle, ProgressCallback progress, const bool* cancel);
"""
        self.assertEqual(scan({"parser.h": initList})[0], [],
                         f"a member-initializer read as a call: {scan({'parser.h': initList})[0]}")
        self.assertEqual(scan({"update.h": declaration})[0], [],
                         f"a function declaration read as a call: {scan({'update.h': declaration})[0]}")

        # ... and the tightening must not swallow the calls it was meant to find.
        # OptionPopup.h has exactly these shapes; a rule that hid them would make
        # this gate look green while eight abort sites stayed in the tree.
        inBlock = """
void handle(const std::function<void()>& requestUpdate) {
  if (pressed) {
    requestUpdate();
  }
  return requestUpdate();
}
"""
        reported = scan({"popup.h": inBlock})[0]
        self.assertEqual(len(reported), 2, f"zero-argument calls must still be reported, got {reported}")

    def test_the_fixture_marker_only_covers_the_line_it_is_written_on(self):
        """CMD:CRASHTEST's deliberate aborts are marked per line, not per file."""
        marked = """
void benchCrash(const std::function<void(int)>& unset) {
  unset(1);  // cb-fixture
}
"""
        unmarked = """
void otherPath(const std::function<void(int)>& unset) {
  unset(1);
}
"""
        self.assertEqual(scan({"bench.cpp": marked})[0], [], "the marked fixture line must not be reported")
        self.assertEqual([p[2] for p in scan({"bench.cpp": unmarked})[0]], ["unset"],
                         "the same name on an unmarked line is still a defect")

    def test_no_bare_callback_invocation_ships(self):
        self.assertEqual(self.problems, [], "\n".join(f"{rel}:{line}: bare {name}()"
                                                     for rel, line, name in self.problems))

    def test_exemptions_are_rare_and_reasoned(self):
        """A file-wide exemption is a hole with a comment attached, so it has to
        justify itself, name a file that exists, and stay rare.  Today the table
        is empty: every remaining bare call in the tree is either wrapped, tested,
        or a per-line bench fixture."""
        stale = []
        for (rel, name), reason in EXEMPT.items():
            if not reason.strip():
                stale.append(f"({rel}, {name}) has no reason")
            elif rel not in self.texts:
                stale.append(f"({rel}, {name}) points at a file that does not exist")
            elif name not in {d[0] for d in declaration_names(strip_noise(self.texts[rel]))}:
                stale.append(f"({rel}, {name}) no longer matches a callback declaration")
        self.assertEqual(stale, [])
        self.assertLessEqual(len(EXEMPT), 4,
                              "more than four file-wide exemptions means the rule is no longer the rule")

    def test_the_bench_fixture_survives_in_the_tree(self):
        """CMD:CRASHTEST's `func` mode is the only proof that the crash screen
        carries a diagnosis, so the gate must not silently delete it."""
        main = self.texts.get("src/main.cpp", "")
        self.assertIn("CRASHTEST func", main, "the deliberate bad_function_call bench mode is gone")
        self.assertIn(FIXTURE_MARKER, main, f"its line lost the {FIXTURE_MARKER} marker")


if __name__ == "__main__":
    unittest.main(verbosity=2)

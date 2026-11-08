#!/usr/bin/env python3
"""Check the base settings table: unique keys, exact reservation, small batches.

`getBaseSettingsList()` reserves its vector before appending the rows, because a
reallocation would double the capacity of the one copy of the table that stays
resident for the whole session (a second ~16 KB block on the ESP32-C3) and the
firmware is built with -fno-exceptions, so a failed allocation aborts. Nothing in
the header can prove that reservation at compile time: SettingInfo holds
std::function/vector members, so the rows are not constant expressions and
`entryCount` cannot be a static_assert over them.

This test closes that gap. For every capability combination found in the guards of
the header itself it re-preprocesses the base list, then

  * rejects duplicate Web Settings API keys,
  * requires every row to sit inside one of the append batches,
  * requires every batch to hold at most MAX_SETTINGS_BATCH_ROWS rows (four
    SettingInfo temporaries is the most the C3 stack may hold at once),
  * compiles `static_assert(entryCount == <counted rows>)` for that combination.

The guard names are discovered from the source, never hardcoded, and two mutation
controls keep the checker honest: a fifth row in a batch must be rejected and a row
added without moving `entryCount` must fail the generated static_assert.
"""

from collections import Counter
from itertools import product
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[2]
SETTINGS_LIST = ROOT / "src/SettingsList.h"

# Mirrors kMaxSettingsBatchRows in src/SettingsList.h.
MAX_SETTINGS_BATCH_ROWS = 4

# Only these preprocessor forms may appear inside the base list; anything else
# would make the row count below unreliable, so it is a hard error.
GUARD_DIRECTIVES = ("#if", "#elif", "#else", "#endif")

GUARD_TOKEN = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")
SUPPORTED_EXPRESSION_TOKENS = re.compile(r"^[\sA-Za-z0-9_!&|()]+$")
GUARD_LEXEME = re.compile(r"\s*(defined|[A-Za-z_][A-Za-z0-9_]*|&&|\|\||!|\(|\))")

BATCH_OPENER = "v.insert(v.end(), {"


class GuardError(ValueError):
    """A base-list guard this checker does not model."""


def tokenize_expression(expression):
    tokens = []
    position = 0
    while position < len(expression):
        match = GUARD_LEXEME.match(expression, position)
        if match is None:
            if expression[position].isspace():
                position += 1
                continue
            raise GuardError(f"unsupported guard token near {expression[position:]!r}")
        tokens.append(match.group(1))
        position = match.end()
    return tokens


class GuardParser:
    """Recursive-descent evaluator for `!`, `&&`, `||`, `defined`, identifiers."""

    def __init__(self, expression, capabilities):
        self.expression = expression
        self.capabilities = capabilities
        self.tokens = tokenize_expression(expression)
        self.position = 0

    def evaluate(self):
        value = self.parse_or()
        if self.position != len(self.tokens):
            raise GuardError(f"trailing tokens in base-list guard {self.expression!r}")
        return value

    def peek(self):
        return self.tokens[self.position] if self.position < len(self.tokens) else None

    def take(self, expected=None):
        token = self.peek()
        if token is None or (expected is not None and token != expected):
            raise GuardError(f"expected {expected or 'a token'} in base-list guard {self.expression!r}")
        self.position += 1
        return token

    def parse_or(self):
        value = self.parse_and()
        while self.peek() == "||":
            self.take("||")
            right = self.parse_and()
            value = value or right
        return value

    def parse_and(self):
        value = self.parse_unary()
        while self.peek() == "&&":
            self.take("&&")
            right = self.parse_unary()
            value = value and right
        return value

    def parse_unary(self):
        if self.peek() == "!":
            self.take("!")
            return not self.parse_unary()
        return self.parse_atom()

    def parse_atom(self):
        token = self.take()
        if token == "(":
            value = self.parse_or()
            self.take(")")
            return value
        if token == "defined":
            if self.peek() == "(":
                self.take("(")
                name = self.take()
                self.take(")")
            else:
                name = self.take()
            return self.capability(name)
        return self.capability(token)

    def capability(self, name):
        if name in ("0", "1"):
            return name == "1"
        if name not in self.capabilities:
            raise GuardError(f"base-list guard {self.expression!r} references unknown capability {name}")
        return self.capabilities[name]


def matching_delimiter(source, opening, opening_char, closing_char):
    """Return the matching delimiter while ignoring C++ string literals."""
    depth = 0
    in_string = False
    escaped = False
    for index in range(opening, len(source)):
        char = source[index]
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            continue
        if char == '"':
            in_string = True
        elif char == opening_char:
            depth += 1
        elif char == closing_char:
            depth -= 1
            if depth == 0:
                return index
    raise ValueError(f"unclosed {opening_char} at {opening}")


def split_arguments(arguments):
    """Split a C++ call's arguments without splitting nested initializers."""
    parts = []
    start = 0
    stack = []
    in_string = False
    escaped = False
    pairs = {"(": ")", "{": "}", "[": "]"}
    for index, char in enumerate(arguments):
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            continue
        if char == '"':
            in_string = True
        elif char in pairs:
            stack.append(pairs[char])
        elif stack and char == stack[-1]:
            stack.pop()
        elif char == "," and not stack:
            parts.append(arguments[start:index].strip())
            start = index + 1
    parts.append(arguments[start:].strip())
    return parts


def base_list_source(source):
    marker = "baseList = [] {"
    start = source.index(marker) + len(marker) - 1
    end = matching_delimiter(source, start, "{", "}")
    return source[start + 1 : end]


def capability_names(body):
    """Discover the capability macros the base list's guards reference."""
    names = set()
    for line in body.splitlines():
        directive = line.strip()
        if not directive.startswith("#"):
            continue
        keyword = directive.split()[0]
        if keyword not in GUARD_DIRECTIVES:
            raise ValueError(f"unsupported preprocessor directive in the base list: {directive}")
        if keyword in ("#else", "#endif"):
            continue
        expression = directive[len(keyword) :].strip()
        if not SUPPORTED_EXPRESSION_TOKENS.match(expression):
            raise ValueError(f"unsupported guard expression in the base list: {directive}")
        for token in GUARD_TOKEN.findall(expression):
            if token != "defined":
                names.add(token)
    if not names:
        raise ValueError("the base list has no capability guards; the table shape changed")
    return sorted(names)


def evaluate_expression(expression, capabilities):
    """Evaluate a preprocessor guard against one capability assignment."""
    return GuardParser(expression, capabilities).evaluate()


def preprocess_capability_guards(source, capabilities):
    """Keep the active branch for the guards the base list actually uses."""
    result = []
    active = True
    branches = []
    for line in source.splitlines(keepends=True):
        directive = line.strip()
        if directive.startswith("#if "):
            condition = evaluate_expression(directive[len("#if ") :].strip(), capabilities)
            branches.append((active, condition))
            active = active and condition
        elif directive.startswith("#elif "):
            parent_active, matched = branches[-1]
            condition = evaluate_expression(directive[len("#elif ") :].strip(), capabilities)
            branches[-1] = (parent_active, matched or condition)
            active = parent_active and not matched and condition
        elif directive == "#else":
            parent_active, matched = branches[-1]
            branches[-1] = (parent_active, True)
            active = parent_active and not matched
        elif directive == "#endif":
            active = branches.pop()[0]
        elif active:
            result.append(line)
    assert not branches, "unclosed base-list capability guard"
    return "".join(result)


def setting_keys(list_source):
    """Web Settings API keys in table order, for an already preprocessed list."""
    key_argument_index = {
        "Toggle": 2,
        "Enum": 3,
        "StaticEnum": 3,
        "Value": 3,
        "SignedValue": 3,
        "String": 3,
        "DynamicEnum": 4,
        "DynamicString": 3,
    }
    keys = []
    position = 0
    while True:
        start = list_source.find("SettingInfo::", position)
        if start < 0:
            return keys
        name_start = start + len("SettingInfo::")
        opening = list_source.find("(", name_start)
        method = list_source[name_start:opening]
        closing = matching_delimiter(list_source, opening, "(", ")")
        position = closing + 1
        if method not in key_argument_index:
            continue
        argument = split_arguments(list_source[opening + 1 : closing])[key_argument_index[method]]
        if argument.startswith('"') and argument.endswith('"'):
            keys.append(argument[1:-1])


def row_count(source):
    return len(re.findall(r"SettingInfo::\w+\(", source))


def batch_sizes(source):
    """Row count of every append batch, in source order."""
    sizes = []
    for batch in re.finditer(re.escape(BATCH_OPENER), source):
        opening = batch.end() - 1
        closing = matching_delimiter(source, opening, "{", "}")
        sizes.append(row_count(source[opening:closing]))
    return sizes


def entry_count_declaration(source):
    match = re.search(r"constexpr size_t entryCount = [^;]+;", source)
    assert match, "the base list has no constexpr entryCount declaration"
    return match.group()


def entry_count_value(declaration):
    """Sum the constant terms of an already preprocessed `98 + 1 + ...` declaration."""
    expression = declaration.split("=", 1)[1].rstrip(";").replace("\n", " ")
    terms = [term.strip() for term in expression.split("+")]
    assert all(term.isdigit() for term in terms), f"entryCount is not a constant sum: {declaration}"
    return sum(int(term) for term in terms)


def check_combination(source, capabilities):
    """Every invariant one capability assignment must satisfy; returns the declaration."""
    keys = setting_keys(source)
    duplicates = sorted(key for key, count in Counter(keys).items() if count > 1)
    assert not duplicates, f"duplicate Web Settings API keys for {capabilities}: {', '.join(duplicates)}"

    rows = row_count(source)
    sizes = batch_sizes(source)
    assert sizes and max(sizes) <= MAX_SETTINGS_BATCH_ROWS, f"oversized settings batch for {capabilities}: {sizes}"
    assert sum(sizes) == rows, f"settings outside insert batches for {capabilities}"

    declaration = entry_count_declaration(source)
    assert entry_count_value(declaration) == rows, (
        f"entryCount reserves {entry_count_value(declaration)} rows but {rows} are appended for {capabilities}; "
        "move the count with the row"
    )
    return declaration


def compile_snippet(snippet):
    return subprocess.run(
        ["c++", "-std=c++20", "-x", "c++", "-fsyntax-only", "-"],
        input=snippet,
        text=True,
        capture_output=True,
    )


def add_row_to_batch(body, batch_index, row):
    """Insert one row before the closing brace of the given append batch."""
    batches = list(re.finditer(re.escape(BATCH_OPENER), body))
    assert batch_index < len(batches), "mutation target batch missing"
    opening = batches[batch_index].end() - 1
    closing = matching_delimiter(body, opening, "{", "}")
    return body[:closing] + row + body[closing:]


def main():
    source = SETTINGS_LIST.read_text()
    body = base_list_source(source)
    names = capability_names(body)

    capacity_checks = ["#include <cstddef>"]
    for values in product((False, True), repeat=len(names)):
        capabilities = dict(zip(names, values))
        list_source = preprocess_capability_guards(body, capabilities)
        declaration = check_combination(list_source, capabilities)
        rows = row_count(list_source)
        capacity_checks.append(
            f"namespace combination_{len(capacity_checks)} {{ {declaration} "
            f"static_assert(entryCount == {rows}); }}"
        )

    compiled = compile_snippet("\n".join(capacity_checks))
    assert compiled.returncode == 0, f"capacity static_asserts do not compile:\n{compiled.stderr}"

    # Mutation control 1: a fifth row in a batch must be rejected. The first batch
    # is unconditional, so the violation shows up in every capability combination.
    probe_row = 'SettingInfo::Toggle(StrId::STR_SLEEP_STATE_BADGE, nullptr, "batchProbe", StrId::STR_CAT_DISPLAY),\n'
    mutated_body = add_row_to_batch(body, 0, probe_row)
    for values in product((False, True), repeat=len(names)):
        capabilities = dict(zip(names, values))
        mutated = preprocess_capability_guards(mutated_body, capabilities)
        if max(batch_sizes(mutated)) > MAX_SETTINGS_BATCH_ROWS:
            break
    else:
        raise AssertionError("a five-row batch was not rejected; the batch cap check is dead")

    # Mutation control 2: adding a row without moving entryCount must fail the
    # generated static_assert (this is the exact drift the check exists for).
    capabilities = dict(zip(names, (True,) * len(names)))
    list_source = preprocess_capability_guards(body, capabilities)
    declaration = check_combination(list_source, capabilities)
    drifted_source = preprocess_capability_guards(add_row_to_batch(body, 0, probe_row), capabilities)
    drifted = compile_snippet(
        f"#include <cstddef>\n{declaration}\nstatic_assert(entryCount == {row_count(drifted_source)});"
    )
    assert drifted.returncode != 0, "an un-moved entryCount was accepted; the capacity check is dead"

    print(f"Web Settings API keys are unique across {2 ** len(names)} capability combinations")
    print(f"Base settings capacity matches every combination ({', '.join(names)}) without vector growth")
    print(f"Every settings initializer batch has at most {MAX_SETTINGS_BATCH_ROWS} entries")


if __name__ == "__main__":
    main()

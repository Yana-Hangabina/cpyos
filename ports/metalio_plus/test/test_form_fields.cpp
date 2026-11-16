// Host tests for the form/JSON helpers used by /mkdir, /rename, /move, /delete.
#include "form_fields.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}
}  // namespace

int main() {
    using namespace form_fields;
    std::vector<Field> fields;
    parseForm("path=%2FBooks&name=New%20Folder&empty=", fields);
    check(get(fields, "path") && *get(fields, "path") == "/Books", "path decoded");
    check(get(fields, "name") && *get(fields, "name") == "New Folder", "space decoded");
    check(get(fields, "empty") && get(fields, "empty")->empty(), "empty value");
    check(get(fields, "missing") == nullptr, "missing field");

    fields.clear();
    parseForm("a=1+b&b=%E4%B8%AD", fields);
    check(*get(fields, "a") == "1 b", "plus to space");
    check(*get(fields, "b") == "\xE4\xB8\xAD", "utf-8 percent decode");

    fields.clear();
    parseForm("broken&x", fields);  // no '=' -> ignored
    check(fields.empty(), "ignored malformed pairs");

    std::vector<std::string> paths;
    check(parseJsonStringArray("[\"/a\",\"/b c\"]", paths) && paths.size() == 2 && paths[1] == "/b c",
          "json array");
    check(parseJsonStringArray(" [ ] ", paths) && paths.empty(), "empty array");
    check(parseJsonStringArray("[\"/a\\\"b\"]", paths) && paths[0] == "/a\"b", "escaped quote");
    check(!parseJsonStringArray("[\"/a\",]", paths), "trailing comma rejected");
    check(!parseJsonStringArray("/a", paths), "not an array");
    check(!parseJsonStringArray("[\"unterminated]", paths), "unterminated string");

    if (failures == 0) std::printf("form fields: all checks passed\n");
    return failures == 0 ? 0 : 1;
}

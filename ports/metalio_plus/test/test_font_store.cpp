#include "font_store.h"

#include <cstdio>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
  if (!ok) { std::printf("FAIL: %s\n", what); ++failures; }
}
}  // namespace

int main() {
  using plus_fonts::safeFamily;
  using plus_fonts::safeFilename;
  check(safeFamily("Noto Sans SC"), "plain family");
  check(safeFamily("思源黑体"), "utf8 family");
  check(!safeFamily(""), "empty family");
  check(!safeFamily(".."), "dotdot family");
  check(!safeFamily(".hidden"), "dot family");
  check(!safeFamily("a/b"), "slash family");
  check(!safeFamily("a\\b"), "backslash family");
  check(!safeFamily(std::string(65, 'a')), "long family");

  check(safeFilename("NotoSans.ttf"), "ttf");
  check(safeFilename("Font.OTF"), "uppercase extension");
  check(safeFilename("Font.otf"), "otf");
  check(safeFilename("Font.ttc"), "ttc");
  check(safeFilename("Font.cpfont"), "cpfont");
  check(!safeFilename("../x.ttf"), "traversal filename");
  check(!safeFilename(".x.ttf"), "dot filename");
  check(!safeFilename("x.exe"), "wrong extension");
  check(!safeFilename(std::string(100, 'a') + ".ttf"), "long filename");

  if (failures == 0) std::printf("font store: all checks passed\n");
  return failures == 0 ? 0 : 1;
}

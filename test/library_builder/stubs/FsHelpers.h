#pragma once

#include <string>
#include <string_view>

namespace FsHelpers {
inline bool checkFileExtension(const std::string& path, const char* extension) { return path.ends_with(extension); }
inline bool hasEpubExtension(const std::string& path) { return checkFileExtension(path, ".epub"); }
inline bool hasFb2Extension(const std::string& path) { return checkFileExtension(path, ".fb2"); }
inline bool hasXtcExtension(const std::string& path) {
  return checkFileExtension(path, ".xtc") || checkFileExtension(path, ".xtch");
}
// Mirrors lib/FsHelpers: firmware-owned and OS-owned names the shelf never indexes.
inline bool isProtectedPathComponent(const std::string_view component) {
  return component.empty() || component.front() == '.' || component == "System Volume Information" ||
         component == "XTCache";
}
}  // namespace FsHelpers

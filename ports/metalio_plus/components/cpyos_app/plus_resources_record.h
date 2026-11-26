#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

// On-card record of CloudZao resources this device has installed
// ("/.crosspoint/cloudzao_resources.txt"). Deliberately free of Arduino/HAL so
// the writer and the parser are host-testable: a mis-parsed record would
// silently hide downloaded resources from the page (they could then neither be
// seen nor deleted).
//
// Wire format:
//
//   CZR1
//   <type char>\t<path>\t<size>\t<sha256>\n
//
// `type` is B (book), G (badge/image) or F (font). Paths are absolute card
// paths without "..". Unknown lines are skipped; a missing magic rejects the
// whole file (a corrupted record must not resurrect arbitrary paths).
namespace plus_resources_record {

constexpr char kMagic[] = "CZR1";
constexpr size_t kMaxEntries = 64;
constexpr size_t kMaxBytes = 8192;

struct Entry {
  char type = '?';
  std::string path;
  uint64_t file_size = 0;
  std::string sha256;
};

inline bool pathIsSafe(std::string_view path) {
  return path.size() >= 2 && path.size() < 240 && path[0] == '/' && path.find("..") == std::string_view::npos;
}

// Only the three resource types CPYOS installs; anything else is a line from a
// future/foreign writer and must not become a deletable row.
inline bool typeIsKnown(char type) { return type == 'B' || type == 'G' || type == 'F'; }

inline std::string encode(const std::vector<Entry>& entries) {
  std::string out;
  out.reserve(kMaxBytes / 8);
  out += kMagic;
  out.push_back('\n');
  size_t written = 0;
  for (const auto& entry : entries) {
    if (!typeIsKnown(entry.type) || !pathIsSafe(entry.path)) continue;
    if (written++ >= kMaxEntries) break;
    out.push_back(entry.type);
    out.push_back('\t');
    out += entry.path;
    out.push_back('\t');
    out += std::to_string(static_cast<unsigned long long>(entry.file_size));
    out.push_back('\t');
    out += entry.sha256;
    out.push_back('\n');
  }
  return out;
}

inline bool decode(std::string_view text, std::vector<Entry>& out) {
  out.clear();
  if (text.size() > kMaxBytes) return false;
  size_t pos = 0;
  bool first = true;
  while (pos <= text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string_view::npos) end = text.size();
    std::string_view line = text.substr(pos, end - pos);
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (first) {
      first = false;
      if (line != kMagic) return false;
    } else if (!line.empty() && out.size() < kMaxEntries) {
      const size_t t1 = line.find('\t');
      const size_t t2 = t1 == std::string_view::npos ? std::string_view::npos : line.find('\t', t1 + 1);
      const size_t t3 = t2 == std::string_view::npos ? std::string_view::npos : line.find('\t', t2 + 1);
      if (t3 != std::string_view::npos && t1 > 0) {
        Entry entry;
        entry.type = line[0];
        entry.path.assign(line.substr(t1 + 1, t2 - t1 - 1));
        const std::string size(line.substr(t2 + 1, t3 - t2 - 1));
        entry.file_size = strtoull(size.c_str(), nullptr, 10);
        entry.sha256.assign(line.substr(t3 + 1));
        if (typeIsKnown(entry.type) && pathIsSafe(entry.path)) out.push_back(std::move(entry));
      }
    }
    if (end == text.size()) break;
    pos = end + 1;
  }
  return true;
}

}  // namespace plus_resources_record

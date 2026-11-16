#pragma once

// Pure path helpers for the SD file-transfer server. Kept free of IDF headers
// so the traversal rules can be exercised by a host test.

#include <cctype>
#include <string>

namespace web_transfer {
namespace paths {

inline constexpr const char* kRoot = "/sdcard";

inline std::string urlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size() && std::isxdigit(static_cast<unsigned char>(in[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(in[i + 2]))) {
            const auto hex = [](char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return c - 'A' + 10;
            };
            out += static_cast<char>((hex(in[i + 1]) << 4) | hex(in[i + 2]));
            i += 2;
        } else {
            out += in[i];
        }
    }
    return out;
}

// Logical paths are absolute, already URL-decoded, and never contain a ".."
// component or the private dot directory. This is a deny-list on purpose: the
// server only ever appends to kRoot, so anything that could escape it or read
// device secrets is refused rather than normalised.
inline bool safe(const std::string& logical) {
    if (logical.empty() || logical[0] != '/') return false;
    if (logical.find("..") != std::string::npos) return false;
    if (logical.find("/.crosspoint") == 0 || logical == "/.crosspoint") return false;
    if (logical.find("//") != std::string::npos) return false;
    return true;
}

inline std::string toReal(const std::string& logical) {
    if (logical == "/") return kRoot;
    return std::string(kRoot) + logical;
}

}  // namespace paths
}  // namespace web_transfer

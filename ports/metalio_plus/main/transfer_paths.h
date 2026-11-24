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
    if (logical.find('\\') != std::string::npos) return false;
    // FAT is case-insensitive, so "/.CROSSPOINT" reaches the same directory.
    {
        size_t slash = logical.find('/', 1);
        std::string first = logical.substr(1, slash == std::string::npos ? std::string::npos : slash - 1);
        std::string lower;
        lower.reserve(first.size());
        for (char c : first) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (lower.rfind(".crosspoint", 0) == 0) return false;
    }
    if (logical.find("//") != std::string::npos) return false;
    // FAT exposes ".crosspoint" through its 8.3 alias (CROSSP~1). Blocking the
    // long name alone would let WebDAV/HTTP read the same settings and session
    // files through the alias. Reject any segment that looks like that alias.
    size_t start = 0;
    while (start <= logical.size()) {
        size_t slash = logical.find('/', start + 1);
        std::string seg = logical.substr(start + 1, slash == std::string::npos ? std::string::npos : slash - start - 1);
        if (!seg.empty()) {
            std::string upper;
            upper.reserve(seg.size());
            for (char c : seg) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
            if (upper.find('~') != std::string::npos && upper.rfind("CROSSP", 0) == 0) return false;
            if (upper == "CROSSPOINT") return false;
            // A collision-free 8.3 stem of the same directory.
            if (upper.size() <= 12 && upper.rfind("CROSSPOI", 0) == 0) return false;
        }
        if (slash == std::string::npos) break;
        start = slash;
    }
    return true;
}

inline std::string toReal(const std::string& logical) {
    if (logical == "/") return kRoot;
    return std::string(kRoot) + logical;
}

}  // namespace paths
}  // namespace web_transfer

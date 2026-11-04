#pragma once

#include <string>
#include <string_view>

namespace webbrowser {

// Returns a lowercase charset name ("utf-8", "gbk", "gb18030", ...) or empty
// when neither the Content-Type header nor the document says anything.
// `head` should be the first few KB of the (still undecoded) body.
std::string sniffCharset(std::string_view head, std::string_view contentType);

// True for the GBK family, which the device must transcode to UTF-8 before the
// extractor sees it.
bool isGbkCharset(std::string_view charset);

}  // namespace webbrowser

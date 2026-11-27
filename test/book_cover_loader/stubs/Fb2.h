#pragma once

#include <string>

// FB2 is converted to an EPUB next to it; the loader only asks for that path.
class Fb2 {
 public:
  Fb2(std::string path, const std::string&) : path_(std::move(path)) {}
  std::string getEpubPath() const { return path_ + ".epub"; }

 private:
  std::string path_;
};

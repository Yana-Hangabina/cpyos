#include "PinyinFormat.h"

namespace pinyin {

const char* pydbValidityName(const PydbValidity validity) {
  switch (validity) {
    case PydbValidity::Ok:
      return "ok";
    case PydbValidity::TooSmall:
      return "too small";
    case PydbValidity::BadMagic:
      return "bad magic";
    case PydbValidity::UnknownVersion:
      return "unknown version";
    case PydbValidity::SizeMismatch:
      return "size mismatch";
    case PydbValidity::SectionsInconsistent:
      return "sections inconsistent";
    case PydbValidity::BadCrc:
      return "bad crc";
  }
  return "unknown";
}

}  // namespace pinyin

#include "OpdsFeed.h"

#include <Memory.h>

namespace {
constexpr size_t kMaxPendingBytes = 64;
const std::string kEmpty;

bool isJsonBomStart(const std::string& pending, const size_t index) {
  // UTF-8 BOM: EF BB BF. Partial prefixes are still "not decided yet".
  return pending.size() > index && static_cast<uint8_t>(pending[index]) == 0xEF;
}
}  // namespace

OpdsFeed::OpdsFeed() = default;

size_t OpdsFeed::write(const uint8_t* data, size_t size) {
  if (size == 0) return size;

  if (xml_) {
    xml_->write(data, size);
    return size;
  }
  if (json_) {
    json_->write(data, size);
    return size;
  }

  if (malformed_) return size;
  pending_.append(reinterpret_cast<const char*>(data), size);

  size_t index = 0;
  if (pending_.size() >= 3 && static_cast<uint8_t>(pending_[0]) == 0xEF &&
      static_cast<uint8_t>(pending_[1]) == 0xBB && static_cast<uint8_t>(pending_[2]) == 0xBF) {
    index = 3;
  } else if (isJsonBomStart(pending_, 0) && pending_.size() < 3) {
    return size;  // wait for the rest of a possible BOM
  }

  for (; index < pending_.size(); ++index) {
    const auto byte = static_cast<uint8_t>(pending_[index]);
    if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') continue;
    if (byte == '{' || byte == '[') {
      json_ = makeUniqueNoThrow<OpdsJsonParser>();
      if (!json_) {
        malformed_ = true;
        return size;
      }
      json_->write(reinterpret_cast<const uint8_t*>(pending_.data()), pending_.size());
    } else if (byte == '<') {
      xml_ = makeUniqueNoThrow<OpdsParser>();
      if (!xml_) {
        malformed_ = true;
        return size;
      }
      xml_->write(reinterpret_cast<const uint8_t*>(pending_.data()), pending_.size());
    } else {
      malformed_ = true;
      return size;
    }
    std::string().swap(pending_);
    return size;
  }

  if (pending_.size() > kMaxPendingBytes) malformed_ = true;
  return size;
}

void OpdsFeed::flush() {
  if (xml_) xml_->flush();
  // JSON needs an explicit end-of-input check: the tokenizer emits every token
  // at its delimiter, so a body cut off mid-document looks like a short feed.
  if (json_) json_->finish();
}

bool OpdsFeed::error() const {
  if (malformed_ || (!xml_ && !json_)) return true;
  if (xml_) return xml_->error();
  return json_->error() || !json_->recognized();
}

bool OpdsFeed::truncated() const {
  if (xml_) return xml_->truncated();
  if (json_) return json_->truncated();
  return false;
}

const std::vector<OpdsEntry>& OpdsFeed::getEntries() const {
  static const std::vector<OpdsEntry> kNone;
  if (xml_) return xml_->getEntries();
  if (json_) return json_->getEntries();
  return kNone;
}

std::vector<OpdsEntry> OpdsFeed::takeEntries() {
  if (xml_) return xml_->takeEntries();
  if (json_) return json_->takeEntries();
  return {};
}

const std::string& OpdsFeed::getSearchTemplate() const {
  if (xml_) return xml_->getSearchTemplate();
  if (json_) return json_->getSearchTemplate();
  return kEmpty;
}

const std::string& OpdsFeed::getNextPageUrl() const {
  if (xml_) return xml_->getNextPageUrl();
  if (json_) return json_->getNextPageUrl();
  return kEmpty;
}

const std::string& OpdsFeed::getPrevPageUrl() const {
  if (xml_) return xml_->getPrevPageUrl();
  if (json_) return json_->getPrevPageUrl();
  return kEmpty;
}

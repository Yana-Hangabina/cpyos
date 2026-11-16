// Streaming multipart/form-data parser for the documented POST /upload endpoint.
//
// Only what the file-transfer API needs: one (or more) parts with a
// Content-Disposition filename. Bytes are forwarded to the sink as they arrive,
// so a 500 MiB book never has to fit in RAM. The parser keeps at most one
// boundary plus a few bytes pending, and tolerates the boundary being split
// across feed() calls. ESP-IDF-free so it has host tests.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace multipart {

struct Sink {
  bool (*start)(void* ctx, const std::string& filename) = nullptr;
  bool (*write)(void* ctx, const uint8_t* data, size_t len) = nullptr;
  void (*finish)(void* ctx) = nullptr;
  void* ctx = nullptr;
  // Non-file parts (e.g. the `family` field the official FontsPage sends before
  // the font file). Bounded to 1 KiB; omitted parts are ignored.
  bool (*field)(void* ctx, const std::string& name, const std::string& value) = nullptr;
};

// Extracts the boundary token from a Content-Type value, handling the quoted
// form (`boundary="abc"`). Returns false when absent or too long.
inline bool extractBoundary(const std::string& content_type, std::string& boundary) {
  const std::string key = "boundary=";
  size_t pos = content_type.find(key);
  if (pos == std::string::npos) return false;
  pos += key.size();
  boundary.clear();
  if (pos < content_type.size() && content_type[pos] == '"') {
    const size_t end = content_type.find('"', pos + 1);
    if (end == std::string::npos) return false;
    boundary = content_type.substr(pos + 1, end - pos - 1);
  } else {
    const size_t end = content_type.find(';', pos);
    boundary = content_type.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
  }
  while (!boundary.empty() && (boundary.back() == ' ' || boundary.back() == '\r')) boundary.pop_back();
  return !boundary.empty() && boundary.size() <= 70;
}

class Parser {
 public:
  Parser(std::string boundary, Sink sink) : boundary_(std::move(boundary)), sink_(sink) {}

  bool feed(const uint8_t* data, size_t len) {
    if (state_ == State::Error || state_ == State::Done) return state_ == State::Done;
    pending_.append(reinterpret_cast<const char*>(data), len);
    return pump();
  }

  bool finish() {
    if (state_ == State::Error) return false;
    if (state_ == State::Body && !pending_.empty()) return fail();
    if (state_ == State::Body) return endFile();
    return state_ == State::Done || state_ == State::Preamble;
  }

  bool done() const { return state_ == State::Done; }

 private:
  enum class State { Preamble, Headers, Body, AfterBoundary, Done, Error };

  bool fail() {
    state_ = State::Error;
    pending_.clear();
    return false;
  }

  bool endFile() {
    if (in_file_ && sink_.finish) sink_.finish(sink_.ctx);
    if (in_field_ && !flushField()) return fail();
    in_file_ = false;
    in_field_ = false;
    state_ = State::Done;
    return true;
  }

  bool flushField() {
    if (sink_.field && !field_name_.empty() &&
        !sink_.field(sink_.ctx, field_name_, field_value_)) {
      return false;
    }
    return true;
  }

  static bool fieldNameFromHeaders(const std::string& headers, std::string& out) {
    const std::string key = "name=";
    size_t pos = headers.find(key);
    if (pos == std::string::npos) return false;
    pos += key.size();
    if (pos < headers.size() && headers[pos] == '"') {
      const size_t end = headers.find('"', pos + 1);
      if (end == std::string::npos) return false;
      out = headers.substr(pos + 1, end - pos - 1);
      return !out.empty();
    }
    const size_t end = headers.find_first_of(";\r\n", pos);
    out = headers.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    return !out.empty();
  }

  bool pump() {
    for (;;) {
      if (state_ == State::Preamble) {
        const std::string first = "--" + boundary_;
        const size_t pos = pending_.find(first);
        if (pos == std::string::npos) {
          if (pending_.size() > first.size() + 4) pending_.erase(0, pending_.size() - (first.size() + 4));
          return true;
        }
        pending_.erase(0, pos + first.size());
        state_ = State::Headers;
      } else if (state_ == State::Headers) {
        const size_t end = pending_.find("\r\n\r\n");
        if (end == std::string::npos) {
          if (pending_.size() > 8192) return fail();
          return true;
        }
        const std::string headers = pending_.substr(0, end);
        pending_.erase(0, end + 4);
        std::string filename;
        if (filenameFromHeaders(headers, filename)) {
          if (!sink_.start || !sink_.start(sink_.ctx, filename)) return fail();
          in_file_ = true;
          in_field_ = false;
        } else {
          in_file_ = false;
          in_field_ = true;
          field_name_.clear();
          field_value_.clear();
          if (!fieldNameFromHeaders(headers, field_name_)) return fail();
        }
        state_ = State::Body;
      } else if (state_ == State::Body) {
        const std::string marker = "\r\n--" + boundary_;
        const size_t pos = pending_.find(marker);
        if (pos == std::string::npos) {
          // Keep enough tail to recognise a split marker.
          const size_t keep = marker.size() + 2;
          if (pending_.size() > keep) {
            const size_t emit = pending_.size() - keep;
            if (sink_.write && !sink_.write(sink_.ctx, reinterpret_cast<const uint8_t*>(pending_.data()), emit)) {
              return fail();
            }
            pending_.erase(0, emit);
          }
          return true;
        }
        if (pos > 0) {
          if (in_file_) {
            if (sink_.write &&
                !sink_.write(sink_.ctx, reinterpret_cast<const uint8_t*>(pending_.data()), pos)) {
              return fail();
            }
          } else if (in_field_) {
            if (field_value_.size() + pos > 1024) return fail();
            field_value_.append(pending_.data(), pos);
          }
        }
        pending_.erase(0, pos + marker.size());
        // Either "--" (last boundary) or CRLF followed by the next part.
        state_ = State::AfterBoundary;
      } else if (state_ == State::AfterBoundary) {
        if (pending_.size() < 2) return true;  // wait for more bytes
        if (pending_.compare(0, 2, "--") == 0) return endFile();
        if (pending_.compare(0, 2, "\r\n") != 0) return fail();
        pending_.erase(0, 2);
        if (in_file_ && sink_.finish) sink_.finish(sink_.ctx);
        if (in_field_ && !flushField()) return fail();
        in_file_ = false;
        in_field_ = false;
        state_ = State::Headers;
      } else {
        return state_ != State::Error;
      }
    }
  }

  static bool filenameFromHeaders(const std::string& headers, std::string& out) {
    const std::string key = "filename=";
    size_t pos = headers.find(key);
    if (pos == std::string::npos) return false;
    pos += key.size();
    if (pos < headers.size() && headers[pos] == '"') {
      const size_t end = headers.find('"', pos + 1);
      if (end == std::string::npos) return false;
      out = headers.substr(pos + 1, end - pos - 1);
      return !out.empty();
    }
    const size_t end = headers.find_first_of(";\r\n", pos);
    out = headers.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    return !out.empty();
  }

  std::string boundary_;
  Sink sink_;
  std::string pending_;
  State state_ = State::Preamble;
  bool in_file_ = false;
  bool in_field_ = false;
  std::string field_name_;
  std::string field_value_;
};

}  // namespace multipart

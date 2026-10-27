#include "JjwxcComments.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace jjwxc {
namespace {

char asciiLower(const char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

bool ciMatch(const char* text, const size_t len, const char* lower) {
  const size_t n = strlen(lower);
  if (n > len) return false;
  for (size_t i = 0; i < n; ++i) {
    if (asciiLower(text[i]) != asciiLower(lower[i])) return false;
  }
  return true;
}

const char* ciFind(const char* text, const size_t len, const char* lower) {
  const size_t n = strlen(lower);
  if (n == 0 || n > len) return nullptr;
  for (size_t i = 0; i + n <= len; ++i) {
    if (ciMatch(text + i, len - i, lower)) return text + i;
  }
  return nullptr;
}

// Index just past the closing quote of the string starting at `p` (which must be
// a quote). Stops at `end`.
const char* skipString(const char* p, const char* end) {
  if (p >= end || *p != '"') return p;
  ++p;
  while (p < end) {
    if (*p == '\\' && p + 1 < end) {
      p += 2;
      continue;
    }
    if (*p == '"') return p + 1;
    ++p;
  }
  return end;
}

// Index just past the object starting at `p` (which must be '{'), honouring
// strings and nested objects/arrays.
const char* skipObject(const char* p, const char* end) {
  if (p >= end || *p != '{') return p;
  int depth = 0;
  while (p < end) {
    if (*p == '"') {
      p = skipString(p, end);
      continue;
    }
    if (*p == '{') ++depth;
    else if (*p == '}') {
      --depth;
      if (depth == 0) return p + 1;
    }
    ++p;
  }
  return end;
}

// Finds `"key"` inside [begin,end), then reads its value as a string or a bare
// token into `out`. Returns false when absent.
bool valueOf(const char* begin, const char* end, const char* key, char* out, const size_t cap) {
  if (out == nullptr || cap == 0) return false;
  out[0] = '\0';
  const int keyLen = static_cast<int>(strlen(key));
  const char* p = begin;
  while (p < end) {
    const char* hit = ciFind(p, static_cast<size_t>(end - p), key);
    if (hit == nullptr) return false;
    p = hit + keyLen;
    while (p < end && *p != ':') ++p;
    if (p >= end) return false;
    ++p;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    if (p >= end) return false;
    if (*p == '"') {
      const char* close = skipString(p, end);
      size_t used = 0;
      const char* q = p + 1;
      while (q < close - 1 && used + 1 < cap) {
        char c = *q;
        size_t seq = 1;
        if (c == '\\' && q + 1 < close - 1) {
          ++q;
          c = *q;
          if (c == 'n' || c == 'r' || c == 't') c = ' ';
        } else {
          const unsigned char uc = static_cast<unsigned char>(c);
          if ((uc & 0xE0) == 0xC0) seq = 2;
          else if ((uc & 0xF0) == 0xE0) seq = 3;
          else if ((uc & 0xF8) == 0xF0) seq = 4;
          if (q + seq > close - 1) seq = 1;
        }
        if (used + seq >= cap) break;
        memcpy(out + used, q, seq);
        used += seq;
        q += seq;
      }
      out[used] = '\0';
      return used > 0;
    }
    const char* start = p;
    while (p < end && *p != ',' && *p != '}' && *p != ']') ++p;
    size_t n = static_cast<size_t>(p - start);
    if (n >= cap) n = cap - 1;
    memcpy(out, start, n);
    out[n] = '\0';
    return n > 0;
  }
  return false;
}

bool codeOk(const char* body, const size_t len) {
  char code[16];
  if (!valueOf(body, body + len, "\"code\"", code, sizeof(code))) return false;
  return strcmp(code, "200") == 0 || strcmp(code, "0") == 0;
}

// Calls `sink` for every object inside the array that follows `arrayKey`.
template <typename Entry, typename Sink>
size_t walkArray(const char* body, const size_t len, const char* arrayKey, const char* key,
                 bool (*fill)(const char* begin, const char* end, Entry&), Sink sink, void* ctx) {
  if (body == nullptr || len == 0 || !codeOk(body, len)) return 0;
  const char* p = ciFind(body, len, arrayKey);
  if (p == nullptr) return 0;
  p += strlen(arrayKey);
  const char* end = body + len;
  while (p < end && *p != '[') {
    if (*p == ':' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
      ++p;
      continue;
    }
    return 0;
  }
  if (p >= end) return 0;
  ++p;
  size_t count = 0;
  while (p < end) {
    while (p < end && (*p == ' ' || *p == ',' || *p == '\r' || *p == '\n' || *p == '\t')) ++p;
    if (p >= end || *p == ']') break;
    if (*p != '{') {
      ++p;
      continue;
    }
    const char* objEnd = skipObject(p, end);
    if (objEnd <= p) break;
    Entry item;
    if (fill(p, objEnd, item)) {
      if (!sink(ctx, item, count)) return count;
      ++count;
    }
    p = objEnd;
  }
  (void)key;
  return count;
}

bool fillParagraph(const char* begin, const char* end, ParagraphComment& item) {
  char total[16];
  char show[8];
  if (!valueOf(begin, end, "\"paragraph_id\"", item.paragraphId, sizeof(item.paragraphId))) return false;
  item.total = valueOf(begin, end, "\"comment_total\"", total, sizeof(total)) ? static_cast<uint32_t>(atoi(total)) : 0;
  item.showType = valueOf(begin, end, "\"show_type\"", show, sizeof(show)) ? static_cast<uint8_t>(atoi(show)) : 0;
  return true;
}

uint32_t numberValue(const char* begin, const char* end, const char* key) {
  char text[24];
  return valueOf(begin, end, key, text, sizeof(text)) ? static_cast<uint32_t>(strtoul(text, nullptr, 10)) : 0;
}

bool fillComment(const char* begin, const char* end, CommentItem& item) {
  valueOf(begin, end, "\"commentId\"", item.commentId, sizeof(item.commentId));
  valueOf(begin, end, "\"commentAuthor\"", item.author, sizeof(item.author));
  valueOf(begin, end, "\"commentBody\"", item.body, sizeof(item.body));
  valueOf(begin, end, "\"ip_pos\"", item.ip, sizeof(item.ip));
  valueOf(begin, end, "\"commentDate\"", item.date, sizeof(item.date));
  item.agreeNum = numberValue(begin, end, "\"agreenum\"");
  item.disagreeNum = numberValue(begin, end, "\"disagreenum\"");
  item.replyTotal = numberValue(begin, end, "\"replyTotal\"");
  item.readerAgree = static_cast<uint8_t>(numberValue(begin, end, "\"readeragree_type\""));
  item.isAuthor = numberValue(begin, end, "\"isAuthor\"") != 0;
  return item.author[0] != '\0' || item.body[0] != '\0';
}

bool fillReply(const char* begin, const char* end, CommentReplyItem& item) {
  valueOf(begin, end, "\"replyId\"", item.replyId, sizeof(item.replyId));
  // getReplyList spells these lower-case (replyAuthor/replyBody/commentdate);
  // the compare is ASCII case-insensitive, so the same keys read both spellings.
  valueOf(begin, end, "\"replyAuthor\"", item.author, sizeof(item.author));
  valueOf(begin, end, "\"replyBody\"", item.body, sizeof(item.body));
  valueOf(begin, end, "\"ip_pos\"", item.ip, sizeof(item.ip));
  valueOf(begin, end, "\"commentDate\"", item.date, sizeof(item.date));
  item.agreeNum = numberValue(begin, end, "\"agreenum\"");
  item.disagreeNum = numberValue(begin, end, "\"disagreenum\"");
  item.floor = numberValue(begin, end, "\"floor\"");
  item.readerAgree = static_cast<uint8_t>(numberValue(begin, end, "\"readeragree_type\""));
  item.isAuthor = numberValue(begin, end, "\"isAuthor\"") != 0;
  return item.author[0] != '\0' || item.body[0] != '\0';
}

bool isUrlUnreserved(const unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
         c == '.' || c == '~';
}

bool appendRaw(char* out, const size_t cap, size_t& pos, const char* text) {
  if (text == nullptr) return false;
  const size_t n = strlen(text);
  if (pos + n + 1 > cap) return false;
  memcpy(out + pos, text, n);
  pos += n;
  out[pos] = '\0';
  return true;
}

const char kHex[] = "0123456789ABCDEF";

// encodeURIComponent semantics. `twice` reproduces the PC reader's writeComment
// payload: encodeURIComponent() in the page, then jQuery encodes the query.
bool appendEncoded(char* out, const size_t cap, size_t& pos, const char* text, const bool twice) {
  if (text == nullptr) return false;
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p != 0; ++p) {
    if (isUrlUnreserved(*p)) {
      if (pos + 2 > cap) return false;
      out[pos++] = static_cast<char>(*p);
      out[pos] = '\0';
    } else if (twice) {
      if (pos + 5 > cap) return false;  // "%25" + two hex digits
      out[pos++] = '%';
      out[pos++] = '2';
      out[pos++] = '5';
      out[pos++] = kHex[(*p >> 4) & 0x0F];
      out[pos++] = kHex[*p & 0x0F];
      out[pos] = '\0';
    } else {
      if (pos + 4 > cap) return false;  // "%" + two hex digits + NUL
      out[pos++] = '%';
      out[pos++] = kHex[(*p >> 4) & 0x0F];
      out[pos++] = kHex[*p & 0x0F];
      out[pos] = '\0';
    }
  }
  return true;
}

bool appendNumber(char* out, const size_t cap, size_t& pos, const unsigned long value) {
  char text[24];
  const int n = snprintf(text, sizeof(text), "%lu", value);
  return n > 0 && static_cast<size_t>(n) < sizeof(text) && appendRaw(out, cap, pos, text);
}

}  // namespace

size_t parseParagraphComments(const char* body, size_t len, ParagraphCommentSink sink, void* ctx) {
  if (sink == nullptr) return 0;
  return walkArray<ParagraphComment>(body, len, "\"data\"", "paragraph_id", &fillParagraph, sink, ctx);
}

size_t parseCommentList(const char* body, size_t len, CommentSink sink, void* ctx) {
  if (sink == nullptr) return 0;
  return walkArray<CommentItem>(body, len, "\"commentList\"", "commentId", &fillComment, sink, ctx);
}

size_t parseReplyList(const char* body, size_t len, CommentReplySink sink, void* ctx) {
  if (sink == nullptr) return 0;
  const size_t count = walkArray<CommentReplyItem>(body, len, "\"replyList\"", "replyId", &fillReply, sink, ctx);
  if (count > 0) return count;
  // Measured 2026-10-25: formatter=app answers {"code":"200","data":[...]}.
  // The object-shaped replyList form is kept because the PC page accepts both.
  return walkArray<CommentReplyItem>(body, len, "\"data\"", "replyId", &fillReply, sink, ctx);
}

bool parseCommentActionResult(const char* body, size_t len, CommentActionResult& out) {
  out = CommentActionResult{};
  if (body == nullptr || len == 0) return false;
  const char* end = body + len;
  char code[16];
  if (!valueOf(body, end, "\"code\"", code, sizeof(code))) return false;
  out.code = atoi(code);
  valueOf(body, end, "\"message\"", out.message, sizeof(out.message));
  return true;
}

size_t buildCommentAgreeUrl(char* out, size_t outCap, const bool like, const char* novelId, const uint32_t chapterId,
                            const char* commentId, const char* replyId) {
  if (out == nullptr || outCap == 0 || novelId == nullptr || commentId == nullptr) return 0;
  out[0] = '\0';
  size_t pos = 0;
  if (!appendRaw(out, outCap, pos,
                 "https://www.jjwxc.net/app.jjwxc/Pc/Comment/addAgree?agreetype=") ||
      !appendRaw(out, outCap, pos, like ? "1" : "-1") ||
      !appendRaw(out, outCap, pos, "&novelid=") || !appendRaw(out, outCap, pos, novelId) ||
      !appendRaw(out, outCap, pos, "&chapterid=") || !appendNumber(out, outCap, pos, chapterId) ||
      !appendRaw(out, outCap, pos, "&commentid=") || !appendRaw(out, outCap, pos, commentId) ||
      !appendRaw(out, outCap, pos, "&replyid=") ||
      !appendRaw(out, outCap, pos, (replyId != nullptr && replyId[0] != '\0') ? replyId : "0")) {
    out[0] = '\0';
    return 0;
  }
  return pos;
}

size_t buildCommentReplyListUrl(char* out, size_t outCap, const char* novelId, const uint32_t chapterId,
                                const char* commentId, const uint32_t offset, const uint32_t limit) {
  if (out == nullptr || outCap == 0 || novelId == nullptr || commentId == nullptr) return 0;
  out[0] = '\0';
  size_t pos = 0;
  if (!appendRaw(out, outCap, pos,
                 "https://www.jjwxc.net/app.jjwxc/Pc/comment/getReplyList?novelid=") ||
      !appendRaw(out, outCap, pos, novelId) || !appendRaw(out, outCap, pos, "&chapterid=") ||
      !appendNumber(out, outCap, pos, chapterId) || !appendRaw(out, outCap, pos, "&commentid=") ||
      !appendRaw(out, outCap, pos, commentId) || !appendRaw(out, outCap, pos, "&offset=") ||
      !appendNumber(out, outCap, pos, offset) || !appendRaw(out, outCap, pos, "&limit=") ||
      !appendNumber(out, outCap, pos, limit) || !appendRaw(out, outCap, pos, "&formatter=app")) {
    out[0] = '\0';
    return 0;
  }
  return pos;
}

size_t buildCommentWriteUrl(char* out, size_t outCap, const char* novelId, const uint32_t chapterId,
                            const uint32_t paragraphId, const char* body, const char* author, const char* deviceId) {
  if (out == nullptr || outCap == 0 || novelId == nullptr || body == nullptr) return 0;
  out[0] = '\0';
  size_t pos = 0;
  if (!appendRaw(out, outCap, pos,
                 "https://www.jjwxc.net/app.jjwxc/Pc/comment/writeComment?novelid=") ||
      !appendRaw(out, outCap, pos, novelId) || !appendRaw(out, outCap, pos, "&chapterid=") ||
      !appendNumber(out, outCap, pos, chapterId) || !appendRaw(out, outCap, pos, "&paragraph_id=") ||
      !appendNumber(out, outCap, pos, paragraphId) || !appendRaw(out, outCap, pos, "&commentBody=") ||
      !appendEncoded(out, outCap, pos, body, /*twice=*/true) ||
      !appendRaw(out, outCap, pos, "&commentAuthor=") ||
      !appendEncoded(out, outCap, pos, author != nullptr ? author : "", /*twice=*/true) ||
      !appendRaw(out, outCap, pos,
                 "&commentSubject=&commentMark=0&emoji_id=&comment_auth_num=&shumeideviceId=") ||
      !appendEncoded(out, outCap, pos,
                     (deviceId != nullptr && deviceId[0] != '\0') ? deviceId : "get_deviceid_timeout",
                     /*twice=*/false) ||
      !appendRaw(out, outCap, pos, "&r=0")) {
    out[0] = '\0';
    return 0;
  }
  return pos;
}

size_t buildCommentReplyBody(char* out, size_t outCap, const char* novelId, const uint32_t chapterId,
                             const char* commentId, const char* body, const char* author) {
  if (out == nullptr || outCap == 0 || novelId == nullptr || commentId == nullptr || body == nullptr) return 0;
  out[0] = '\0';
  size_t pos = 0;
  if (!appendRaw(out, outCap, pos, "novelId=") || !appendRaw(out, outCap, pos, novelId) ||
      !appendRaw(out, outCap, pos, "&chapterId=") || !appendNumber(out, outCap, pos, chapterId) ||
      !appendRaw(out, outCap, pos, "&commentId=") || !appendRaw(out, outCap, pos, commentId) ||
      !appendRaw(out, outCap, pos, "&commentBody=") ||
      !appendEncoded(out, outCap, pos, body, /*twice=*/false) ||
      !appendRaw(out, outCap, pos, "&commentAuthor=") ||
      !appendEncoded(out, outCap, pos, author != nullptr ? author : "", /*twice=*/false) ||
      !appendRaw(out, outCap, pos, "&signCommentInfo=&comment_auth_num=&emoji_id=&r=0")) {
    out[0] = '\0';
    return 0;
  }
  return pos;
}

}  // namespace jjwxc

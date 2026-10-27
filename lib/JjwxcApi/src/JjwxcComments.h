// Paragraph comments (段评): the two PC-compat endpoints, both GET + token.
//
// Measured live 2026-10-17:
//   GET https://www.jjwxc.net/app.jjwxc/Pc/comment/getNovelParagraphCommentNum
//       ?versionCode=489&novelid=..&chapterid=..&token=<sid or base64(sid)>
//     -> {"code":"200","data":[{"paragraph_id":"29","comment_total":"3",
//          "show_type":"0"}, ...]}
//   GET .../Pc/comment/getCommentList
//       ?versionCode=489&novelId=..&chapterId=..&paragraph_id=..&offset=0&limit=N&token=..
//     -> {"code":"200","data":{"commentTotal":"1","commentList":[{
//          "commentId":"...", "commentAuthor":"...", "commentBody":"...",
//          "ip_pos":"上海", "commentDate":"2星期前", ...}]}}
//
// Free chapters answer anonymously; VIP chapters need the token (no token ->
// code 1004). The .com host is 403; only .jjwxc.net works.
//
// No heap, no STL, no Arduino/ESP headers. Rate limiting is the caller's job:
// this module parses one answer and never fetches.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace jjwxc {

struct ParagraphComment {
  char paragraphId[16] = {};
  uint32_t total = 0;
  uint8_t showType = 0;
};

using ParagraphCommentSink = bool (*)(void* ctx, const ParagraphComment& item, size_t index);

// Number of paragraphs in the index, 0 when the answer is malformed or carries
// no data. The caller treats 0 as "this chapter has no paragraph comments".
size_t parseParagraphComments(const char* body, size_t len, ParagraphCommentSink sink, void* ctx);

struct CommentItem {
  // The server id is what like/reply act on. Empty on a JJD1 v1 sidecar: those
  // rows are readable offline but cannot be interacted with until a refresh.
  char commentId[16] = {};
  char author[48] = {};
  char body[512] = {};
  char ip[32] = {};
  char date[32] = {};
  uint32_t agreeNum = 0;
  uint32_t disagreeNum = 0;
  uint32_t replyTotal = 0;
  // 1 = this account liked it, 2 = disliked it. The PC reader calls the same
  // field readeragree_type; 0 is "no reaction" (and is also what an anonymous
  // list carries).
  uint8_t readerAgree = 0;
  bool isAuthor = false;
};

using CommentSink = bool (*)(void* ctx, const CommentItem& item, size_t index);

size_t parseCommentList(const char* body, size_t len, CommentSink sink, void* ctx);

struct CommentReplyItem {
  char replyId[16] = {};
  char author[48] = {};
  char body[512] = {};
  char ip[32] = {};
  char date[32] = {};
  uint32_t agreeNum = 0;
  uint32_t disagreeNum = 0;
  uint32_t floor = 0;
  uint8_t readerAgree = 0;
  bool isAuthor = false;
};

using CommentReplySink = bool (*)(void* ctx, const CommentReplyItem& item, size_t index);

// GET /app.jjwxc/Pc/comment/getReplyList?formatter=app. The service answers
// either a bare array under `data` or an object carrying `replyList`; both are
// accepted. A parent comment with no replies returns an empty array.
size_t parseReplyList(const char* body, size_t len, CommentReplySink sink, void* ctx);

// One parser for all three write answers (addAgree / writeComment /
// writeReply). The service reports 200 on success, 1004 for a dead session and
// 10 when it wants the web captcha; `message` carries its own sentence.
struct CommentActionResult {
  int code = -1;
  char message[160] = {};
  bool ok() const { return code == 200 || code == 0; }
};

bool parseCommentActionResult(const char* body, size_t len, CommentActionResult& out);

// Request builders. All return the number of bytes written (excluding the NUL),
// or 0 when the buffer is too small / an argument is missing. Keeping them here
// rather than in the activity is what lets the host test pin the exact wire
// bytes without a device or a session.
//
// `like` sends agreetype=1, otherwise -1 (cancel). `replyId` is "0" for a
// top-level comment.
size_t buildCommentAgreeUrl(char* out, size_t outCap, bool like, const char* novelId, uint32_t chapterId,
                            const char* commentId, const char* replyId);

size_t buildCommentReplyListUrl(char* out, size_t outCap, const char* novelId, uint32_t chapterId,
                                const char* commentId, uint32_t offset, uint32_t limit);

// The PC reader submits a new paragraph comment as a GET with the body and
// author encoded twice (encodeURIComponent in the page, then jQuery encodes the
// query again). Reproducing that exactly matters: the service decodes one layer
// itself and its handler decodes the second.
size_t buildCommentWriteUrl(char* out, size_t outCap, const char* novelId, uint32_t chapterId,
                            uint32_t paragraphId, const char* body, const char* author, const char* deviceId);

// Replies are a normal form POST; the body and author are encoded once.
size_t buildCommentReplyBody(char* out, size_t outCap, const char* novelId, uint32_t chapterId,
                             const char* commentId, const char* body, const char* author);

}  // namespace jjwxc

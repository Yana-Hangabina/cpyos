// Cross-device reading progress: the WAP read endpoint and the App write endpoint.
//
// Measured live on 2026-10-17:
//   POST https://m.jjwxc.net/app.jjwxc/Wap/ReadingProgress/getReadingProgress
//        novelid=<id>&chapterid=<local chapter>&source=wap&token=<base64(sid)>
//     -> {"code":"200","data":{"chapterid":"50","isVip":"1","text":"…",
//          "position":"1407","readingtime":"2026-10-17 14:10:58",
//          "source":"Android","readpercentage":"26.88%","showProgress":"1"}}
//     The Wap view hides records whose source is "wap" (this device's own writes);
//     the Pc view (/app.jjwxc/Pc/ReadingProgress/…) hides "pc". Both show Android.
//
//   POST https://android.jjwxc.net/readingProgressAndroid/readingProgress
//        readingprogressstring=<json array>&user_token=<base64(sid)>
//     source MUST be "wap": "Android" -> 500, "pc" -> 1022 (verified live).
//
// No heap, no STL, no Arduino/ESP headers.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace jjwxc {

struct ReadingProgress {
  bool valid = false;
  char chapterId[16] = {};
  uint32_t position = 0;
  char text[96] = {};
  char source[16] = {};
  char readingTime[24] = {};
  char readPercentage[12] = {};
};

// Parses the read endpoint's envelope. `valid` is false for an empty data object
// (which is what "local and cloud agree" looks like), while the function itself
// still returns true for a well-formed answer.
bool parseReadingProgress(const char* body, size_t len, ReadingProgress& out);

// True when the answer carried a record with a non-empty chapterid.
bool hasReadingProgress(const char* body, size_t len);

// True when the envelope's code is 200 (or 0). The write endpoint answers
// {"code":"200","message":...} with no data object, so hasReadingProgress()
// cannot be used to acknowledge a write.
bool readingProgressResponseOk(const char* body, size_t len);

// Builds `novelid&chapterid&source=wap&token=...` for the read endpoint.
size_t buildProgressReadBody(const char* novelId, const char* chapterId, const char* sid, char* out, size_t cap);

// Builds the Android write body. `readerId` is the 8-digit prefix of the sid
// (pass it explicitly so a caller that already parsed the jar does not have to
// split it twice). `text` may be empty; it is escaped and truncated on a UTF-8
// boundary. Returns 0 when the body would not fit.
size_t buildProgressWriteBody(const char* novelId, const char* readerId, const char* chapterId, uint32_t position,
                              const char* text, uint32_t readingTime, const char* sid, char* out, size_t cap);

// Copies the 8-digit reader id at the start of a sid (`<readerid>_<rest>`).
// Returns false when the sid does not start with exactly eight digits and '_'.
bool readerIdFromSid(const char* sid, char* out, size_t cap);

}  // namespace jjwxc

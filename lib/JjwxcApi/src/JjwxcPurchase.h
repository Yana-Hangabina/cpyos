// Chapter purchase over WAP.
//
// Verified end to end on 2026-10-17 with a real 9-point purchase. The form that
// matters is the password-confirmation POST -- the intermediate /buyvip page only
// renders that form, so the client can post straight to it (this is exactly what
// the verified script did):
//
//   POST https://m.jjwxc.net/buy/buy_vip
//        novel_id=<id>&chapterid[]=<id>&renew_status=0&buytype=chapters&
//        not_use_backpoint=0&checkps=0&shumeideviceId=get_deviceid_timeout&
//        password=<6 digits>&but=确实订购
//     -> "购买成功，花费晋江币9点"
//
// The price comes from the GET /buy/<novel>/<chapter> page. Unpurchased chapters
// are checkboxes named `chapterid[]` carrying `data-point`; purchased ones are
// named `buychapterid[]`, which is how a page can be read to tell "already owned"
// from "not in the list at all".
//
// No heap, no STL, no Arduino/ESP headers. Nothing here logs or stores the
// password.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace jjwxc {

struct ChapterPrice {
  bool found = false;      // present as an unpurchased chapterid[] input
  bool purchased = false;  // present as a buychapterid[] input
  uint32_t points = 0;     // data-point of the chapterid[] input
};

bool parseChapterPrice(const char* html, size_t len, const char* chapterId, ChapterPrice& out);

enum class PurchaseOutcome : uint8_t {
  Success,
  WrongPassword,
  Insufficient,
  AlreadyOwned,
  Unknown,
};

struct PurchaseResult {
  PurchaseOutcome outcome = PurchaseOutcome::Unknown;
  int points = 0;
  char message[160] = {};
};

// Parses the HTML answer of /buy/buy_vip. Success is the service's own sentence
// ("购买成功"), never a bare HTTP 200; the failure sentences are matched because
// this page, unlike login, does render them server-side.
PurchaseResult parsePurchaseResult(const char* html, size_t len);

// Body for the single verified POST. Returns 0 when anything would not fit; a
// truncated password must never be sent.
size_t buildBuyBody(const char* novelId, const char* chapterId, const char* password, char* out, size_t cap);

}  // namespace jjwxc

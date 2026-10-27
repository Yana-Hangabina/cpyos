// Main-device approval: the login fallback when a risk check demands a captcha
// this device cannot solve.
//
// The WAP login page already ships this flow (captured 2026-10-17):
//   POST /app.jjwxc/Wap/Login/sendVerifyNotifyToMainDevice  loginName=<name>
//     -> {"code":200,"data":{"authKey":"..."}}
//   POST /app.jjwxc/Wap/Login/askLoginRes  authKey=<key>&action=check
//     polled once per second, 60 times; {"code":200} means the user approved on
//     the phone. The page then puts authKey into the login form's hidden field
//     and submits it.
//
// Nothing here logs the login name or the auth key.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace jjwxc {

// Extracts data.authKey. False when the answer is not a code-200 envelope or the
// key is absent/empty.
bool parseAuthKey(const char* body, size_t len, char* out, size_t cap);

// True when askLoginRes answered code 200 (approval received). Any other code,
// including "still waiting", is false.
bool parseApproval(const char* body, size_t len);

size_t buildNotifyBody(const char* loginName, char* out, size_t cap);
size_t buildAskBody(const char* authKey, char* out, size_t cap);

}  // namespace jjwxc

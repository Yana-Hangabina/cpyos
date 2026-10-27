// Favourites: class (folder) list parsing and the form/answer shapes of the
// mutation endpoints the WAP pages themselves call.
//
// Measured live on 2026-10-17 (device session, m.jjwxc.net):
//   POST /app.jjwxc/Wap/Favorite/addNovelFavorite
//        novelidstr=<id>&chapterid=0&classid=<id>          -> {"code":200,...}
//   POST /app.jjwxc/Wap/Favorite/editNovelFavoriteClass
//        novelids=<id>&chapterid=0&classid=<id>            -> code 200
//   POST /app.jjwxc/Pc/Favorite/deleteNovelFavorite
//        novelids=<id>&isread=0                            -> code 200
//   POST /app.jjwxc/Wap/FavoriteClass/addFavoriteClassname
//        sort=0&classname=<name>                          -> code 200
//   POST /app.jjwxc/Wap/FavoriteClass/editFavoriteClassname
//        newclassname=<name>&sort=0&classid=<id>          -> code 200
//   POST /app.jjwxc/Wap/FavoriteClass/delByClassid
//        classid=<id>                                     -> code 200
//   POST /app.jjwxc/Wap/FavoriteClass/saveClassSort
//        sort=<json array>                                -> code 200
//
// The folder page /my/favlist renders:
//   <div class="one_class [can_edit]" data-classid="-5">
//     <div class="class_name class_name_-5">一周动态</div>
//   </div>
// Negatives are the site's built-in pseudo views (-5 weekly, -1 recent, -2 temp,
// -3 local import); 0 is "uncategorised" and positives are user folders. The
// parser reports them all and leaves the policy to the caller.
//
// No heap, no STL, no Arduino/ESP headers.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace jjwxc {

struct FavoriteClass {
  char id[16] = {};
  char name[48] = {};
};

using FavoriteClassSink = bool (*)(void* ctx, const FavoriteClass& item, size_t index);

size_t parseFavoriteClasses(const char* html, size_t len, FavoriteClassSink sink, void* ctx);

// What one mutation answered. `ok` is deliberately narrower than "HTTP 200": the
// endpoints always answer 200 and put the real verdict in `code`.
struct FavoriteResult {
  int code = -1;
  bool ok = false;
  char message[128] = {};
};

bool parseFavoriteResult(const char* body, size_t len, FavoriteResult& out);

// Favourite state from GET /book2/fv/<novelId>.
//
// The page is the site's own favourite/change-class page. Measured 2026-10-19:
//   already favourited -> contains `editNovelFavClass(...)` and "已成功收藏文章"
//   not favourited     -> contains `addNovelFavClass(...)` (the add button)
// `known` is false for any other shape (including the login page), so a caller
// can choose to hide the destructive action rather than guess.
struct FavoriteStatus {
  bool known = false;
  bool isFavorite = false;
};
bool parseFavoriteStatus(const char* html, size_t len, FavoriteStatus& out);

// The authoritative, READ-ONLY status endpoint used by the book page itself:
//   POST /app.jjwxc/Wap/Favorite/favoriteStatus   favnovelid=<id>
//   -> {"code":"200","data":[{"status":"1","classid":"0",
//        "class_name":"尚未分类"}],"message":""}
// status "1" = favourited, "0" = not. GET /book2/fv/<id> is the ADD flow (it
// answers "已成功收藏"), so it must never be used as a status probe.
struct FavoriteStatusInfo {
  bool known = false;
  bool isFavorite = false;
  char classId[16] = {};
  char className[32] = {};
};
bool parseFavoriteStatusResponse(const char* body, size_t len, FavoriteStatusInfo& out);
size_t buildFavoriteStatusBody(const char* novelId, char* out, size_t cap);

// Form bodies, already urlencoded. All return the body length, or 0 when an
// input would not fit -- callers must not POST a truncated body.
size_t buildFavoriteAddBody(const char* novelId, const char* classId, char* out, size_t cap);
size_t buildFavoriteMoveBody(const char* novelIds, const char* classId, char* out, size_t cap);
size_t buildFavoriteDeleteBody(const char* novelIds, char* out, size_t cap);
size_t buildFavoriteClassCreateBody(const char* name, char* out, size_t cap);
size_t buildFavoriteClassRenameBody(const char* classId, const char* name, char* out, size_t cap);
size_t buildFavoriteClassDeleteBody(const char* classId, char* out, size_t cap);

}  // namespace jjwxc

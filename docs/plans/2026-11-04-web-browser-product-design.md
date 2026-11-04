# Web Browser — Product & UX Design (v1)

Branch `feat/webbrowser` (worktree `repos/crossmux-browser`). Design only: no source changes here.
It specifies the next iteration on the working prototype (Home / Loading / Reading / Error / link
picker / RAM history), read alongside `docs/engineering/web-browser.md`, `StatusCards.h`,
`OptionPopup.h` and the device shots `t1.png` (ZH chapter), `v2.png` (next chapter + nav),
`v4.png` (link picker), `g6.png` (EN book).

## 1. Positioning

A pocket text-first browser: paste a chapter URL, read it like a book, tap onward links, and come
back later on the same page. Jobs: read a chapter on a site with no app (page 1 in <6 s, correct
Chinese); follow a serial forward (link picker → next chapter in ≤3 taps); keep the place across
sleep/exit (reopen in 2 taps); read a long English article (no right-edge clipping); escape a dead
end (one clear reason, one way out).

"Good" on this panel = one text column with real 16-level AA, no chrome noise, nothing on screen
that cannot be acted on, every wait cancellable, every failure named, one panel scan per turn.

**What the screenshots say (P0 fixes).** ① Adjacent inline links concatenate — v2 reads
`下一章上一章 目录 设置`; insert one thin gap and let the run wrap as its own meta line. ② Block
boundaries are lost — t1 shows heading `1、众生的苗床` sharing a clipped line with `[欢迎…`; a
heading must be its own block, measured *and* drawn bold. ③ The footer carries five hints plus the
counter (`<> 翻页 确认 链接 返回 后退 上 地址 1 / 18`); cut to four. ④ Nothing says "you can
bookmark this", and leaving loses the place. ⑤ Site nav ahead of content eats ~200 px of 1216;
prefer `<main>`/`<article>` first when present.

## 2. Navigation model

```
Menu ──地址…──► Home ──open link/row──► Loading ──ok──► Reading
 ▲                ▲                        │ fail          │ ▲
 └── Up ──────────┘  Back=exit app          └──► Error ◄─────┘ │
      Reading ◄────────────────────────────── Back: page-1    │
         └── Bookmarks / History (child pages; Back returns) ──┘
```

Three levels only: **Home → Reading → Menu**. Bookmarks/History are child pages of their opener,
the link picker is an overlay; Back never jumps to Home.

| Where | Back does |
|---|---|
| Reading, page > 1 | previous page (no network, same URL) |
| Reading, page 1, URL stack > 1 | previous URL at its saved page |
| Reading, page 1, stack == 1 | leave app → Apps grid |
| Menu / link picker | close overlay |
| Bookmarks / History | return to opener, same page/row |
| Home | leave app |
| Loading | cancel: `正在停止…`, leave ≤3 s, discard the arriving result |

**Address entry** — one entry point (Home address pill; identical `地址…` item in Menu). Opens the
existing keyboard prefilled with the current URL, cursor at end. On confirm: trim, add `https://`
when no scheme, upgrade `http://` (+ toast), reject other schemes / non-ASCII hosts inline (in the
overlay, not a page). No search engine.

**Link picker** — `OptionPopup`, title `本页链接`, ≤8 links in reading order, ordinal prefix
(`1 下一章`), 40-char truncation. Empty page → toast `本页没有链接`, no dialog. Selection navigates
in place (page 1, push URL).

| Context | Back | Confirm | Left / Right | Up | Touch |
|---|---|---|---|---|---|
| Home | exit app | open row | move selection | focus address pill | pill = keyboard; row = open; cards = lists |
| Reading | §above | link picker | prev / next page | **Menu** | link = follow; tap left 30 % prev, right 30 % next, middle 40 % Menu; back disc = Back; title/host band = Menu; swipe L/R = turn |
| Menu | close | run item | move item | — | tap item; outside = close |
| Link picker | close | follow link | move item | — | tap row; outside = close |
| Bookmarks / History | return | open entry | move row | Menu (row actions) | row = open; tail trash = delete; swipe = scroll |
| Loading | cancel | — | — | — | Cancel pill |
| Error | back | retry | — | — | Retry pill |

`Up` is now **Menu everywhere** (address entry moves inside it; the `上 地址` hint disappears). Only
muscle-memory break in this iteration, and it makes reload/bookmark/lists discoverable.

## 3. Screen specifications

Shared shell (684×1216, margin 28): header y 28–140 — back disc Ø64, title `UI_10` BOLD one line
ellipsised, battery pill right, host `SMALL` under the title, 1 px rule; footer y 1120–1196 — hints
`SMALL` left, counter right, 1 px rule. No new fonts or bitmaps: reuse `icon_bookmark_32` /
`icon_timer_48` and the theme's pill/card primitives.

| Screen | Layout / content | Actions, states |
|---|---|---|
| **Home** | Header `浏览器` + battery, no back disc; address pill y 148 h 64 r 32 2 px ring + globe + `输入网址`; one card, rows `书签 12 ›` / `历史 50 ›` (h 88, chevron); `最近访问` + ≤5 rows h 112 (title `UI_10`; `host · 第 N 页 · 今天 14:32` in `SMALL`). | Pill → keyboard; row → open at saved page; cards → lists. Empty: `还没有访问记录` / `粘贴一个网址开始阅读`. Footer `确认 打开 · 上 地址 · 返回 退出`. No "reload last" button — row 1 is that page. |
| **Loading** | `status_card::paintWait` over Home/Reading (never blank, one push, no `clearScreen`); 560×328 card: `正在载入`, host, elapsed `3.2 s`, duck swim (`permille < 0`, `phaseMs`), Cancel pill. | After 20 s append `网络较慢…`; 45 s → Error; Cancel → `正在停止…`, leave ≤3 s, discard result. Footer hints suppressed. |
| **Reading** | Header = `<title>` (`UI_10` bold, ellipsis) + final-URL host, tap = Menu; meta heading blocks on their own lines, bold, wrap ≤2; nav link runs on their own line; body x 40–644, y 168–1108, `CJK_READER_SIZE_FONT_ID`, 16-level canvas, indent kept, links underlined with a 1 px gap between runs. | L/R or left/right thirds turn; Confirm = picker; Up = Menu; Back per §2; bookmarked page shows a 20×26 filled corner mark (top-right). Footer `<> 翻页 · 确认 链接 · 上 菜单 · 返回 后退` + `3 / 18`. No URL bar or progress bar. |
| **Error** | Nothing loaded: full page `无法打开此页`, host `SMALL`, reason ≤4 wrapped lines `SMALL`, footer `确认 重试 · 返回 后退`. | A document already loaded (reload/nav failure): `status_card::paintMessage` over the existing page; Back closes the card. |
| **Link picker** | `OptionPopup` `本页链接`, ≤8 rows in reading order, ordinal prefix (`1 下一章`), 40-char truncation, 2 px ring selection (as in v4), 12.5 % scrim, one overlay push. | Empty page → toast `本页没有链接`, no dialog; tap/Confirm follows in place (page 1, push URL). |
| **Menu** | `OptionPopup` `页面菜单`, ≤6 items: `地址…` · `刷新本页` · `加入书签` / `移除此页书签` · `书签列表` · `历史` · `返回主页`. | Item 3 is state-dependent; reload keeps the page index when the new document is long enough, else page 1 + `页面已变，已跳到开头`. |
| **Bookmarks list** | Child page: `书签 · 12` + back disc; rows h 112 — title `UI_10`, `host · 第 N 页 · 2026-11-04` `SMALL`, tail trash 56×56; footer `确认 打开 · 上 菜单 · 返回 返回`. | Open → navigate then jump to `page` (clamped; clamp → toast). Empty: `还没有书签` + `阅读页按「上」→ 加入书签`. |
| **History list** | Same geometry, `历史 · 50`, newest first; time `今天 14:32` / `MM-DD` / `YYYY-MM-DD`, `—` when `visitedAt == 0`. | Open restores the last read page. Empty: `还没有访问记录`. |
| **Delete / clear** | `components/InlineConfirm.h` (as in reader/library), two options, focus Cancel: `删除这条记录？` / `清除全部 50 条？`. | Confirm → write + toast `已删除` / `历史已清除`; clear-all only from the list's Up-menu. |

## 4. This iteration's features

| Feature | Behaviour |
|---|---|
| Bookmark current page | Menu toggle, key = normalized URL + page; toast; corner mark updates without re-fetch |
| Bookmarks list | §3, open / delete / clear-all, survives reboot |
| Visit history | Written on leaving Reading (Back to Home, exit, navigation away) and every 10 turns; MRU; dedupe by normalized URL (lowercase scheme+host, drop `#fragment`); cap 50; `page` updated in place |
| Open current page | Lists and Home recents restore the saved page, not page 1 |
| Address entry | §2, validated, `http→https` notice |
| Reload | Menu item; keeps page when possible; never reuses a stale body |

Storage in `/.crosspoint/browser/`, WPL1 text:

| File | Cap | Order | Notes |
|---|---|---|---|
| `history.txt` | 50 records / ≤48 KiB | MRU first | dedupe by URL, `page` = last read page |
| `bookmarks.txt` | 100 records / ≤64 KiB | newest first | dedupe by URL+page |

```
WPL1\t1\n
url\ttitle\tvisitedAt\tpage\n      // url ASCII, title UTF-8, \\ \t \n escaped, page 0-based
```
Writer: `<file>.tmp` then `Storage.replaceFile` (atomic, same rule as `annotations.bin`). Reader:
bad header → behave as empty and log once, but do **not** delete the file until the next successful
write; malformed line → skip; over cap → keep newest N; eviction by age, silent.

Budgets: `BrowserStore` ≈5 KB + one generic list page ≈6–7 KB + wiring ≤2 KB + strings ≈7–9 KB ⇒
**≤20 KB of the ~200 KB flash margin**. Static RAM ≤64 B; entries in PSRAM (150 records ≈105 KB
worst case) loaded only while a list is open, freed in `onExit`. No new font/bitmap/component copy.

## 5. Robustness

| Situation | UX | Key — EN / ZH |
|---|---|---|
| No saved network / association fails | Error page, Back only; **never** push the Wi-Fi UI | `STR_BRW_ERR_OFFLINE` No Wi-Fi available / 没有可用的 Wi-Fi |
| DNS fails | Error page, host echoed | `STR_BRW_ERR_DNS` Can't look up that host / 无法解析域名 |
| TLS / connect fails | Error page + Retry | `STR_BRW_ERR_TLS` Secure connection failed / 安全连接失败 |
| HTTP ≥400 | Error page with the code | `STR_BRW_ERR_HTTP_FMT` Server returned %d / 服务器返回 %d |
| Non-HTML (image/pdf/zip) | Error page naming the type, no retry loop | `STR_BRW_ERR_NOT_HTML_FMT` Not a web page (%s) / 这不是网页（%s） |
| HTML with no extractable text (JS shell) | Named error + Back | `STR_BRW_ERR_EMPTY` No readable text on this page / 这一页没有可读的正文 |
| Markup > 1 MiB | Render the start, toast once | `STR_BRW_ERR_TOO_LARGE` Page is too large / 页面过大 + `STR_BRW_TOAST_TRUNCATED` Showing the start / 仅显示开头 |
| Charset/gzip undecodable | Error page — never feed mojibake to the tokenizer | `STR_BRW_ERR_ENCODING` Can't decode this page / 无法解码此页 |
| Bad address | Inline message in the keyboard overlay | `STR_BRW_ERR_BAD_URL` Unrecognised address / 网址无法识别 |
| 3xx chain | Follow ≤5, header shows final host, dashboard keeps the requested URL | (none) |
| Cancel while loading | `正在停止…` → leave ≤3 s, result discarded | `STR_BRW_STOPPING` / `STR_BRW_CANCELLED` |
| `http://` typed | Silent upgrade + one toast | `STR_BRW_UPGRADED_HTTPS` Switched to HTTPS / 已改用 HTTPS |

Non-negotiable: keep the existing PSRAM fetch task, IDLE priority and 45 s timeout (no timeout
tuning this iteration). Radio: reset the persistent TLS session **before** `releaseRadio()` and
release only when `wifiSessionActive_` — otherwise the documented `InstrFetchProhibited` panic.
Never delete a stuck TLS task in `onExit`; discard its late result. Only a fetch holds
`background_work`; list pages do not.

## 6. Acceptance (serial, on device)

Extend `CMD:BSTATUS` with `bm=<n> hist=<n> page=<p>/<n> stack=<n> url=<...>`; no new commands.

| # | Drive | Expect |
|---|---|---|
| 1 | `CMD:BROWSER` (empty) | Home with recents or empty state — shot `home` |
| 2 | `CMD:BROWSER https://example.com/` | Wait card over Home, then Reading; `page=1/1` — shots `loading`, `reading-web` |
| 3 | `CMD:BOPEN` a GBK chapter | Chinese, no tofu; `BSTATUS` final host — shot `reading-zh` |
| 4 | `CMD:BNEXT` ×3, `CMD:BPREV` | Counter moves, no `[ERR]`, no full-screen flash per turn |
| 5 | `CMD:BFIXTURE`, `CMD:BLINKS` | Picker `本页链接`, ≤8 rows, ring on selection — shot `links` |
| 6 | `CMD:BLINK 0`, `CMD:BBACK` | Navigates; Back returns to the previous URL at page 1 |
| 7 | `CMD:BBACK` on first URL | Returns to Apps grid; log `radio released`; `wifi=0` |
| 8 | Menu (Up) | 6 items, correct add/remove label — shot `menu` |
| 9 | Bookmark page 2, reopen from list | Same page; `bm` +1 — shots `bookmarks-empty`, `bookmarks` |
| 10 | Reboot, open from Home recents | Same URL and page (WPL1 persistence) |
| 11 | History after 3 sites | 3 rows, newest first, no duplicate URL — shot `history` |
| 12 | Delete one, clear all | Inline confirm, focus Cancel, counts → 0 — shot `confirm` |
| 13 | No Wi-Fi | Offline error page, Back only — shot `error-offline` |
| 14 | `CMD:BOPEN https://httpbin.org/status/404` | `服务器返回 404`, Retry + Back |
| 15 | `CMD:BOPEN <image url>` / a JS-only shell | `这不是网页（image/png）` / `No readable text` |
| 16 | Long article to the last page | No right clipping, exact counter, `CMD:GRAYSTATS` increments |

Shot set: `home`, `loading`, `reading-zh`, `reading-en`, `links`, `menu`, `bookmarks-empty`,
`bookmarks`, `history`, `confirm`, `error-offline`; each on a clean state.

## 7. Priorities

**P0** Up=Menu + 4-hint footer; address entry with validation and one notice; reload; page bookmarks
+ list; history + list + clear; restore page from lists/recents; WPL1 storage (atomic, capped);
loading card with cancel; the error table in §5; link-run spacing and heading-block fixes; all
strings via `tr()`.

**P1** Swipe page turn parity; "open Wi-Fi settings" on the offline page; cross-host redirect
notice; per-page "text only / keep nav links" toggle; held-key list scrolling; bookmark titles
derived from `<title>` (no editing).

**P2** Browser font size independent of the reader; find-on-page; offline page cache
(`.cache/`, 20 pages); per-site "start at page 1" memory.

**Not now (with reason).** JavaScript (no engine fits ~200 KB); images and inline image flow
(layout); forms/login/cookies (no model, invites credential handling); tabs (RAM + discoverability);
downloads; plain HTTP (SecureClient only); IDN hosts; CA bundle + clock check (meaningless before a
clock guard — `setInsecure()` stays documented, not "fixed"); smarter article extraction; ad
filtering; share/export.

## 8. Appendix

### 8.1 New i18n keys — EN / ZH (reuse `STR_CANCEL`, `STR_RETRY`, `STR_BACK`, `STR_SELECT`,
`STR_DELETE`, `STR_CLEAR`; † reuse if the prototype already has it)

```
STR_BRW_ADDRESS Address / 地址            STR_BRW_ENTER_ADDRESS Enter address / 输入网址
STR_BRW_RECENT Recent / 最近访问          STR_BRW_BOOKMARKS Bookmarks / 书签
STR_BRW_HISTORY History / 历史            STR_BRW_MENU Menu / 菜单
STR_BRW_HINT_TURN Turn / 翻页             STR_BRW_HINT_LINKS Links / 链接
STR_BRW_HINT_MENU Menu / 菜单             STR_BRW_HINT_BACK Back / 后退
STR_BRW_MENU_TITLE Page menu / 页面菜单   STR_BRW_RELOAD Reload page / 刷新本页
STR_BRW_ADD_BM Add bookmark / 加入书签    STR_BRW_REMOVE_BM Remove bookmark / 移除此页书签
STR_BRW_BOOKMARKS_TITLE Bookmarks / 书签列表
STR_BRW_HISTORY_TITLE History / 访问历史  STR_BRW_HOME Home / 返回主页
STR_BRW_LINKS_TITLE† Links on this page / 本页链接
STR_BRW_BM_ADDED Bookmark added / 已加入书签
STR_BRW_BM_REMOVED Bookmark removed / 已移除书签
STR_BRW_NO_LINKS No links on this page / 本页没有链接
STR_BRW_PAGE_MOVED Page changed; opened at start / 页面已变，已跳到开头
STR_BRW_TOAST_TRUNCATED Showing the start / 仅显示开头
STR_BRW_EMPTY_BM No bookmarks yet / 还没有书签
STR_BRW_EMPTY_HISTORY No history yet / 还没有访问记录
STR_BRW_EMPTY_HINT Press Up in a page → Add bookmark / 阅读页按「上」→ 加入书签
STR_BRW_DELETE_ENTRY Delete this entry / 删除这条
STR_BRW_CLEAR_ALL Clear all / 清除全部
STR_BRW_CONFIRM_DELETE Delete this entry? / 删除这条记录？
STR_BRW_CONFIRM_CLEAR_FMT Clear all %d entries? / 清除全部 %d 条？
STR_BRW_LOADING Loading / 正在载入    STR_BRW_SLOW Network is slow… / 网络较慢…
STR_BRW_STOPPING Stopping… / 正在停止…  STR_BRW_CANCELLED Cancelled / 已取消
STR_BRW_UPGRADED_HTTPS Switched to HTTPS / 已改用 HTTPS
STR_BRW_ERR_OFFLINE No Wi-Fi available / 没有可用的 Wi-Fi
STR_BRW_ERR_DNS Can't look up that host / 无法解析域名
STR_BRW_ERR_TLS Secure connection failed / 安全连接失败
STR_BRW_ERR_HTTP_FMT Server returned %d / 服务器返回 %d
STR_BRW_ERR_NOT_HTML_FMT Not a web page (%s) / 这不是网页（%s）
STR_BRW_ERR_EMPTY No readable text on this page / 这一页没有可读的正文
STR_BRW_ERR_TOO_LARGE Page is too large / 页面过大
STR_BRW_ERR_ENCODING Can't decode this page / 无法解码此页
STR_BRW_ERR_BAD_URL Unrecognised address / 网址无法识别
```

42 keys. Key-count change ⇒ **reflash the data partition** with the app, or the 33 packed languages
fall back to English.

### 8.2 New files / components

| Path | Purpose | Budget |
|---|---|---|
| `lib/WebBrowser/src/BrowserStore.{h,cpp}` | WPL1 codec, two files, caps, dedupe, atomic write | ≈5 KB text |
| `src/activities/apps/webbrowser/BrowserListActivity.{h,cpp}` | one list page, `Mode {Bookmarks, History}`, row model, tail trash, Up-menu, inline confirm | ≈6–7 KB text |
| `src/activities/apps/webbrowser/WebBrowserActivity.{h,cpp}` (edit) | menu state, bookmark/history wiring, Up=Menu, footer, page restore, error taxonomy | ≤2 KB delta |
| `lib/I18n/translations/{english,chinese}.yaml` (edit) | §8.1 | ≈7–9 KB |
| `test/webbrowser/` (edit) | WPL1 round-trip, dedupe, cap eviction, bad header, escapes, page clamp | 0 device cost |

Not built: a second history class, a separate menu class, new icons, new fonts, a settings page.
Menu/picker/confirm reuse `OptionPopup` / `InlineConfirm`; waits and errors reuse `status_card`;
transient feedback reuses `components/Toast.h`.

// CPYOS reader demo for the Metalio E-INK4 Plus.
//
// This is the first end-to-end reading path on the Plus that uses the real
// CPYOS book engine: discovery on SD -> ZIP/OPF parse -> Section layout ->
// Page render through GfxRenderer with the built-in CJK font. It deliberately
// bypasses ActivityManager (which is still blocked on the Arduino web-server
// and network APIs) so the reading core can be validated on hardware.
//
// Interaction: tap the left/right third or swipe, or use the cover prev/next
// keys. Chapter boundaries advance to the next/previous spine item.
//
// UNTESTED ON HARDWARE.

#include <Arduino.h>
#include <Epub.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <builtinFonts/notosans_cjk_12.h>

#include "PortContentCrypto.h"

#include <memory>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <strings.h>
#include <vector>

#include "CrossPointSettings.h"
#include "Page.h"
#include "Section.h"
#include "GfxRenderer.h"
#include "cpyos_sd.h"

namespace {

EpdFont cjkFont(&notosans_cjk_12);
EpdFontFamily cjkFamily(&cjkFont);
GfxRenderer* s_renderer = nullptr;

std::shared_ptr<Epub> s_book;
std::unique_ptr<Section> s_section;
ReaderRenderSpec s_spec;
int s_spine = 0;
int s_page = 0;
int s_margin = 24;
int s_header = 64;
int s_footer = 68;
char s_status[96] = "reader: no book";

enum class View { Shelf, Reader };
View s_view = View::Shelf;
std::vector<std::string> s_books;
int s_selected = 0;
int s_shelfTop = 0;
constexpr int kRowH = 64;
constexpr int kMaxBooks = 50;

constexpr int kBuildPagesPerTick = 8;

bool isEpubName(const char* name) {
    const size_t len = std::strlen(name);
    return len > 5 && strcasecmp(name + len - 5, ".epub") == 0;
}

// Depth-first collection of .epub paths, skipping dot-directories so the
// CPYOS cache (/.crosspoint) and macOS metadata are not scanned.
void collectEpubs(const std::string& dir, int depth) {
    if (depth > 4 || static_cast<int>(s_books.size()) >= kMaxBooks) return;
    HalFile root = Storage.open(dir.c_str());
    if (!root || !root.isDirectory()) return;
    root.rewindDirectory();
    HalFile entry = root.openNextFile();
    while (entry && static_cast<int>(s_books.size()) < kMaxBooks) {
        char name[192] = {0};
        entry.getName(name, sizeof(name));
        const std::string child = dir == "/" ? std::string("/") + name : dir + "/" + name;
        if (entry.isDirectory()) {
            if (name[0] != '.') collectEpubs(child, depth + 1);
        } else if (isEpubName(name)) {
            s_books.push_back(child);
        }
        entry = root.openNextFile();
    }
}

std::string baseName(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    return name;
}

bool buildSection(int spine, int startPage) {
    if (!s_book) return false;
    s_section = std::make_unique<Section>(s_book, spine, *s_renderer);
    if (!s_section) return false;
    if (!s_section->startBuild(s_spec)) {
        snprintf(s_status, sizeof(s_status), "layout failed (spine %d, err %d)", spine,
                 static_cast<int>(s_section->buildError()));
        return false;
    }
    int ticks = 0;
    while (s_section->isBuilding()) {
        if (!s_section->buildSomeMore(kBuildPagesPerTick)) {
            break;
        }
        // Yield regularly: a long chapter can take seconds, and the IDF task
        // watchdog needs IDLE time on this core while we build in the UI loop.
        if ((++ticks & 3) == 0) vTaskDelay(1);
    }
    s_spine = spine;
    s_page = startPage;
    if (s_page >= s_section->pageCount) s_page = s_section->pageCount > 0 ? s_section->pageCount - 1 : 0;
    return s_section->pageCount > 0;
}

bool openBook(const std::string& path) {
    s_book.reset();
    s_section.reset();
    s_book = std::make_shared<Epub>(path, "/.crosspoint");
    if (!s_book || !s_book->load(true, SETTINGS.embeddedStyle == 0)) {
        snprintf(s_status, sizeof(s_status), "EPUB load failed");
        s_book.reset();
        return false;
    }
    if (!buildSection(0, 0)) {
        snprintf(s_status, sizeof(s_status), "first chapter has no readable pages");
        return false;
    }
    snprintf(s_status, sizeof(s_status), "%s", path.c_str());
    return true;
}

void renderShelf() {
    GfxRenderer& r = *s_renderer;
    const int width = r.getScreenWidth();
    const int height = r.getScreenHeight();
    r.clearScreen(0xFF);
    r.drawText(1, s_margin, 16, "CPYOS \xC2\xB7 \xE4\xB9\xA6\xE5\xBA\x93", true);
    char line[64];
    snprintf(line, sizeof(line), "%u books", static_cast<unsigned>(s_books.size()));
    r.drawText(1, width - s_margin - 8 * static_cast<int>(strlen(line)), 16, line, true);

    const int top = s_header + 8;
    const int rows = (height - top - s_footer) / kRowH;
    for (int row = 0; row < rows; ++row) {
        const int index = s_shelfTop + row;
        if (index >= static_cast<int>(s_books.size())) break;
        const int y = top + row * kRowH;
        if (index == s_selected) {
            r.fillRect(s_margin - 8, y - 4, width - 2 * s_margin + 16, kRowH - 8, true);
            r.drawText(1, s_margin, y, baseName(s_books[index]).c_str(), false);
        } else {
            r.drawText(1, s_margin, y, baseName(s_books[index]).c_str(), true);
        }
    }
    if (s_books.empty()) {
        r.drawText(1, s_margin, top + 8, "no .epub on the SD card", true);
    }
    r.drawText(1, s_margin, height - s_footer, "tap a row / up-down to select", true);
    r.displayBuffer(HalDisplay::FULL_REFRESH);
}

void renderReader() {
    GfxRenderer& r = *s_renderer;
    const int width = r.getScreenWidth();
    const int height = r.getScreenHeight();
    r.clearScreen(0xFF);

    const char* title = s_book ? s_book->getTitle().c_str() : "no book";
    r.drawText(1, s_margin, 14, title, true);
    char line[96];
    snprintf(line, sizeof(line), "%d/%d  %d/%d", s_spine + 1,
             s_book ? s_book->getSpineItemsCount() : 0, s_page + 1,
             s_section ? s_section->pageCount : 0);
    r.drawText(1, width - s_margin - 8 * static_cast<int>(strlen(line)), 14, line, true);

    if (s_section) {
        auto page = s_section->loadPage(s_page);
        if (page) {
            page->render(r, 1, s_margin, s_header);
        } else {
            r.drawText(1, s_margin, s_header + 20, "page load failed", true);
        }
    } else {
        r.drawText(1, s_margin, s_header + 20, s_status, true);
    }

    r.drawText(1, s_margin, height - s_footer, s_status, true);
    r.drawText(1, s_margin, height - s_footer + 24, "tap edges / cover keys / swipe", true);
    r.displayBuffer(HalDisplay::FULL_REFRESH);
}

void turnPage(int delta) {
    if (!s_section) return;
    int page = s_page + delta;
    if (page >= s_section->pageCount) {
        if (s_spine + 1 < s_book->getSpineItemsCount() && buildSection(s_spine + 1, 0)) {
            snprintf(s_status, sizeof(s_status), "chapter %d", s_spine + 1);
        }
    } else if (page < 0) {
        if (s_spine > 0 && buildSection(s_spine - 1, 0x7FFF)) {
            snprintf(s_status, sizeof(s_status), "chapter %d", s_spine + 1);
        }
    } else {
        s_page = page;
    }
    renderReader();
}

}  // namespace

extern "C" void cpyos_reader_demo(void) {
    display.begin();
    static GfxRenderer renderer(display);
    s_renderer = &renderer;
    renderer.begin();
    freeink::content::portContentCryptoSelfTest();
    renderer.insertFont(1, cjkFamily);

    gpio.begin();
    const bool mounted = cpyos_sd_is_mounted();
    if (mounted) {
        Storage.begin();
        SETTINGS.loadFromFile();
    }

    const int width = renderer.getScreenWidth();
    const int height = renderer.getScreenHeight();
    s_spec = SETTINGS.readerRenderSpec(static_cast<uint16_t>(width - 2 * s_margin),
                                       static_cast<uint16_t>(height - s_header - s_footer));
    s_spec.fontId = 1;
    s_spec.collectTouchLinks = false;

    if (!mounted) {
        snprintf(s_status, sizeof(s_status), "SD card not mounted");
        renderReader();
        return;
    }
    collectEpubs("/", 0);
    s_view = View::Shelf;
    renderShelf();
}

extern "C" void cpyos_reader_loop(void) {
    gpio.beginFrame();
    gpio.update();

    float nx = 0.0f;
    float ny = 0.0f;
    float sx0 = 0.0f, sy0 = 0.0f, sx1 = 0.0f, sy1 = 0.0f;

    if (s_view == View::Shelf) {
        if (gpio.wasPressed(HalGPIO::BTN_DOWN) && !s_books.empty()) {
            s_selected = std::min(s_selected + 1, static_cast<int>(s_books.size()) - 1);
            renderShelf();
        } else if (gpio.wasPressed(HalGPIO::BTN_UP) && !s_books.empty()) {
            s_selected = std::max(s_selected - 1, 0);
            renderShelf();
        } else if (gpio.wasTouchTap(nx, ny) && !s_books.empty()) {
            // Panel-normalized tap -> portrait logical row.
            int lx = 0;
            int ly = 0;
            s_renderer->tapToLogical(nx, ny, lx, ly);
            const int top = s_header + 8;
            const int row = (ly - top) / kRowH;
            if (row >= 0) {
                const int index = s_shelfTop + row;
                if (index < static_cast<int>(s_books.size())) {
                    s_selected = index;
                    if (openBook(s_books[index])) {
                        s_view = View::Reader;
                        renderReader();
                        return;
                    }
                    renderShelf();
                }
            }
        }
        return;
    }

    if (gpio.wasPressed(HalGPIO::BTN_BACK)) {
        s_view = View::Shelf;
        s_book.reset();
        s_section.reset();
        renderShelf();
        return;
    }

    int delta = 0;
    if (gpio.wasTouchTap(nx, ny)) {
        delta = nx < (1.0f / 3.0f) ? -1 : (nx > (2.0f / 3.0f) ? 1 : 0);
    } else if (gpio.wasSwipe(sx0, sy0, sx1, sy1)) {
        delta = sx1 < sx0 ? 1 : -1;
    } else if (gpio.wasPressed(HalGPIO::BTN_DOWN)) {
        delta = 1;
    } else if (gpio.wasPressed(HalGPIO::BTN_UP)) {
        delta = -1;
    }
    if (delta != 0) {
        turnPage(delta);
    }
}

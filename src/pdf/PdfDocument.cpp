// The in-app engine and the XIP engine are mutually exclusive: under
// CROSSPOINT_PDF_XIP the code lives in the `pdfmod` partition and this
// translation unit must compile to nothing (same contract as
// PdfModuleEntry.cpp).  Without the XIP term here, a clean `readpico_pdf`
// build links this file against libmupdf, which that env deliberately does
// not link -- so the env only ever built by reusing a stale object file.
#if defined(CROSSPOINT_MUPDF) && !defined(CROSSPOINT_PDF_XIP)

#include "PdfDocument.h"
#include "PdfLineJoin.h"

#include "mupdf/fitz/glyph-cache.h"

// The module build must not link against app symbols: HalStorage/heap/logging
// resolve through the host table instead (PdfModuleHostGlue.h).
#if defined(CROSSPOINT_PDF_MODULE)
#include "PdfModuleHostGlue.h"
#else
#include <HalStorage.h>
#include <Logging.h>
#include <esp_debug_helpers.h>
#include <esp_heap_caps.h>
#endif
#include "PdfRenderBudget.h"
#include "PdfInk.h"
#include "PdfDeadline.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <new>
#include <vector>

// Added by patches/mupdf/source/fitz/image.c (the trimmed MuPDF build): a hard
// ceiling on decoded image bytes so a content-heavy page fails to a smaller
// band budget instead of exhausting PSRAM. Not in the stock 1.26.3 headers.
extern "C" void fz_set_image_budget(long long bytes);

namespace {

// Band height for the tiled path.  A band re-runs the whole page content, so
// fewer, taller bands are much faster; 384 rows is about three bands for a
// 674x873 page and needs a 259 KB pixmap.  Memory stays flat because the glyph
// cache is purged after every band (that, not the pixmap, was the hog).
constexpr int kPdfTileHeight = 384;

// Reused across tiles/pages.  Page resources can leave only ~40 KB of largest
// free block, so allocating an 86 KB tile at that point is what failed on the
// 112-page PDF; this buffer is allocated before the page is opened.
uint8_t* gPdfTileBuffer = nullptr;
size_t gPdfTileBufferSize = 0;

// Per-page wall clock for the fallback ladder.  The app stops waiting at 75 s
// and shows the card, but the worker would otherwise keep grinding rung after
// rung (each re-opens the document), holding the sync slot so the NEXT page
// waits for it.  90 s lets a page that would finish just past the wait still
// land, and bounds the post-timeout work to seconds instead of minutes.
constexpr uint32_t kLadderBudgetMs = 90000;
uint32_t gLadderDeadlineMs = 0;  // 0 = no per-page budget

bool ladderExpired() { return pdf_deadline::expired(gLadderDeadlineMs, millis()); }

// --- Emergency reserve ------------------------------------------------------
//
// Page 41 asks for one ~572 KB contiguous block late in its run while ~1 MB is
// still free but fragmented; no scale factor, image budget or AA setting moves
// that number.  This reserve is never touched by normal allocation: it is only
// handed out after the heap has already refused a request, so it stays one
// contiguous megabyte until the moment it is needed.  Freed blocks go back to a
// small free list so a page that needs two big buffers in sequence still works.
constexpr size_t kReserveBytes = 1u << 20;
constexpr size_t kReserveMin = 64u << 10;

uint8_t* gResBase = nullptr;
size_t gResBump = 0;

struct ResHeader {
  size_t size;
  ResHeader* next;  // free-list link while the block is free
};

ResHeader* gResFree = nullptr;

bool inReserve(const void* p) {
  const auto* q = static_cast<const uint8_t*>(p);
  return gResBase && q >= gResBase && q < gResBase + kReserveBytes;
}

void reserveInit() {
  if (gResBase) return;
  gResBase = static_cast<uint8_t*>(heap_caps_malloc(kReserveBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  gResBump = 0;
  gResFree = nullptr;
  if (gResBase) {
    LOG_ERR("PDF", "emergency reserve: %u bytes", static_cast<unsigned>(kReserveBytes));
  }
}

void reserveRelease() {
  if (!gResBase) return;
  heap_caps_free(gResBase);
  gResBase = nullptr;
  gResBump = 0;
  gResFree = nullptr;
}

void* reserveAlloc(size_t size) {
  if (!gResBase) return nullptr;
  const size_t aligned = (size + 15) & ~static_cast<size_t>(15);
  for (ResHeader** link = &gResFree; *link; link = &(*link)->next) {
    ResHeader* block = *link;
    if (block->size >= aligned) {
      *link = block->next;
      return block + 1;
    }
  }
  const size_t need = sizeof(ResHeader) + aligned;
  if (gResBump + need > kReserveBytes) return nullptr;
  auto* header = reinterpret_cast<ResHeader*>(gResBase + gResBump);
  header->size = aligned;
  gResBump += need;
  return header + 1;
}

void reserveFree(void* ptr) {
  auto* header = static_cast<ResHeader*>(ptr) - 1;
  header->next = gResFree;
  gResFree = header;
}

// MuPDF writes warnings through the context callback.  Left at the default they
// go to the serial console one blocking USB write at a time; a figure-heavy
// book produced thousands of "FT_Render_Glyph(...MONO): cannot render this glyph
// format" lines and that I/O alone stretched the background page cache to
// ~97 s/page.  They are not actionable for the reader, so drop them.
void quietWarning(void*, const char*) {}

void* psramMalloc(void*, size_t size) {
  void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p && size >= kReserveMin) {
    p = reserveAlloc(size);
    if (p) LOG_ERR("PDF", "alloc %u served from reserve", static_cast<unsigned>(size));
  }
  if (!p) {
    LOG_ERR("PDF", "PSRAM alloc %u failed: free=%u largest=%u", static_cast<unsigned>(size),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
  }
  return p;
}

void* psramRealloc(void*, void* ptr, size_t size) {
  void* p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) {
    LOG_ERR("PDF", "PSRAM realloc %u failed: free=%u largest=%u", static_cast<unsigned>(size),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    if (!inReserve(ptr) && size >= kReserveMin) {
      if (void* q = reserveAlloc(size)) {
        const size_t old = ptr ? heap_caps_get_allocated_size(ptr) : 0;
        if (ptr && old) std::memcpy(q, ptr, old < size ? old : size);
        if (ptr) heap_caps_free(ptr);
        LOG_ERR("PDF", "realloc %u served from reserve", static_cast<unsigned>(size));
        return q;
      }
    }
    if (size >= 131072) {
      LOG_ERR("PDF", "large realloc %u failed; caller backtrace:", static_cast<unsigned>(size));
      esp_backtrace_print(14);
    }
  }
  return p;
}

void psramFree(void*, void* ptr) {
  if (inReserve(ptr)) {
    reserveFree(ptr);
    return;
  }
  heap_caps_free(ptr);
}

// One fz_stream over a HalFile.  MuPDF's file stream uses stdio, which cannot
// see the SdFat volume CrossMux mounts, so the PDF reader feeds it this.
struct HalStreamState {
  HalFile file;
  unsigned char buffer[4096];
};

int nextHalFile(fz_context* ctx, fz_stream* stm, size_t) {
  auto* state = static_cast<HalStreamState*>(stm->state);
  const int n = state->file.read(state->buffer, sizeof(state->buffer));
  if (n < 0) fz_throw(ctx, FZ_ERROR_SYSTEM, "PDF read failed");
  stm->rp = state->buffer;
  stm->wp = state->buffer + n;
  stm->pos += n;
  if (n == 0) return EOF;
  return *stm->rp++;
}

void seekHalFile(fz_context* ctx, fz_stream* stm, int64_t offset, int whence) {
  auto* state = static_cast<HalStreamState*>(stm->state);
  int64_t base = 0;
  if (whence == SEEK_CUR) {
    base = stm->pos;
  } else if (whence == SEEK_END) {
    base = static_cast<int64_t>(state->file.fileSize64());
  }
  const int64_t target = base + offset;
  if (target < 0 || !state->file.seek64(static_cast<uint64_t>(target))) {
    fz_throw(ctx, FZ_ERROR_SYSTEM, "PDF seek failed");
  }
  stm->pos = target;
  stm->rp = state->buffer;
  stm->wp = state->buffer;
}

void dropHalFile(fz_context*, void* state) { delete static_cast<HalStreamState*>(state); }

void put16(unsigned char* p, uint16_t v) {
  p[0] = static_cast<unsigned char>(v);
  p[1] = static_cast<unsigned char>(v >> 8);
}

void put32(unsigned char* p, uint32_t v) {
  p[0] = static_cast<unsigned char>(v);
  p[1] = static_cast<unsigned char>(v >> 8);
  p[2] = static_cast<unsigned char>(v >> 16);
  p[3] = static_cast<unsigned char>(v >> 24);
}

bool writeBmp(fz_context* ctx, fz_pixmap* pix, const std::string& path) {
  const int width = fz_pixmap_width(ctx, pix);
  const int height = fz_pixmap_height(ctx, pix);
  const int stride = fz_pixmap_stride(ctx, pix);
  const unsigned char* samples = fz_pixmap_samples(ctx, pix);
  if (width <= 0 || height <= 0 || !samples) return false;

  // 8-bit greyscale BMP: Bitmap::readNextRow(..., Gray8) maps the palette to
  // luminance, and drawBitmapGrayscale16() turns it into the panel's 16 levels.
  const uint32_t rowBytes = (static_cast<uint32_t>(width) + 3u) & ~3u;
  const uint32_t imageSize = rowBytes * static_cast<uint32_t>(height);
  const uint32_t dataOffset = 14u + 40u + 256u * 4u;

  unsigned char header[54] = {};
  header[0] = 'B';
  header[1] = 'M';
  put32(header + 2, dataOffset + imageSize);
  put32(header + 10, dataOffset);
  put32(header + 14, 40u);
  put32(header + 18, static_cast<uint32_t>(width));
  put32(header + 22, static_cast<uint32_t>(height));
  put16(header + 26, 1u);
  put16(header + 28, 8u);
  put32(header + 30, 0u);
  put32(header + 34, imageSize);
  put32(header + 38, 2835u);
  put32(header + 42, 2835u);
  put32(header + 46, 256u);
  put32(header + 50, 256u);

  HalFile out;
  if (!Storage.openFileForWrite("PDF", path.c_str(), out)) return false;
  if (out.write(header, sizeof(header)) != sizeof(header)) return false;

  unsigned char palette[256 * 4];
  for (int i = 0; i < 256; ++i) {
    palette[i * 4 + 0] = static_cast<unsigned char>(i);
    palette[i * 4 + 1] = static_cast<unsigned char>(i);
    palette[i * 4 + 2] = static_cast<unsigned char>(i);
    palette[i * 4 + 3] = 0;
  }
  if (out.write(palette, sizeof(palette)) != sizeof(palette)) return false;

  std::vector<unsigned char> row(rowBytes);
  for (int y = height - 1; y >= 0; --y) {
    std::fill(row.begin(), row.end(), 0);
    std::memcpy(row.data(), samples + static_cast<size_t>(y) * stride, static_cast<size_t>(width));
    if (out.write(row.data(), row.size()) != row.size()) return false;
  }
  out.close();
  return true;
}

// BMP header/palette for a full-page image, written once before the strips.
bool writeBmpHeader(HalFile& out, int width, int height) {
  if (width <= 0 || height <= 0) return false;
  const uint32_t rowBytes = (static_cast<uint32_t>(width) + 3u) & ~3u;
  const uint32_t imageSize = rowBytes * static_cast<uint32_t>(height);
  const uint32_t dataOffset = 14u + 40u + 256u * 4u;

  unsigned char header[54] = {};
  header[0] = 'B';
  header[1] = 'M';
  put32(header + 2, dataOffset + imageSize);
  put32(header + 10, dataOffset);
  put32(header + 14, 40u);
  put32(header + 18, static_cast<uint32_t>(width));
  put32(header + 22, static_cast<uint32_t>(height));
  put16(header + 26, 1u);
  put16(header + 28, 8u);
  put32(header + 30, 0u);
  put32(header + 34, imageSize);
  put32(header + 38, 2835u);
  put32(header + 42, 2835u);
  put32(header + 46, 256u);
  put32(header + 50, 256u);
  if (out.write(header, sizeof(header)) != sizeof(header)) return false;

  unsigned char palette[256 * 4];
  for (int i = 0; i < 256; ++i) {
    palette[i * 4 + 0] = static_cast<unsigned char>(i);
    palette[i * 4 + 1] = static_cast<unsigned char>(i);
    palette[i * 4 + 2] = static_cast<unsigned char>(i);
    palette[i * 4 + 3] = 0;
  }
  return out.write(palette, sizeof(palette)) == sizeof(palette);
}

// Write a strip (bottom-up, matching Bitmap's BMP reader) as compact BMP rows.
bool writeBmpRows(HalFile& out, fz_context* ctx, fz_pixmap* pix, int width) {
  const int height = fz_pixmap_height(ctx, pix);
  const int stride = fz_pixmap_stride(ctx, pix);
  const unsigned char* samples = fz_pixmap_samples(ctx, pix);
  if (height <= 0 || width <= 0 || !samples) return false;
  const uint32_t rowBytes = (static_cast<uint32_t>(width) + 3u) & ~3u;
  std::vector<unsigned char> row(rowBytes);
  for (int y = height - 1; y >= 0; --y) {
    std::fill(row.begin(), row.end(), 0);
    std::memcpy(row.data(), samples + static_cast<size_t>(y) * stride, static_cast<size_t>(width));
    if (out.write(row.data(), row.size()) != row.size()) {
      LOG_ERR("PDF", "BMP row write failed at row %d/%d", y, height);
      return false;
    }
  }
  return true;
}

}  // namespace

PdfDocument::PdfDocument(std::string path) : path_(std::move(path)) {}

namespace {
void logPsram(const char* step) {
  LOG_ERR("PDF", "close %-18s psram free=%u largest=%u", step,
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}
}  // namespace

PdfDocument::~PdfDocument() {
  logPsram("enter");
  close();
  logPsram("after close");
  reserveRelease();
  logPsram("after reserve");
  if (gPdfTileBuffer) {
    heap_caps_free(gPdfTileBuffer);
    gPdfTileBuffer = nullptr;
    gPdfTileBufferSize = 0;
  }
  logPsram("after tile buffer");
}

bool PdfDocument::load() {
  // The MuPDF/FreeType globals that must be zero live in this module's .bss, and
  // the LOADER zeroes them through the alias and reads them back before calling
  // the entry (PdfModuleRuntime).  An earlier version of this function also
  // memset _ext_ram_bss_start here; that symbol is a linker LAYOUT symbol of the
  // APP image (host_provides exported it), so every open zeroed the app's
  // EXT_RAM_BSS_ATTR globals -- which after a Wi-Fi session hold live lwIP state,
  // producing `assert xQueueSemaphoreTake ((pxQueue))` in tcpip_thread during the
  // first page render. Never zero anything through an app layout symbol.

  close();
  passwordRequired_ = false;
  passwordWrong_ = false;

  fz_alloc_context alloc{nullptr, psramMalloc, psramRealloc, psramFree};
  LOG_ERR("PDF", "load %s psram free=%u largest=%u", path_.c_str(),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
  ctx_ = fz_new_context(&alloc, nullptr, FZ_STORE_UNLIMITED);
  if (ctx_) fz_set_warning_callback(ctx_, quietWarning, nullptr);
  if (!ctx_) return false;

  auto* state = new (std::nothrow) HalStreamState();
  if (!state) {
    close();
    return false;
  }
  if (!Storage.openFileForRead("PDF", path_.c_str(), state->file)) {
    delete state;
    close();
    return false;
  }

  bool ok = false;
  fz_var(ok);
  fz_try(ctx_) {
    stream_ = fz_new_stream(ctx_, state, nextHalFile, dropHalFile);
    stream_->seek = seekHalFile;
    fz_register_document_handlers(ctx_);
    doc_ = fz_open_document_with_stream(ctx_, "pdf", stream_);
    // Opening the stream succeeds for an encrypted document; page access then
    // returns blank content while count_pages still works.  Authenticate here
    // (empty string when no password was supplied) so the reader can ask for
    // one instead of showing blank pages.  This was missing entirely: every
    // password-protected PDF opened "successfully" and rendered white.
    if (fz_needs_password(ctx_, doc_)) {
      const char* supplied = password_.empty() ? "" : password_.c_str();
      if (!fz_authenticate_password(ctx_, doc_, supplied)) {
        passwordRequired_ = true;
        passwordWrong_ = !password_.empty();
        fz_throw(ctx_, FZ_ERROR_GENERIC, "password required");
      }
    }
    pageCount_ = fz_count_pages(ctx_, doc_);
    LOG_ERR("PDF", "opened: pages=%d psram free=%u largest=%u", pageCount_,
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    ok = pageCount_ > 0;
  }
  fz_catch(ctx_) {
    // A missing/rejected password is not a broken file; the reader turns it
    // into the password prompt and keeps the message out of the log noise.
    if (!passwordRequired_) LOG_ERR("PDF", "load failed: %s", fz_caught_message(ctx_));
    ok = false;
  }
  if (!ok) {
    close();
    return false;
  }
  // Emergency reserve is implemented but disabled: measured on pages 41/28 it
  // served at most one allocation and both pages still failed, while the 1 MB
  // it holds comes straight out of the page working set.  Keep the code so the
  // idea can be revisited with a smaller reserve, but do not spend the memory.
  loadOutline();
  return true;
}

void PdfDocument::loadOutline() {
  outline_.clear();
  if (!ctx_ || !doc_) return;

  fz_outline* root = nullptr;
  fz_var(root);
  fz_try(ctx_) { root = fz_load_outline(ctx_, doc_); }
  fz_catch(ctx_) { root = nullptr; }
  if (!root) return;

  std::function<void(fz_outline*, int)> walk = [&](fz_outline* node, int depth) {
    for (; node && outline_.size() < 500; node = node->next) {
      const int page = node->page.page;
      if (node->title && page >= 0 && page < pageCount_) {
        outline_.push_back({node->title, page});
      }
      if (node->down && depth < 8) walk(node->down, depth + 1);
    }
  };
  walk(root, 0);
  fz_drop_outline(ctx_, root);
}

static void skipFillImage(fz_context* ctx, fz_device* dev, fz_image* img, fz_matrix ctm, float alpha,
                          fz_color_params colorParams) {
  (void)ctx;
  (void)dev;
  (void)img;
  (void)ctm;
  (void)alpha;
  (void)colorParams;
}

// Image *masks* are a separate device entry.  A figure drawn as a 1-bit mask
// (a scanned chart, a stencil) never reaches fill_image, so skipping only that
// one left pages like #6 still allocating several MB with "images skipped".
static void skipFillImageMask(fz_context* ctx, fz_device* dev, fz_image* img, fz_matrix ctm,
                              fz_colorspace* colorspace, const float* color, float alpha,
                              fz_color_params colorParams) {
  (void)ctx;
  (void)dev;
  (void)img;
  (void)ctm;
  (void)colorspace;
  (void)color;
  (void)alpha;
  (void)colorParams;
}


static bool renderTiles(fz_context* ctx, fz_page* page, fz_matrix ctm, fz_irect bbox, int tileHeight,
                        const std::string& bmpPath, bool skipImages = false, bool useDisplayList = false) {
  HalFile out;
  if (!Storage.openFileForWrite("PDF", bmpPath.c_str(), out)) return false;

  fz_display_list* list = nullptr;
  fz_pixmap* pix = nullptr;
  bool ok = false;
  int tilesDone = 0;
  bool allBands = true;  // a budget break leaves a SHORT bmp: never call that a page
  fz_var(list);
  fz_var(pix);
  fz_var(ok);
  fz_var(tilesDone);
  LOG_ERR("PDF", "tiles start %dx%d tile=%d skipImg=%d free=%u largest=%u", bbox.x1 - bbox.x0, bbox.y1 - bbox.y0,
          tileHeight, skipImages ? 1 : 0, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
  fz_try(ctx) {
    const int width = bbox.x1 - bbox.x0;
    const int height = bbox.y1 - bbox.y0;
    if (width <= 0 || height <= 0) fz_throw(ctx, FZ_ERROR_GENERIC, "empty tile bbox");
    if (!writeBmpHeader(out, width, height)) fz_throw(ctx, FZ_ERROR_GENERIC, "BMP header write failed");

    if (useDisplayList) {
      fz_try(ctx) { list = fz_new_display_list_from_page(ctx, page); }
      fz_catch(ctx) { list = nullptr; }
    }

    // One tile pixmap at a time.  The old full-page pixmap (1.6 MB for a
    // 674x1203 output) was the allocation that failed on PSRAM.  A display
    // list is only requested from the 64 KB PSRAM worker; constructing it on
    // the 8 KB loopTask stack overflows.
    const int th = std::max(1, tileHeight);
    // BMP rows are bottom-up: render/write the lowest tile first.
    for (int y1 = bbox.y1; y1 > bbox.y0; y1 -= th) {
      const int y0 = std::max(bbox.y0, y1 - th);
      const fz_irect tileBbox{bbox.x0, y0, bbox.x1, y1};
      const size_t tileBytes = static_cast<size_t>(width) * static_cast<size_t>(y1 - y0) + 512;
      if (gPdfTileBuffer && gPdfTileBufferSize >= tileBytes) {
        pix = fz_new_pixmap_with_bbox_and_data(ctx, fz_device_gray(ctx), tileBbox, nullptr, 0, gPdfTileBuffer);
      } else {
        pix = fz_new_pixmap_with_bbox(ctx, fz_device_gray(ctx), tileBbox, nullptr, 0);
      }
      fz_clear_pixmap_with_value(ctx, pix, 255);

      fz_device* dev = nullptr;
      fz_var(dev);
      fz_try(ctx) {
        dev = fz_new_draw_device_with_bbox(ctx, ctm, pix, &tileBbox);
        auto savedFillImage = dev->fill_image;
        auto savedFillImageMask = dev->fill_image_mask;
        if (skipImages) {
          dev->fill_image = &skipFillImage;
          dev->fill_image_mask = &skipFillImageMask;
        }
        if (list) {
          fz_run_display_list(ctx, list, dev, fz_identity, fz_infinite_rect, nullptr);
        } else {
          fz_run_page(ctx, page, dev, fz_identity, nullptr);
        }
        if (skipImages) {
          dev->fill_image = savedFillImage;
          dev->fill_image_mask = savedFillImageMask;
        }
        fz_close_device(ctx, dev);
      }
      fz_always(ctx) { fz_drop_device(ctx, dev); }
      fz_catch(ctx) { fz_rethrow(ctx); }

      if (!writeBmpRows(out, ctx, pix, width)) fz_throw(ctx, FZ_ERROR_GENERIC, "BMP row write failed");
      fz_drop_pixmap(ctx, pix);
      pix = nullptr;
      if (ladderExpired()) {
        LOG_ERR("PDF", "tiles: ladder budget reached after %d tiles", tilesDone);
        allBands = false;
        break;
      }
      ++tilesDone;
      fz_empty_store(ctx);
      // The glyph cache holds rendered glyphs for every size on the page.  A
      // full page of CJK text grows it into the megabytes, which is what a
      // tight page then fails to grow a buffer past.  Purging between bands
      // bounds it without changing the output.
      fz_purge_glyph_cache(ctx);
      LOG_ERR("PDF", "tile %d done y=[%d,%d) free=%u largest=%u", tilesDone, y0, y1,
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
              static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    }
    ok = allBands;
  }
  fz_catch(ctx) {
    // The previous version only set ok=false, so the reader showed "page load"
    // error" while MuPDF's actual reason (allocation failure, broken image,
    // unsupported filter) never reached the serial log.  Always report it.
    // Four conversions, four arguments.  The previous format asked for a tile
    // total that this function never computed ("%d/%d"), so every field after
    // it was shifted: the MuPDF error code printed as "tile 1/2", PSRAM free as
    // "code=", the largest block as "free=" and an uninitialized vararg register
    // as "largest=0".  It cost a crash hunt its real numbers -- see
    // docs/engineering/pdf-xip-psram-init-incident-20261119.md section 8.
    LOG_ERR("PDF", "tile render failed at tile %d code=%d free=%u largest=%u", tilesDone + 1, fz_caught(ctx),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    LOG_ERR("PDF", "tile render reason: %s", fz_caught_message(ctx) ? fz_caught_message(ctx) : "(null)");
    delay(20);
    ok = false;
  }
  LOG_ERR("PDF", "tiles end ok=%d tiles=%d free=%u largest=%u", ok ? 1 : 0, tilesDone,
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
  fz_drop_pixmap(ctx, pix);
  if (list) fz_drop_display_list(ctx, list);
  out.close();
  if (!ok) Storage.remove(bmpPath.c_str());
  return ok;
}

// Render the whole visible page into one pixmap and write it out.
//
// Why this exists next to renderTiles(): a tiled run asks the draw device for
// each tile's subarea, and MuPDF caches one decoded image tile per distinct
// subarea.  On a figure-heavy page that multiplies the decoded image set by
// the tile count (measured on page 68: 2.0 MB consumed by the first 128-row
// tile alone, then the next tile ran out of PSRAM).  One pass with one output
// pixmap decodes each image once; it costs width*height bytes of output
// (674x873 = 588 KB) instead of an unbounded number of image tiles.
// Defined further down (needs the pixmap helpers); used by the render paths.
namespace {
void stretchContrast(fz_context* ctx, fz_pixmap* pix, int* outLow, int* outHigh, float* outInk = nullptr);
}

static bool renderWholePage(fz_context* ctx, fz_page* page, fz_matrix ctm, fz_irect bbox,
                            const std::string& bmpPath, bool skipImages, float* outInk = nullptr) {
  HalFile out;
  if (!Storage.openFileForWrite("PDF", bmpPath.c_str(), out)) return false;

  const int width = bbox.x1 - bbox.x0;
  const int height = bbox.y1 - bbox.y0;
  if (width <= 0 || height <= 0) {
    out.close();
    return false;
  }

  fz_pixmap* pix = nullptr;
  fz_device* dev = nullptr;
  bool ok = false;
  float ink = 0.0f;  // ink fraction of what was drawn (see pdf_ink)
  fz_var(pix);
  fz_var(dev);
  fz_var(ok);
  // Step lines, deliberately surrounding every MuPDF call rather than only the
  // ones that can fail.  The reader's 180 s worker deadline says *that* a
  // render never returned; these say which call it is sitting in, which is the
  // difference between a diagnosis and a guess.  A wedge is now recorded as
  // "run_page start" with no "run_page done" -- that is MuPDF page execution,
  // not the pixmap, not the SD write.
  fz_try(ctx) {
    if (!writeBmpHeader(out, width, height)) fz_throw(ctx, FZ_ERROR_GENERIC, "BMP header write failed");
    LOG_ERR("PDF", "render step: bmp header written %dx%d", width, height);
    pix = fz_new_pixmap_with_bbox(ctx, fz_device_gray(ctx), bbox, nullptr, 0);
    LOG_ERR("PDF", "render step: pixmap alloc pix=%d free=%u largest=%u", pix ? 1 : 0,
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    if (!pix) fz_throw(ctx, FZ_ERROR_GENERIC, "whole-page pixmap allocation failed");
    fz_clear_pixmap_with_value(ctx, pix, 255);
    dev = fz_new_draw_device(ctx, ctm, pix);
    LOG_ERR("PDF", "render step: draw device created, run_page start");
    auto savedFillImage = dev->fill_image;
    auto savedFillImageMask = dev->fill_image_mask;
    if (skipImages) {
      dev->fill_image = &skipFillImage;
      dev->fill_image_mask = &skipFillImageMask;
    }
    fz_run_page(ctx, page, dev, fz_identity, nullptr);
    LOG_ERR("PDF", "run_page done free=%u largest=%u", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    if (skipImages) {
      dev->fill_image = savedFillImage;
      dev->fill_image_mask = savedFillImageMask;
    }
    fz_close_device(ctx, dev);
    int lo = 0, hi = 255;
    stretchContrast(ctx, pix, &lo, &hi, outInk ? &ink : nullptr);
    LOG_ERR("PDF", "auto-levels %d..%d", lo, hi);
    LOG_ERR("PDF", "close_device done free=%u", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    ok = writeBmpRows(out, ctx, pix, width);
    LOG_ERR("PDF", "bmp rows written ok=%d free=%u", ok ? 1 : 0,
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  }
  fz_always(ctx) { fz_drop_device(ctx, dev); }
  fz_catch(ctx) {
    LOG_ERR("PDF", "full-page render failed code=%d free=%u largest=%u", fz_caught(ctx),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    LOG_ERR("PDF", "full-page reason: %s", fz_caught_message(ctx) ? fz_caught_message(ctx) : "(null)");
    delay(20);
    ok = false;
  }
  fz_drop_pixmap(ctx, pix);
  out.close();
  if (!ok) Storage.remove(bmpPath.c_str());
  if (outInk) *outInk = ok ? ink : 0.0f;
  return ok;
}

// fz_load_page that turns a MuPDF exception into a null pointer, so callers
// outside an fz_try can reload a page without nesting setjmp frames.
static fz_page* loadPageSafe(fz_context* ctx, fz_document* doc, int pageNo) {
  fz_page* page = nullptr;
  fz_var(page);
  fz_try(ctx) { page = fz_load_page(ctx, doc, pageNo); }
  fz_catch(ctx) { page = nullptr; }
  return page;
}

namespace {

// UTF-8 append/scan helpers for stext characters.
void appendUtf8(std::string& out, int cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

bool isWideByte(unsigned char c) { return c >= 0xE0; }  // UTF-8 lead of a 3/4-byte char

constexpr size_t kMaxReflowChars = 12000;

}  // namespace

namespace {

// Bounding box of the page's actual content, in page space.  A scanned or
// exported image page usually carries white margins; filling the screen with
// the margins intact is what makes an image page look small and off-centre.
// A 96-px preview is enough to find the box and costs one cheap pass.
fz_rect contentBoundsOf(fz_context* ctx, fz_page* page, const fz_rect& bounds) {
  const float w = bounds.x1 - bounds.x0;
  const float h = bounds.y1 - bounds.y0;
  if (w <= 0 || h <= 0) return bounds;
  const float s = std::min(96.0f / w, 96.0f / h);
  if (s <= 0) return bounds;

  const fz_matrix ctm = fz_concat(fz_scale(s, s), fz_translate(-bounds.x0, -bounds.y0));
  const fz_irect ibox = fz_round_rect(fz_transform_rect(bounds, ctm));
  fz_pixmap* pix = nullptr;
  fz_device* dev = nullptr;
  fz_rect out = bounds;
  fz_var(pix);
  fz_var(dev);
  fz_try(ctx) {
    pix = fz_new_pixmap_with_bbox(ctx, fz_device_gray(ctx), ibox, nullptr, 0);
    fz_clear_pixmap_with_value(ctx, pix, 255);
    dev = fz_new_draw_device(ctx, ctm, pix);
    fz_run_page(ctx, page, dev, fz_identity, nullptr);
    fz_close_device(ctx, dev);

    const int pw = ibox.x1 - ibox.x0;
    const int ph = ibox.y1 - ibox.y0;
    const int stride = fz_pixmap_stride(ctx, pix);
    const unsigned char* samples = fz_pixmap_samples(ctx, pix);
    int minx = pw, miny = ph, maxx = -1, maxy = -1;
    for (int y = 0; y < ph; ++y) {
      const unsigned char* row = samples + static_cast<size_t>(y) * stride;
      for (int x = 0; x < pw; ++x) {
        if (row[x] < 245) {
          if (x < minx) minx = x;
          if (x > maxx) maxx = x;
          if (y < miny) miny = y;
          if (y > maxy) maxy = y;
        }
      }
    }
    if (maxx > minx && maxy > miny && (maxx - minx) > pw / 8 && (maxy - miny) > ph / 8) {
      const float mx = (bounds.x1 - bounds.x0) * 0.01f;
      const float my = (bounds.y1 - bounds.y0) * 0.01f;
      out = fz_rect{bounds.x0 + minx / s - mx, bounds.y0 + miny / s - my, bounds.x0 + (maxx + 1) / s + mx,
                    bounds.y0 + (maxy + 1) / s + my};
      out = fz_intersect_rect(out, bounds);
    }
  }
  fz_always(ctx) { fz_drop_device(ctx, dev); }
  fz_catch(ctx) { out = bounds; }
  fz_drop_pixmap(ctx, pix);
  return out;
}

}  // namespace

namespace {

// Auto-levels for a rendered page.  A scan's ink lands in the middle of the
// 0..255 range, which on a 16-level panel reads as grey instead of black; the
// same page re-mapped so its darkest ink is 0 and its paper is 255 reads crisp.
// Percentile clipping keeps a few stray pixels from flattening the whole page.
void stretchContrast(fz_context* ctx, fz_pixmap* pix, int* outLow, int* outHigh, float* outInk) {
  const int w = fz_pixmap_width(ctx, pix);
  const int h = fz_pixmap_height(ctx, pix);
  const int stride = fz_pixmap_stride(ctx, pix);
  unsigned char* samples = fz_pixmap_samples(ctx, pix);
  if (w <= 0 || h <= 0 || !samples) return;

  size_t hist[256] = {};
  const size_t total = static_cast<size_t>(w) * static_cast<size_t>(h);
  for (int y = 0; y < h; ++y) {
    const unsigned char* row = samples + static_cast<size_t>(y) * stride;
    for (int x = 0; x < w; ++x) ++hist[row[x]];
  }

  const size_t clip = total / 200;  // 0.5 % at each end
  size_t acc = 0;
  int low = 0;
  for (; low < 255; ++low) {
    acc += hist[low];
    if (acc > clip) break;
  }
  acc = 0;
  int high = 255;
  for (; high > 0; --high) {
    acc += hist[high];
    if (acc > clip) break;
  }
  if (outLow) *outLow = low;
  if (outHigh) *outHigh = high;
  if (outInk) *outInk = pdf_ink::inkFromHistogram(hist, static_cast<unsigned>(low),
                                                   static_cast<unsigned>(high), total);
  if (high - low < 24) return;  // already contrasty (or blank)

  unsigned char lut[256];
  for (int i = 0; i < 256; ++i) {
    const int v = (i - low) * 255 / (high - low);
    lut[i] = static_cast<unsigned char>(v < 0 ? 0 : (v > 255 ? 255 : v));
  }
  for (int y = 0; y < h; ++y) {
    unsigned char* row = samples + static_cast<size_t>(y) * stride;
    for (int x = 0; x < w; ++x) row[x] = lut[row[x]];
  }
}

}  // namespace

bool PdfDocument::pageHasText(int pageNo) {
  if (!ctx_ || !doc_ || pageNo < 0 || pageNo >= pageCount_ || pageCount_ > 32) return false;
  fz_page* page = nullptr;
  fz_stext_page* text = nullptr;
  bool has = false;
  fz_var(page);
  fz_var(text);
  fz_var(has);
  fz_try(ctx_) {
    page = fz_load_page(ctx_, doc_, pageNo);
    text = fz_new_stext_page_from_page(ctx_, page, nullptr);
    for (fz_stext_block* block = text ? text->first_block : nullptr; block && !has; block = block->next) {
      if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
      for (fz_stext_line* line = block->u.t.first_line; line && !has; line = line->next) {
        if (line->first_char) has = true;
      }
    }
  }
  fz_catch(ctx_) { has = false; }
  fz_drop_stext_page(ctx_, text);
  fz_drop_page(ctx_, page);
  fz_empty_store(ctx_);
  fz_shrink_store(ctx_, 0);
  return has;
}


bool PdfDocument::extractText(int pageNo, std::vector<std::string>& paragraphs, bool& hasImages) {
  paragraphs.clear();
  hasImages = false;
  if (!ctx_ || !doc_ || pageNo < 0 || pageNo >= pageCount_) return false;

  fz_page* page = nullptr;
  fz_stext_page* text = nullptr;
  bool ok = false;
  fz_var(page);
  fz_var(text);
  fz_var(ok);
  // How many text blocks produced a paragraph, so the summary line states the extraction
  // shape without the option's opinion folded into it.  With the option OFF this must read
  // "N blocks -> N paragraphs" on every page: the device-side proof that the setting reached
  // the engine.  That claim used to live only in a comment while the bench hook wrote
  // SETTINGS and stopped, so the engine kept splitting behind a switched-off option.
  // Function scope because the summary prints after the try/catch.
  size_t blocksEmitted = 0;
  fz_try(ctx_) {
    page = fz_load_page(ctx_, doc_, pageNo);
    text = fz_new_stext_page_from_page(ctx_, page, nullptr);
    ok = text != nullptr;
  }
  fz_catch(ctx_) { ok = false; }

  if (ok && text) {
    size_t total = 0;
    // Reused across the blocks of this page so the walk allocates once, not per block.
    std::vector<pdf_join::LineRecord> records;
    for (fz_stext_block* block = text->first_block; block && total < kMaxReflowChars; block = block->next) {
      if (block->type == FZ_STEXT_BLOCK_IMAGE) {
        // A single figure or table means the bitmap path wins: reflowing would
        // drop it entirely.
        hasImages = true;
        continue;
      }
      if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
      // MuPDF groups evenly-spaced hard-wrapped lines into ONE block -- measured on a
      // Chinese page built for this: four separately inserted 16-character lines came
      // back as one block with four lines.  Gluing such a block without looking at the
      // line boxes therefore merges two adjacent paragraphs into a single run, and once
      // the break is gone no app-side rule (`pdf_merge`) can restore it.  So ask the
      // geometry where the paragraphs of this block begin, and honour that.
      //
      // One record per line, in MuPDF's order, because `paragraphStarts()` indexes into
      // the same sequence twice below.  `records` is declared outside the block loop so a
      // page holds one buffer that grows once, rather than allocating per block.
      records.clear();
      const size_t paragraphsBeforeBlock = paragraphs.size();
      for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
        pdf_join::LineRecord record;
        record.box = pdf_join::LineBox{line->bbox.x0, line->bbox.y0, line->bbox.x1, line->bbox.y1};
        // A line whose only characters are blanks is invisible on the page but has a
        // small box: as text it is nothing, as geometry it reads as a colossal shortfall.
        // Both would be wrong, so it is reported as a break and contributes no characters.
        for (fz_stext_char* ch = line->first_char; ch && !record.printable; ch = ch->next) {
          record.printable = !pdf_join::isBlankCp(ch->c);
        }
        records.push_back(record);
      }
      const std::vector<int> starts = pdf_join::paragraphStarts(records, splitParagraphs_);
      size_t startAt = 0;
      std::string para;
      size_t index = 0;
      for (fz_stext_line* line = block->u.t.first_line; line; line = line->next, ++index) {
        if (!records[index].printable) continue;
        std::string lineText;
        for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) appendUtf8(lineText, ch->c);
        while (startAt < starts.size() && starts[startAt] < static_cast<int>(index)) ++startAt;
        if (!para.empty() && startAt < starts.size() && starts[startAt] == static_cast<int>(index)) {
          // The geometry says this line begins a paragraph: close the current one, which
          // is what the app then lays out as a new <p>.
          paragraphs.push_back(std::move(para));
          para.clear();
        }
        if (!para.empty()) {
          if (para.back() == '-') {
            para.pop_back();  // hyphenated line break
          } else if (!isWideByte(static_cast<unsigned char>(para.back())) &&
                     !isWideByte(static_cast<unsigned char>(lineText.front()))) {
            para += ' ';
          }
        }
        para += lineText;
        total += lineText.size();
        if (total >= kMaxReflowChars) break;
      }
      if (!para.empty()) paragraphs.push_back(std::move(para));
      if (paragraphs.size() > paragraphsBeforeBlock) ++blocksEmitted;
      if (paragraphs.size() >= 400) break;
    }
  }

  // One line per extraction, printed by the engine itself: the paragraphing it produced and
  // whether the reader's option was in force.  The app-side `reflow merge:` line only appears
  // when the option is on, so without this there was no observable off-state.
  LOG_INF("PDF", "reflow engine: %u blocks -> %u paragraphs split=%s",
          static_cast<unsigned>(blocksEmitted), static_cast<unsigned>(paragraphs.size()),
          splitParagraphs_ ? "on" : "off");
  fz_drop_stext_page(ctx_, text);
  fz_drop_page(ctx_, page);
  fz_purge_glyph_cache(ctx_);
  fz_empty_store(ctx_);
  fz_shrink_store(ctx_, 0);
  return ok && !paragraphs.empty();
}

void PdfDocument::setParagraphSplit(bool enabled) { splitParagraphs_ = enabled; }

bool PdfDocument::writeBlankBmp(const std::string& bmpPath, int maxWidth, int maxHeight) {
  if (maxWidth <= 0 || maxHeight <= 0) return false;
  HalFile out;
  if (!Storage.openFileForWrite("PDF", bmpPath.c_str(), out)) return false;
  bool ok = writeBmpHeader(out, maxWidth, maxHeight);
  const uint32_t rowBytes = (static_cast<uint32_t>(maxWidth) + 3u) & ~3u;
  std::vector<unsigned char> row(rowBytes, 255);
  for (int y = 0; ok && y < maxHeight; ++y) {
    ok = out.write(row.data(), row.size()) == row.size();
  }
  out.close();
  if (!ok) Storage.remove(bmpPath.c_str());
  return ok;
}

bool PdfDocument::renderPageToBmp(int pageNo, const std::string& bmpPath, int maxWidth, int maxHeight, bool cover) {
  // One budget for the whole page build, ladder included: the app stops waiting
  // at 75 s, and this is what keeps the worker from grinding on after that.
  struct LadderScope {
    LadderScope() { gLadderDeadlineMs = millis() + kLadderBudgetMs; }
    ~LadderScope() { gLadderDeadlineMs = 0; }
  } ladderScope;
  if (!ctx_ || !doc_ || pageNo < 0 || pageNo >= pageCount_ || maxWidth <= 0 || maxHeight <= 0) return false;

  // Large PDFs hold decoded image tiles for the page just rendered: after a
  // successful full-page pass fz_drop_page()/fz_empty_store() only give back
  // part of it (measured 3.9 MB still held, 5.0 MB -> 1.1 MB free).  Reopening
  // the document is the only thing that returns all of it, and it costs about
  // 0.6 s, so do it *before* rendering instead of after a failure: every large
  // page then starts from the same ~5.8 MB instead of inheriting the previous
  // page's leftovers and failing the first attempt.
  // Margins come from the PSRAM this device has, not from this device's numbers
  // (PdfRenderBudget.h; the 8 MB values are reproduced exactly, so nothing
  // changes here -- a smaller board stops being handed impossible reserves).
  const pdf_render_budget::Budget budget =
      pdf_render_budget::forPsram(static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)));
  {
    // Not `pageCount_ > 32`: a two-page document can hold a page that needs
    // more memory than a hundred text pages.  The condition is the memory
    // itself, which is what this branch is about.
    const size_t reserve = static_cast<size_t>(maxWidth) * static_cast<size_t>(maxHeight) + budget.prepReserve;
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < reserve) {
      LOG_ERR("PDF", "render %d: prep reset (free=%u reserve=%u)", pageNo + 1,
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
              static_cast<unsigned>(reserve));
      close();
      if (!load()) return false;
    }
  }

  LOG_ERR("PDF", "render %d/%d max=%dx%d cover=%d psram free=%u largest=%u", pageNo + 1, pageCount_, maxWidth,
          maxHeight, cover ? 1 : 0, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
  fz_page* page = nullptr;
  fz_pixmap* pix = nullptr;
  bool ok = false;
  bool rendered = false;
  bool haveLayout = false;
  fz_matrix ctm = fz_identity;
  fz_irect bbox{0, 0, 1, 1};
  fz_rect pageBounds{};
  float pageScale = 1.0f;
  fz_var(page);
  fz_var(pix);
  fz_var(ok);
  fz_var(rendered);
  fz_var(haveLayout);

  const auto layout = [&](const fz_rect& bounds, const bool useCover, const float factor, fz_matrix& outCtm,
                          fz_irect& outBbox, float& outScale) {
    const float pageW = std::max(1.0f, bounds.x1 - bounds.x0);
    const float pageH = std::max(1.0f, bounds.y1 - bounds.y0);
    const float fitScale =
        std::min(static_cast<float>(maxWidth) / pageW, static_cast<float>(maxHeight) / pageH) * factor;
    const float coverScale =
        std::max(static_cast<float>(maxWidth) / pageW, static_cast<float>(maxHeight) / pageH) * factor;
    outScale = useCover ? coverScale : fitScale;

    // Work in device space: scale first, then translate the scaled page so its
    // centre lands on the output centre.  The old code translated before
    // scaling (fz_concat(translate, scale)) AND ignored bounds.x0/y0, which is
    // why a cover page came out clipped on the left with white on the other
    // three sides.
    const fz_matrix scaleMatrix = fz_scale(outScale, outScale);
    const fz_rect scaled = fz_transform_rect(bounds, scaleMatrix);
    const int outW = useCover ? maxWidth : std::max(1, static_cast<int>(std::ceil(scaled.x1 - scaled.x0)));
    const int outH = useCover ? maxHeight : std::max(1, static_cast<int>(std::ceil(scaled.y1 - scaled.y0)));
    const float dx = scaled.x0 + (scaled.x1 - scaled.x0 - outW) / 2.0f;
    const float dy = scaled.y0 + (scaled.y1 - scaled.y0 - outH) / 2.0f;
    outCtm = fz_concat(scaleMatrix, fz_translate(-dx, -dy));
    outBbox = fz_irect{0, 0, outW, outH};
  };

  fz_try(ctx_) {
    page = fz_load_page(ctx_, doc_, pageNo);
    const fz_rect bounds = fz_bound_page(ctx_, page);
    const float width = bounds.x1 - bounds.x0;
    const float height = bounds.y1 - bounds.y0;
    if (width <= 0 || height <= 0) fz_throw(ctx_, FZ_ERROR_FORMAT, "empty PDF page");

    // Image-only pages: trim the scan's white margins, then fit the remaining
    // content to the screen.  Fitting (rather than cropping to fill) keeps every
    // glyph of a scanned form on screen; with the margins gone the white border
    // is already down to a few pixels.
    const fz_rect target = cover ? contentBoundsOf(ctx_, page, bounds) : bounds;
    layout(target, false, 1.0f, ctm, bbox, pageScale);
    pageBounds = target;
    haveLayout = true;
    fz_set_image_budget(pageCount_ <= 32 ? 2LL * 1024 * 1024 : 96LL * 1024);
    LOG_ERR("PDF", "page bounds %.1fx%.1f scale=%.4f image-budget=%lld", width, height, pageScale,
            static_cast<long long>(pageCount_ <= 32 ? 2LL * 1024 * 1024 : 96LL * 1024));

    // Prefer one full-page pass when PSRAM can hold the output pixmap plus a
    // working margin (see renderWholePage).  Tiles remain the low-memory path.
    const size_t outBytes = static_cast<size_t>(bbox.x1 - bbox.x0) * static_cast<size_t>(bbox.y1 - bbox.y0);
    const size_t freeNow = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    // The full-page pixmap is ONE allocation.  Total free space says nothing
    // about whether it can be satisfied; the largest contiguous block does.
    // Choosing this rung on free-space alone walked into a MuPDF call that
    // never returned (measured: free 1.42 MB but largest 573 KB for a 1.09 MB
    // pixmap, and the render job was still running 180 s later with nothing in
    // the log).  Tiles are the path that works in that shape, so that is what
    // the comparison must select.
    const size_t largestNow = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    if (freeNow > outBytes + budget.fullPageSlack && largestNow > outBytes + budget.pixmapSlack) {
      LOG_ERR("PDF", "render %d: full-page attempt out=%u free=%u largest=%u", pageNo + 1,
              static_cast<unsigned>(outBytes), static_cast<unsigned>(freeNow),
              static_cast<unsigned>(largestNow));
      ok = renderWholePage(ctx_, page, ctm, bbox, bmpPath, false);
    } else {
      LOG_ERR("PDF", "render %d: tiled attempt out=%u free=%u largest=%u", pageNo + 1,
              static_cast<unsigned>(outBytes), static_cast<unsigned>(freeNow),
              static_cast<unsigned>(largestNow));
      ensureTileBuffer(maxWidth);
      ok = renderTiles(ctx_, page, ctm, bbox, kPdfTileHeight, bmpPath, false, useDisplayList_);
    }
    rendered = ok;
  }
  fz_catch(ctx_) {
    LOG_ERR("PDF", "render %d failed: %s", pageNo + 1, fz_caught_message(ctx_));
    ok = false;
  }
  // Same reasoning as the prep reset: this fallback exists because PSRAM is
  // fragmented by what the previous page left behind, so the trigger is the
  // memory, not the number of pages in the file.
  if (!rendered && haveLayout && page &&
      heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < budget.stripGate) {
    // Large PDFs can accumulate 3 MB+ page resources plus image caches. Drop
    // the old page/context and reopen the document so the strip retry starts
    // from a defragmented PSRAM heap instead of the failed full-page state.
    fz_empty_store(ctx_);
    LOG_ERR("PDF", "render %d: resetting MuPDF context before strip fallback", pageNo + 1);
    fz_drop_page(ctx_, page);
    page = nullptr;
    close();
    ok = false;
    if (load()) {
      fz_try(ctx_) {
        page = fz_load_page(ctx_, doc_, pageNo);
        const fz_rect bounds = fz_bound_page(ctx_, page);
        const float width = bounds.x1 - bounds.x0;
        const float height = bounds.y1 - bounds.y0;
        if (width <= 0 || height <= 0) fz_throw(ctx_, FZ_ERROR_FORMAT, "empty PDF page");
        layout(bounds, cover, 1.0f, ctm, bbox, pageScale);
        pageBounds = bounds;
        haveLayout = true;
        ok = true;
        LOG_ERR("PDF", "render %d: context reset page bounds %.1fx%.1f scale=%.4f", pageNo + 1, width, height,
                pageScale);
      }
      fz_catch(ctx_) {
        LOG_ERR("PDF", "render %d: context reset failed: %s", pageNo + 1, fz_caught_message(ctx_));
        ok = false;
      }
    }
    if (!ok) {
      fz_drop_page(ctx_, page);
      fz_empty_store(ctx_);
      fz_shrink_store(ctx_, 0);
      Storage.remove(bmpPath.c_str());
      return false;
    }
  }
  if (!rendered && haveLayout && page) {
    // Every attempt starts from a rebuilt context, not just a rebuilt page.
    // Measured on page 6: after a failed render, fz_drop_page() +
    // fz_empty_store() + fz_shrink_store(0) left the heap at 743 KB, and the
    // next attempt began at 233 KB and failed before drawing anything.  Only
    // dropping the fz_context returns the decoded image tiles, and it costs
    // about 0.6 s.
    const auto resetContext = [&]() -> bool {
      if (page) fz_drop_page(ctx_, page);
      page = nullptr;
      fz_empty_store(ctx_);
      fz_shrink_store(ctx_, 0);
      close();
      if (!load()) return false;
      page = loadPageSafe(ctx_, doc_, pageNo);
      return page != nullptr;
    };
    const auto attempt = [&](const char* label, bool whole, bool skipImages, float factor, bool lowQuality = false,
                             int bandHeight = kPdfTileHeight) -> bool {
      if (ladderExpired()) {
        LOG_ERR("PDF", "render %d: ladder budget reached before %s at %.0f%%", pageNo + 1, label,
                factor * 100.0f);
        return false;
      }
      if (!resetContext()) return false;
      if (lowQuality) {
        // Antialiasing allocates scratch pixmaps per glyph and per path.  Off,
        // the page still lands in the panel's 16 grey levels, and the memory it
        // frees is often what a failing page needs to finish at all.
        fz_set_graphics_aa_level(ctx_, 0);
        fz_set_text_aa_level(ctx_, 0);
      }
      fz_matrix aCtm = fz_identity;
      fz_irect aBbox{0, 0, 1, 1};
      float aScale = 1.0f;
      layout(pageBounds, cover, factor, aCtm, aBbox, aScale);
      LOG_ERR("PDF", "render %d: %s at %.0f%% (images %s) free=%u", pageNo + 1, label, factor * 100.0f,
              skipImages ? "skipped" : "on", static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
      bool r;
      if (whole) {
        float ink = 0.0f;
        r = renderWholePage(ctx_, page, aCtm, aBbox, bmpPath, skipImages, skipImages ? &ink : nullptr);
        if (r && skipImages && pdf_ink::blank(ink)) {
          // Skipping the images emptied the page: on a scan that IS the page.
          // A white sheet would be the silent failure the card exists to
          // replace, so this pass did not succeed -- continue the ladder.
          LOG_ERR("PDF", "render %d: %s at %.0f%% rendered blank (ink %.3f%%); images were the content",
                  pageNo + 1, label, factor * 100.0f, ink * 100.0f);
          Storage.remove(bmpPath.c_str());
          r = false;
        }
      } else {
        ensureTileBuffer(maxWidth);
        r = renderTiles(ctx_, page, aCtm, aBbox, bandHeight, bmpPath, skipImages, useDisplayList_);
      }
      LOG_ERR("PDF", "render %d: %s at %.0f%% -> %s free=%u", pageNo + 1, label, factor * 100.0f, r ? "ok" : "fail",
              static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
      return r;
    };

    // Full-page passes only.  Tiled retries re-parse the page once per tile and
    // each pass keeps its own decoded state, so on the hard pages they ended up
    // needing *more* memory than the full-page pass they were meant to rescue
    // (page 6: full-page 80% fits with 194 KB spare, tiled 80% dies at 233 KB).
    // Scaling the whole page down is both cheaper and better looking than
    // re-rendering it in strips.
    ok = attempt("full-page", true, false, 1.0f);
    if (!ok) ok = attempt("full-page", true, false, 0.9f);
    if (!ok) {
      // Figures are the last thing to give up: shrinking their decode budget
      // and the page together usually fits where either alone does not, and a
      // page with soft figures is still a whole page.
      fz_set_image_budget(96LL * 1024);
      ok = attempt("full-page", true, false, 0.8f);
      fz_set_image_budget(pageCount_ <= 32 ? 2LL * 1024 * 1024 : 96LL * 1024);
    }
    if (!ok) {
      fz_set_image_budget(64LL * 1024);
      ok = attempt("full-page", true, false, 0.8f, true);
      fz_set_image_budget(pageCount_ <= 32 ? 2LL * 1024 * 1024 : 96LL * 1024);
    }
    // Only when even that fails: keep the text, drop the figures.
    // Bands: 384 rows first (fastest), then narrower bands when the page still
    // needs less per-pass memory.  The glyph cache purge after each band is
    // what keeps these flat (measured: 4.99 MB free after every band).
    if (!ok) {
      // One more bitmap try, much smaller and with tiny figures: better a coarse
      // page than none for image pages whose text cannot be reflowed.
      fz_set_image_budget(48LL * 1024);
      ok = attempt("full-page", true, false, 0.6f);
      fz_set_image_budget(pageCount_ <= 32 ? 2LL * 1024 * 1024 : 96LL * 1024);
    }
    if (!ok) ok = attempt("tiles", false, false, 1.0f);
    // Narrower bands were measured on the hard pages and never turned a failure
    // into a success, while each pass costs a full page re-parse.  Dropped.
    if (!ok) ok = attempt("text-only", true, true, 1.0f);
    if (!ok) ok = attempt("text-only", true, true, 0.8f);
    if (!ok) ok = attempt("text-only", true, true, 0.6f);
    // Last resort before the reader reports failure: a small, plain rendering
    // is still a readable page for a document that would otherwise be
    // unopenable at that page.
    if (!ok) ok = attempt("text-only", true, true, 0.4f);
  }
  if (!ok) Storage.remove(bmpPath.c_str());
  fz_drop_pixmap(ctx_, pix);
  fz_drop_page(ctx_, page);
  fz_purge_glyph_cache(ctx_);
  fz_empty_store(ctx_);
  fz_shrink_store(ctx_, 0);
  // The per-page verdict used by the serial tests rides the same host log line
  // as every other message; the module has no direct Serial object.
  LOG_ERR("PDF", "render %d end ok=%d free=%u largest=%u", pageNo + 1, ok ? 1 : 0,
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
  return ok;
}

void PdfDocument::close() {
  if (doc_) {
    fz_drop_document(ctx_, doc_);
    doc_ = nullptr;
  }
  if (stream_) {
    fz_drop_stream(ctx_, stream_);
    stream_ = nullptr;
  }
  if (ctx_) {
    fz_drop_context(ctx_);
    ctx_ = nullptr;
  }
  pageCount_ = 0;
}

bool PdfDocument::ensureTileBuffer(int width) {
  const size_t need = static_cast<size_t>(std::max(1, width)) * kPdfTileHeight + 512;
  if (gPdfTileBuffer && gPdfTileBufferSize >= need) return true;
  if (gPdfTileBuffer) {
    heap_caps_free(gPdfTileBuffer);
    gPdfTileBuffer = nullptr;
    gPdfTileBufferSize = 0;
  }
  gPdfTileBuffer = static_cast<uint8_t*>(heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!gPdfTileBuffer) {
    LOG_ERR("PDF", "tile buffer allocation failed: %u bytes, free=%u largest=%u", static_cast<unsigned>(need),
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
    return false;
  }
  gPdfTileBufferSize = need;
  LOG_ERR("PDF", "tile buffer reserved: %u bytes", static_cast<unsigned>(need));
  return true;
}

void PdfDocument::trimMemory() {
  if (!ctx_) return;
  fz_empty_store(ctx_);
  fz_shrink_store(ctx_, 0);
}

#endif

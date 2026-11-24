// Compiled only into build/pdfmod.bin.  This is the C boundary used by the
// app-side PdfModuleRuntime; keep it in the module so the app never sees MuPDF
// headers or object files.
#if defined(CROSSPOINT_MUPDF) && !defined(CROSSPOINT_PDF_XIP)

#include "PdfModuleApi.h"
#include "PdfModuleHostGlue.h"
#include "PdfDocument.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

void copyString(char* out, int cap, const std::string& value) {
  if (!out || cap <= 0) return;
  const size_t n = std::min(value.size(), static_cast<size_t>(cap - 1));
  std::memcpy(out, value.data(), n);
  out[n] = '\0';
}

void* modOpen(const char* path, const char* password) {
  auto* doc = new PdfDocument(path ? std::string(path) : std::string());
  if (password && password[0]) doc->setPassword(password);
  // A password-protected document keeps the handle alive so the caller can
  // read needsPassword()/wrongPassword() and retry with a new password.
  if (!doc->load() && !doc->needsPassword()) {
    delete doc;
    return nullptr;
  }
  return doc;
}

void modClose(void* handle) { delete static_cast<PdfDocument*>(handle); }

int modNeedsPassword(void* handle) {
  return handle && static_cast<PdfDocument*>(handle)->needsPassword() ? 1 : 0;
}

int modWrongPassword(void* handle) {
  return handle && static_cast<PdfDocument*>(handle)->wrongPassword() ? 1 : 0;
}

int modPageCount(void* handle) { return handle ? static_cast<PdfDocument*>(handle)->pageCount() : 0; }

int modPageHasText(void* handle, int page) {
  return handle && static_cast<PdfDocument*>(handle)->pageHasText(page) ? 1 : 0;
}

int modExtractText(void* handle, int page, char* out, int cap, int* hasImages) {
  if (!handle || !out || cap <= 0) return 0;
  out[0] = '\0';
  if (hasImages) *hasImages = 0;
  std::vector<std::string> paragraphs;
  bool images = false;
  if (!static_cast<PdfDocument*>(handle)->extractText(page, paragraphs, images)) return 0;
  if (hasImages) *hasImages = images ? 1 : 0;
  size_t used = 0;
  for (const auto& p : paragraphs) {
    if (used && used + 2 < static_cast<size_t>(cap)) {
      out[used++] = '\n';
      out[used++] = '\n';
    }
    const size_t n = std::min(p.size(), static_cast<size_t>(cap - 1) - used);
    std::memcpy(out + used, p.data(), n);
    used += n;
    if (used + 1 >= static_cast<size_t>(cap)) break;
  }
  out[used] = '\0';
  return static_cast<int>(used);
}int modOutlineCount(void* handle) {
  return handle ? static_cast<int>(static_cast<PdfDocument*>(handle)->outline().size()) : 0;
}

int modOutlineAt(void* handle, int index, char* title, int cap, int* page) {
  if (!handle || index < 0) return 0;
  const auto& entries = static_cast<PdfDocument*>(handle)->outline();
  if (static_cast<size_t>(index) >= entries.size()) return 0;
  copyString(title, cap, entries[static_cast<size_t>(index)].title);
  if (page) *page = entries[static_cast<size_t>(index)].page;
  return 1;
}

int modRenderBmp(void* handle, int page, const char* path, int width, int height, int cover) {
  return handle && path &&
                 static_cast<PdfDocument*>(handle)->renderPageToBmp(page, path, width, height, cover != 0)
             ? 1
             : 0;
}

int modWriteBlank(void* handle, const char* path, int width, int height) {
  return handle && path && static_cast<PdfDocument*>(handle)->writeBlankBmp(path, width, height) ? 1 : 0;
}

void modTrim(void* handle) {
  if (handle) static_cast<PdfDocument*>(handle)->trimMemory();
}

int modEnsureTile(void* handle, int width) {
  return handle && static_cast<PdfDocument*>(handle)->ensureTileBuffer(width) ? 1 : 0;
}

void modSetDisplayList(void* handle, int enabled) {
  if (handle) static_cast<PdfDocument*>(handle)->setUseDisplayList(enabled != 0);
}

void modSetParagraphSplit(void* handle, int enabled) {
  if (handle) static_cast<PdfDocument*>(handle)->setParagraphSplit(enabled != 0);
}

}  // namespace

extern "C" int pdfmod_entry(const PdfModuleHost* host, PdfModuleOps* ops) {
  if (!host || !ops || ops->structSize < sizeof(PdfModuleOps)) return 1;
  if ((host->abi >> 16) != kPdfModuleHostApiMajor || host->structSize < sizeof(PdfModuleHost)) return 2;
  g_pdf_host = host;
  ops->abi = kPdfModuleAbiVersion;
  ops->open = &modOpen;
  ops->close = &modClose;
  ops->needs_password = &modNeedsPassword;
  ops->wrong_password = &modWrongPassword;
  ops->page_count = &modPageCount;
  ops->page_has_text = &modPageHasText;
  ops->extract_text = &modExtractText;
  ops->outline_count = &modOutlineCount;
  ops->outline_at = &modOutlineAt;
  ops->render_bmp = &modRenderBmp;
  ops->write_blank = &modWriteBlank;
  ops->trim = &modTrim;
  ops->ensure_tile = &modEnsureTile;
  ops->set_display_list = &modSetDisplayList;
  ops->set_paragraph_split = &modSetParagraphSplit;
  return 0;
}

#endif

#if defined(CROSSPOINT_MUPDF)

#include "mupdf/fitz.h"

// Link-probe: forces the PDF interpreter and its dependencies into the
// firmware image.  It is never called on device; -Wl,--undefined keeps it.
extern "C" int pdf_probe_entry(void) {
  fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_UNLIMITED);
  if (!ctx) return 1;
  fz_register_document_handlers(ctx);
  fz_document *doc = NULL;
  fz_page *page = NULL;
  fz_pixmap *pix = NULL;
  fz_var(doc);
  fz_var(page);
  fz_var(pix);
  fz_try(ctx) {
    doc = fz_open_document(ctx, "probe.pdf");
    page = fz_load_page(ctx, doc, 0);
    const fz_matrix ctm = fz_scale(1.0f, 1.0f);
    pix = fz_new_pixmap_from_page(ctx, page, ctm, fz_device_gray(ctx), 0);
  }
  fz_catch(ctx) {
  }
  fz_drop_pixmap(ctx, pix);
  fz_drop_page(ctx, page);
  fz_drop_document(ctx, doc);
  fz_drop_context(ctx);
  return 0;
}

#endif

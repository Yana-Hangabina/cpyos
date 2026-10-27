#include "PdfAssetDownload.h"

#if defined(CROSSPOINT_MUPDF)

#include <Logging.h>

bool tryDownloadPdfAssets() {
  LOG_INF("PDF", "asset download not configured (repository URL pending)");
  return false;
}

#endif

#include "booksource/BookSource.h"

// The private component (when linked) exports exactly one symbol. Keeping the
// reference behind this compile-time gate is what lets the public tree build
// with the component absent: the linker never sees an undefined reference.
#if defined(CROSSPOINT_BOOKSOURCE_JJWXC)
namespace booksource {
ISource* privateSource();
}
#endif

namespace booksource {
namespace {

ISource* resolve() {
#if defined(CROSSPOINT_BOOKSOURCE_JJWXC)
  return privateSource();
#else
  return nullptr;
#endif
}

ISource* cached() {
  // Function-local static: the component's own registration is also a
  // function-local static, so this runs after it on first use regardless of
  // translation-unit initialisation order.
  static ISource* const source = resolve();
  return source;
}

}  // namespace

ISource* active() { return cached(); }

const char* badgeForPath(const char* path) {
  ISource* source = active();
  return source != nullptr && path != nullptr ? source->badgeForPath(path) : nullptr;
}

UpdateGap updateGapForPath(const char* path) {
  ISource* source = active();
  return source != nullptr && path != nullptr ? source->updateGapForPath(path) : UpdateGap{};
}

bool cachedChapterSummary(const char* path, uint32_t& total, bool& readable) {
  ISource* source = active();
  if (source == nullptr || path == nullptr) return false;
  return source->cachedChapterSummary(path, total, readable);
}

bool sessionPresent() {
  ISource* source = active();
  return source != nullptr && source->sessionPresent();
}

void invalidateUpdates() {
  ISource* source = active();
  if (source != nullptr) source->invalidateUpdates();
}

void start() {
  ISource* source = active();
  if (source != nullptr) source->start();
}

void poll() {
  ISource* source = active();
  if (source != nullptr) source->poll();
}

void onStandby() {
  ISource* source = active();
  if (source != nullptr) source->onStandby();
}

void setReaderForeground(const bool foreground) {
  ISource* source = active();
  if (source != nullptr) source->setReaderForeground(foreground);
}

JobBadge jobBadge() {
  ISource* source = active();
  return source != nullptr ? source->jobBadge() : JobBadge::None;
}

uint32_t jobRevision() {
  ISource* source = active();
  return source != nullptr ? source->jobRevision() : 0;
}

}  // namespace booksource

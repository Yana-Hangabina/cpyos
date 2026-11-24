#include "PluginHost.h"

#if defined(CROSSPOINT_PLUGIN_RUNTIME)

#include <esp_elf.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "GfxRenderer.h"
#include "HalDisplay.h"
#include "HalStorage.h"
#include "I18n.h"
#include "Logging.h"
#include "activities/RenderLock.h"
#include "fontIds.h"

extern GfxRenderer renderer;

namespace {

#if defined(__XTENSA__)
// The plugin body is in PSRAM; on Read Pico the internal heap is already
// fragmented by Wi-Fi/fonts, so the loader task stack lives in PSRAM too
// (the task never writes internal flash; only SD/renderer are touched).
constexpr uint32_t kPluginTaskStackBytes = 32 * 1024;
#else
constexpr uint32_t kPluginTaskStackBytes = 24576;
#endif
constexpr UBaseType_t kPluginTaskPriority = 5;
constexpr size_t kEventQueueDepth = 16;
constexpr uint32_t kStopTimeoutMs = 4000;
constexpr size_t kPluginAllocBudget = 2u * 1024u * 1024u;
constexpr size_t kAllocHeaderBytes = 16;
struct AllocHeader {
  uint32_t magic;
  uint32_t size;
};
constexpr uint32_t kAllocMagic = 0x504C5547;  // 'PLUG'


portMUX_TYPE s_eventMux = portMUX_INITIALIZER_UNLOCKED;
CpyosPluginEvent s_events[kEventQueueDepth];
size_t s_eventHead = 0;
size_t s_eventCount = 0;

void resetEvents() {
  taskENTER_CRITICAL(&s_eventMux);
  s_eventHead = 0;
  s_eventCount = 0;
  taskEXIT_CRITICAL(&s_eventMux);
}

const char* apiText(const int32_t id) {
  switch (id) {
    case CPYOS_TEXT_TITLE_2048: return I18N.get(StrId::STR_2048_TITLE);
    case CPYOS_TEXT_TITLE_SOKOBAN: return I18N.get(StrId::STR_SOKOBAN_TITLE);
    case CPYOS_TEXT_TITLE_GOMOKU: return I18N.get(StrId::STR_GOMOKU_TITLE);
    case CPYOS_TEXT_TITLE_MINESWEEPER: return I18N.get(StrId::STR_MINESWEEPER_TITLE);
    case CPYOS_TEXT_TITLE_SUDOKU: return I18N.get(StrId::STR_SUDOKU_TITLE);
    case CPYOS_TEXT_TITLE_CHINESE_CHESS: return I18N.get(StrId::STR_CHINESE_CHESS_TITLE);
    case CPYOS_TEXT_SCORE: return I18N.get(StrId::STR_2048_SCORE);
    case CPYOS_TEXT_GAME_OVER: return I18N.get(StrId::STR_2048_GAME_OVER);
    case CPYOS_TEXT_RESTART_HINT: return I18N.get(StrId::STR_2048_HINT_RESTART);
    case CPYOS_TEXT_NEW_GAME: return I18N.get(StrId::STR_GAME_NEW_GAME);
    case CPYOS_TEXT_RESTART: return I18N.get(StrId::STR_MINESWEEPER_RESTART);
    case CPYOS_TEXT_MOVES: return I18N.get(StrId::STR_SOKOBAN_MOVES);
    case CPYOS_TEXT_LEVEL: return I18N.get(StrId::STR_SOKOBAN_LEVEL);
    case CPYOS_TEXT_BLACK: return I18N.get(StrId::STR_GOMOKU_BLACK);
    case CPYOS_TEXT_WHITE: return I18N.get(StrId::STR_GOMOKU_WHITE);
    case CPYOS_TEXT_RED: return I18N.get(StrId::STR_CHINESE_CHESS_RED);
    case CPYOS_TEXT_BLACK_WINS: return I18N.get(StrId::STR_GOMOKU_BLACK_WINS);
    case CPYOS_TEXT_WHITE_WINS: return I18N.get(StrId::STR_GOMOKU_WHITE_WINS);
    case CPYOS_TEXT_RED_WINS: return I18N.get(StrId::STR_CHINESE_CHESS_RED_WINS);
    case CPYOS_TEXT_CLEARED: return I18N.get(StrId::STR_MINESWEEPER_CLEARED);
    case CPYOS_TEXT_BOOM: return I18N.get(StrId::STR_MINESWEEPER_BOOM);
    case CPYOS_TEXT_SOLVED: return I18N.get(StrId::STR_SUDOKU_SOLVED);
    case CPYOS_TEXT_DIG: return I18N.get(StrId::STR_MINESWEEPER_MODE_DIG);
    case CPYOS_TEXT_PLACE: return I18N.get(StrId::STR_GOMOKU_PLACE);
    case CPYOS_TEXT_MOVE: return I18N.get(StrId::STR_CHINESE_CHESS_MOVE);
    case CPYOS_TEXT_SELECT: return I18N.get(StrId::STR_CHINESE_CHESS_SELECT);
    case CPYOS_TEXT_WIN_HINT: return I18N.get(StrId::STR_SOKOBAN_WIN_HINT);
    case CPYOS_TEXT_HINT_MOVE: return I18N.get(StrId::STR_PIXEL_SWITCH_INTRO_MOVE);
    case CPYOS_TEXT_WIN: return I18N.get(StrId::STR_SOKOBAN_WIN);
    default: return "";
  }
}

int fontIdFor(const int32_t font) {
  switch (font) {
    case CPYOS_FONT_SMALL:
      return UI_10_FONT_ID;
    case CPYOS_FONT_MEDIUM:
      return UI_12_FONT_ID;
    case CPYOS_FONT_LARGE:
      return NOTOSANS_18_FONT_ID;
    case CPYOS_FONT_TITLE:
      return NOTOSERIF_TITLE_FONT_ID;
    default:
      return UI_12_FONT_ID;
  }
}

struct PluginFile {
  HalFile file;
  size_t size = 0;
};

size_t pluginFileSize(void* ctx) { return static_cast<PluginFile*>(ctx)->size; }

int pluginFileRead(void* ctx, size_t off, void* buf, size_t len) {
  auto* source = static_cast<PluginFile*>(ctx);
  if (!source->file.isOpen()) return -EIO;
  if (!source->file.seekSet(off)) return -EIO;
  const size_t got = source->file.read(buf, len);
  return got == len ? 0 : -EIO;
}

void pluginFileClose(void* ctx) {
  auto* source = static_cast<PluginFile*>(ctx);
  source->file.close();
}

}  // namespace

PluginHost& PluginHost::instance() {
  static PluginHost host;
  return host;
}

bool PluginHost::start(const native_plugins::PluginInfo& info) {
  if (state_.load(std::memory_order_acquire) == State::Running) return false;
  if (!info.error.empty()) {
    lastError_ = "package is not loadable";
    state_.store(State::Failed, std::memory_order_release);
    return false;
  }

  info_ = info;
  exitRequested_.store(false, std::memory_order_release);
  lastError_ = "";
  allocatedBytes_ = 0;
  resetEvents();

  api_ = CpyosPluginApi{};
  api_.struct_size = sizeof(CpyosPluginApi);
  api_.api_version = CPYOS_PLUGIN_API_VERSION;
  api_.width = renderer.getScreenWidth();
  api_.height = renderer.getScreenHeight();
  api_.log = &apiLog;
  api_.request_exit = &apiRequestExit;
  api_.exit_requested = &apiExitRequested;
  api_.now_ms = &apiNowMs;
  api_.delay_ms = &apiDelay;
  api_.poll_event = &apiPollEvent;
  api_.clear = &apiClear;
  api_.fill_rect = &apiFillRect;
  api_.draw_text = &apiDrawText;
  api_.text_width = &apiTextWidth;
  api_.present = &apiPresent;
  api_.alloc = &apiAlloc;
  api_.release = &apiRelease;
  api_.data_read = &apiDataRead;
  api_.data_write = &apiDataWrite;
  api_.text = &apiText;

  state_.store(State::Running, std::memory_order_release);
  BaseType_t created = pdFAIL;
#if defined(__XTENSA__) && defined(BOARD_HAS_PSRAM)
  created = xTaskCreatePinnedToCoreWithCaps(taskTrampoline, "native-plugin",
                                            kPluginTaskStackBytes / sizeof(StackType_t), this, kPluginTaskPriority,
                                            &task_, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  taskStackInPsram_ = created == pdPASS;
  if (created != pdPASS) {
    LOG_ERR("PLUG", "PSRAM plugin stack refused (%d); trying internal RAM", static_cast<int>(created));
    task_ = nullptr;
  }
#endif
  if (created != pdPASS) {
    taskStackInPsram_ = false;
    created = xTaskCreate(taskTrampoline, "native-plugin", kPluginTaskStackBytes / sizeof(StackType_t), this,
                          kPluginTaskPriority, &task_);
  }
  if (created != pdPASS) {
    task_ = nullptr;
    lastError_ = "cannot create the plugin task";
    state_.store(State::Failed, std::memory_order_release);
    return false;
  }
  return true;
}

void PluginHost::requestExit() { exitRequested_.store(true, std::memory_order_release); }

void PluginHost::stop() {
  requestExit();
  const uint32_t startMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);
  while (state_.load(std::memory_order_acquire) == State::Running) {
    if (static_cast<uint32_t>(esp_timer_get_time() / 1000) - startMs > kStopTimeoutMs) break;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (state_.load(std::memory_order_acquire) == State::Running && task_ != nullptr) {
    // The plugin ignored the cooperative exit. Deleting it is the last resort;
    // it must not hold the render lock while waiting on a panel scan here.
    LOG_ERR("PLUG", "plugin did not stop in %u ms; deleting its task", static_cast<unsigned>(kStopTimeoutMs));
    if (taskStackInPsram_) {
      vTaskDeleteWithCaps(task_);
    } else {
      vTaskDelete(task_);
    }
    taskStackInPsram_ = false;
    state_.store(State::Idle, std::memory_order_release);
  }
  task_ = nullptr;
  resetEvents();
}

void PluginHost::pushEvent(const CpyosPluginEvent& event) {
  taskENTER_CRITICAL(&s_eventMux);
  if (s_eventCount < kEventQueueDepth) {
    s_events[(s_eventHead + s_eventCount) % kEventQueueDepth] = event;
    ++s_eventCount;
  }
  taskEXIT_CRITICAL(&s_eventMux);
}

void PluginHost::taskTrampoline(void* arg) { static_cast<PluginHost*>(arg)->taskMain(); }

void PluginHost::fail(const char* message) {
  lastError_ = message;
  LOG_ERR("PLUG", "%s: %s", info_.id.c_str(), message);
  printf("plugin: %s failed: %s\n", info_.id.c_str(), message);
}

void PluginHost::taskMain() {
  const std::string marker = std::string(native_plugins::kPluginRoot) + "/" + info_.dirName + "/" +
                             native_plugins::kCrashMarkerName;
  Storage.writeFile(marker.c_str(), "running");

  printf("plugin: load %s (%u bytes)\n", info_.id.c_str(), static_cast<unsigned>(info_.elfSize));

  PluginFile file;
  file.file = Storage.open(info_.elfPath.c_str(), O_RDONLY);
  file.size = info_.elfSize;
  if (!file.file.isOpen()) {
    fail("cannot open plugin.elf");
    Storage.remove(marker.c_str());
    state_.store(State::Failed, std::memory_order_release);
    if (taskStackInPsram_) vTaskDeleteWithCaps(nullptr); else vTaskDelete(nullptr);
    return;
  }
  const elf_source_t source = {pluginFileSize, pluginFileRead, pluginFileClose, &file};

  esp_elf_t elf{};
  const int initRc = esp_elf_init(&elf);
  int rc = initRc;
  if (rc == 0) {
    rc = esp_elf_relocate_source(&elf, &source);
  }
  if (rc != 0) {
    fail("ELF relocation failed");
    printf("plugin: relocate rc=%d\n", rc);
    if (initRc == 0) esp_elf_deinit(&elf);
    file.file.close();
    Storage.remove(marker.c_str());
    state_.store(State::Failed, std::memory_order_release);
    if (taskStackInPsram_) vTaskDeleteWithCaps(nullptr); else vTaskDelete(nullptr);
    return;
  }

  char* argv[2] = {reinterpret_cast<char*>(&api_), const_cast<char*>(info_.id.c_str())};
  const int64_t runStart = esp_timer_get_time();
  esp_elf_request(&elf, 0, 2, argv);
  const int64_t runEnd = esp_timer_get_time();

  freeAllocations();
  esp_elf_deinit(&elf);
  file.file.close();
  Storage.remove(marker.c_str());
  printf("plugin: %s finished exec=%lldus\n", info_.id.c_str(), static_cast<long long>(runEnd - runStart));

  state_.store(State::Finished, std::memory_order_release);
  task_ = nullptr;
  if (taskStackInPsram_) vTaskDeleteWithCaps(nullptr); else vTaskDelete(nullptr);
}

void PluginHost::apiLog(const char* message) {
  PluginHost& host = instance();
  printf("plugin[%s]: %s\n", host.info_.id.c_str(), message == nullptr ? "(null)" : message);
}

void PluginHost::apiRequestExit() { instance().requestExit(); }

int32_t PluginHost::apiExitRequested() {
  return instance().exitRequested_.load(std::memory_order_acquire) ? 1 : 0;
}

uint32_t PluginHost::apiNowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void PluginHost::apiDelay(uint32_t ms) {
  if (ms == 0) {
    vTaskDelay(1);
    return;
  }
  vTaskDelay(pdMS_TO_TICKS(ms));
}

int32_t PluginHost::apiPollEvent(CpyosPluginEvent* out) {
  if (out == nullptr) return 0;
  taskENTER_CRITICAL(&s_eventMux);
  const bool have = s_eventCount > 0;
  if (have) {
    *out = s_events[s_eventHead];
    s_eventHead = (s_eventHead + 1) % kEventQueueDepth;
    --s_eventCount;
  }
  taskEXIT_CRITICAL(&s_eventMux);
  return have ? 1 : 0;
}

void PluginHost::apiClear(int32_t black) {
  RenderLock lock;
  renderer.clearScreen(black ? 0x00 : 0xFF);
}

void PluginHost::apiFillRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t black) {
  RenderLock lock;
  renderer.fillRect(x, y, w, h, black != 0);
}

void PluginHost::apiDrawText(int32_t x, int32_t y, int32_t font, const char* text, int32_t black) {
  if (text == nullptr) return;
  RenderLock lock;
  renderer.drawText(fontIdFor(font), x, y, text, black != 0);
}

int32_t PluginHost::apiTextWidth(int32_t font, const char* text) {
  if (text == nullptr) return 0;
  RenderLock lock;
  return renderer.getTextWidth(fontIdFor(font), text);
}

void PluginHost::apiPresent() {
  RenderLock lock;
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void* PluginHost::apiAlloc(uint32_t bytes) {
  PluginHost& host = instance();
  size_t slot = kMaxAllocations;
  for (size_t i = 0; i < kMaxAllocations; ++i) {
    if (host.allocations_[i] == nullptr) {
      slot = i;
      break;
    }
  }
  if (bytes == 0 || slot == kMaxAllocations || bytes > kPluginAllocBudget ||
      host.allocatedBytes_ > kPluginAllocBudget - bytes) {
    printf("plugin[%s]: allocation refused (%u bytes)\n", host.info_.id.c_str(), static_cast<unsigned>(bytes));
    return nullptr;
  }
  auto* raw = static_cast<uint8_t*>(
      heap_caps_malloc(bytes + kAllocHeaderBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (raw == nullptr) return nullptr;
  auto* header = reinterpret_cast<AllocHeader*>(raw);
  header->magic = kAllocMagic;
  header->size = bytes;
  host.allocatedBytes_ += bytes;
  host.allocations_[slot] = raw;
  return raw + kAllocHeaderBytes;
}

namespace {
bool safeDataName(const char* name) {
  if (name == nullptr || *name == '\0') return false;
  size_t length = 0;
  for (const char* p = name; *p != '\0'; ++p) {
    const char c = *p;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
                    c == '_' || c == '.';
    if (!ok || ++length > 48) return false;
  }
  return name[0] != '.';
}

std::string dataPath(const std::string& id, const char* name) {
  return std::string("/.crosspoint/plugin_data/") + id + "/" + name;
}
}  // namespace

void PluginHost::apiRelease(void* pointer) {
  if (pointer == nullptr) return;
  auto* raw = static_cast<uint8_t*>(pointer) - kAllocHeaderBytes;
  auto* header = reinterpret_cast<AllocHeader*>(raw);
  if (header->magic != kAllocMagic) return;
  PluginHost& host = instance();
  host.allocatedBytes_ -= header->size;
  for (size_t i = 0; i < kMaxAllocations; ++i) {
    if (host.allocations_[i] == raw) host.allocations_[i] = nullptr;
  }
  header->magic = 0;
  free(raw);
}

int32_t PluginHost::apiDataRead(const char* name, uint8_t* buffer, int32_t capacity) {
  if (!safeDataName(name) || buffer == nullptr || capacity <= 0) return -1;
  PluginHost& host = instance();
  const std::string path = dataPath(host.info_.id, name);
  HalFile file;
  if (!Storage.openFileForRead("PLUG", path.c_str(), file)) return -1;
  const size_t size = file.fileSize();
  if (size > static_cast<size_t>(capacity)) {
    file.close();
    return -1;
  }
  const int read = file.read(buffer, size);
  file.close();
  return read == static_cast<int>(size) ? static_cast<int32_t>(size) : -1;
}

int32_t PluginHost::apiDataWrite(const char* name, const uint8_t* data, int32_t length) {
  if (!safeDataName(name) || data == nullptr || length <= 0 || length > 64 * 1024) return -1;
  PluginHost& host = instance();
  const std::string dir = std::string("/.crosspoint/plugin_data/") + host.info_.id;
  Storage.ensureDirectoryExists("/.crosspoint");
  Storage.ensureDirectoryExists("/.crosspoint/plugin_data");
  Storage.ensureDirectoryExists(dir.c_str());
  HalFile file;
  if (!Storage.openFileForWrite("PLUG", dataPath(host.info_.id, name), file)) return -1;
  const size_t written = file.write(data, static_cast<size_t>(length));
  file.close();
  return written == static_cast<size_t>(length) ? 0 : -1;
}

void PluginHost::freeAllocations() {
  for (size_t i = 0; i < kMaxAllocations; ++i) {
    if (allocations_[i] != nullptr) {
      free(allocations_[i]);
      allocations_[i] = nullptr;
    }
  }
  allocatedBytes_ = 0;
}

#endif  // CROSSPOINT_PLUGIN_RUNTIME

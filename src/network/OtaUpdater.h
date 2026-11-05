#pragma once

#include <ReleaseJsonParser.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

class OtaUpdater {
 public:
  enum class Channel : uint8_t { Stable, Nightly };
  enum class Phase : uint8_t { Firmware, Packs };
  using ReleaseNote = ReleaseJsonParser::ReleaseNote;
  using ProgressCallback = void (*)(void* ctx);

  enum OtaUpdaterError {
    OK = 0,
    NO_UPDATE,
    UNSUPPORTED_CHANNEL,
    HTTP_ERROR,
    JSON_PARSE_ERROR,
    UPDATE_OLDER_ERROR,
    INTERNAL_UPDATE_ERROR,
    OOM_ERROR,
    WRONG_DEVICE_ERROR,
  };

 private:
  static constexpr size_t kMaxPacks = 8;
  struct PackSpec {
    char asset[32];
    uint32_t offset = 0;
    uint32_t size = 0;
    uint32_t limit = 0;
    uint8_t sha256[32] = {};
    char tempPath[64] = {};
    bool changed = false;
  };

  bool updateAvailable = false;
  Channel channel = Channel::Stable;
  std::string latestVersion;
  std::string otaUrl;
  size_t otaSize = 0;
  size_t processedSize = 0;
  size_t totalSize = 0;
  std::array<ReleaseNote, ReleaseJsonParser::RELEASE_NOTE_COUNT_MAX> releaseNotes{};
  size_t releaseNoteCount = 0;

  // Data-pack update state. Packs live in the spiffs data partition; app-only
  // OTA used to leave them stale and force a full flash.
  PackSpec packs[kMaxPacks]{};
  size_t packCount = 0;
  size_t dataTotalSize = 0;
  size_t dataProcessedSize = 0;
  Phase phase = Phase::Firmware;
  bool packsLoaded = false;
  bool packUpdates = false;

  OtaUpdaterError loadPackManifest();
  OtaUpdaterError installDataPacks(ProgressCallback onProgress, void* ctx, bool required);

 public:
  size_t getOtaSize() const { return otaSize; }

  size_t getProcessedSize() const { return phase == Phase::Packs ? dataProcessedSize : processedSize; }

  size_t getTotalSize() const { return phase == Phase::Packs ? dataTotalSize : totalSize; }

  Phase getPhase() const { return phase; }

  size_t getPackTotalSize() const { return dataTotalSize; }

  OtaUpdater() = default;
  bool isUpdateNewer() const;
  const std::string& getLatestVersion() const;
  std::span<const ReleaseNote> getReleaseNotes() const {
    return std::span<const ReleaseNote>(releaseNotes.data(), releaseNoteCount);
  }
#ifdef SIMULATOR
  void loadSimulatorReleaseNotes() {
    static constexpr const char* MOCK_NOTES[] = {
        "OTA更新页采用连续文档布局并支持完整日志翻页",    "更新日志按条目整页排版并保持单条内容完整显示",
        "默认字体下载新增20号与22号字号并补充伪粗体说明", "优化中文TXT分页扫描与缓存重建提升首次打开速度",
        "提升AirPage图片格式兼容性并保留旧BMP接口回退",   "修复微信读书浏览页退出卡死以及详情菜单行高异常",
        "改善多款游戏宽屏居中显示并减少象棋AI循环走子",   "提升夜间版发布与双固件下载流程的稳定性和可恢复性",
    };
    static_assert(std::size(MOCK_NOTES) <= ReleaseJsonParser::RELEASE_NOTE_COUNT_MAX);
    releaseNoteCount = std::size(MOCK_NOTES);
    for (size_t i = 0; i < releaseNoteCount; ++i) {
      strncpy(releaseNotes[i].data(), MOCK_NOTES[i], releaseNotes[i].size() - 1);
      releaseNotes[i].back() = '\0';
    }
  }
#endif
  OtaUpdaterError checkForUpdate(Channel requestedChannel);
  // Fetches the packs manifest for the release found by checkForUpdate() and
  // compares each blob with what is already in the data partition. Returns true
  // when at least one pack differs, so the caller can offer a packs-only update
  // even when the app version is already current.
  bool checkPackUpdates();
  OtaUpdaterError installUpdate(ProgressCallback onProgress = nullptr, void* ctx = nullptr);
  // Downloads and writes only the data packs. Used after an app-only OTA from a
  // firmware that predates data-pack updates, and as a retry path.
  OtaUpdaterError installDataPacksOnly(ProgressCallback onProgress = nullptr, void* ctx = nullptr);
  // Diagnostics for the mainland-China mirrors: measure every candidate for the
  // last checked release asset and log the winner. Does not flash anything.
  void debugProbeDownloadMirrors();
};

#pragma once

// Bounded plugin job pool for the browser-side plugin runner.
//
// Pure logic (no IDF/Arduino): the web handlers own JSON and HTTP, this table
// owns the lifecycle. A job is submitted -> claimed by a page running the
// plugin -> completed with the claim id it was given. When a claim's lease
// expires the job goes back to pending; a late completion with the old claim is
// then stale and must not overwrite the newer runner's result.

#include <cstdint>
#include <cstring>

namespace plugin_jobs {

enum State : uint8_t {
  kEmpty = 0,
  kPending = 1,
  kRunning = 2,
  kDone = 3,
  kError = 4,
};

constexpr size_t kPluginLen = 24;
constexpr size_t kActionLen = 24;
constexpr size_t kArgsLen = 192;
constexpr size_t kResultLen = 192;

struct Job {
  uint32_t id = 0;         // 0 = empty slot
  uint32_t claim = 0;      // current claim; a completion must echo it
  uint32_t updatedAt = 0;  // ms of the last state change
  uint8_t state = kEmpty;
  char plugin[kPluginLen] = {0};
  char action[kActionLen] = {0};
  char args[kArgsLen] = {0};    // serialized JSON value
  char result[kResultLen] = {0};  // serialized JSON value from the executor
};

enum class Outcome {
  Ok,
  AlreadySettled,
  StaleClaim,
  NotFound,
};

template <size_t N>
class Table {
 public:
  // An empty slot, else the oldest finished one, else nullptr (queue full).
  Job* alloc(uint32_t now) {
    Job* best = nullptr;
    for (auto& job : jobs_) {
      if (job.state == kEmpty) return prepare(job, now);
      const bool finished = job.state == kDone || job.state == kError;
      if (finished && (best == nullptr || job.updatedAt < best->updatedAt)) best = &job;
    }
    return best != nullptr ? prepare(*best, now) : nullptr;
  }

  // Next pending job for `plugin`, after re-queueing an expired lease.
  Job* claim(const char* plugin, uint32_t now, uint32_t leaseMs) {
    if (plugin == nullptr) return nullptr;
    for (auto& job : jobs_) {
      if (job.state == kRunning && static_cast<uint32_t>(now - job.updatedAt) > leaseMs) {
        job.state = kPending;
        job.updatedAt = now;
      }
      if (job.state != kPending || std::strcmp(plugin, job.plugin) != 0) continue;
      job.state = kRunning;
      job.claim = nextClaim_++;
      job.updatedAt = now;
      return &job;
    }
    return nullptr;
  }

  Outcome complete(uint32_t id, uint32_t claim, bool ok, const char* resultJson, uint32_t now) {
    for (auto& job : jobs_) {
      if (job.id != id || job.state == kEmpty) continue;
      if (job.state == kDone || job.state == kError) return Outcome::AlreadySettled;
      if (job.state != kRunning) return Outcome::NotFound;
      if (job.claim != claim) return Outcome::StaleClaim;
      job.state = ok ? kDone : kError;
      job.updatedAt = now;
      job.result[0] = '\0';
      if (resultJson != nullptr) {
        if (std::strlen(resultJson) >= sizeof(job.result)) {
          std::strncpy(job.result, "{\"error\":\"result too large\"}", sizeof(job.result) - 1);
        } else {
          std::strncpy(job.result, resultJson, sizeof(job.result) - 1);
        }
        job.result[sizeof(job.result) - 1] = '\0';
      }
      return Outcome::Ok;
    }
    return Outcome::NotFound;
  }

  const Job* find(uint32_t id) const {
    for (const auto& job : jobs_) {
      if (job.id == id && job.state != kEmpty) return &job;
    }
    return nullptr;
  }

  Job* at(size_t index) { return index < N ? &jobs_[index] : nullptr; }
  const Job* at(size_t index) const { return index < N ? &jobs_[index] : nullptr; }
  static constexpr size_t capacity() { return N; }
  void clear() {
    for (auto& job : jobs_) job = Job{};
    nextId_ = 1;
    nextClaim_ = 1;
  }

 private:
  Job* prepare(Job& job, uint32_t now) {
    job = Job{};
    job.id = nextId_++;
    job.state = kPending;
    job.updatedAt = now;
    return &job;
  }

  Job jobs_[N]{};
  uint32_t nextId_ = 1;
  uint32_t nextClaim_ = 1;
};

}  // namespace plugin_jobs

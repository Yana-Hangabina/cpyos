#pragma once

// Minimal in-memory stand-in for lib/hal/HalStorage, enough to run
// book_cache::pruneRegenerable() on the host.  Shape (and the child-enumeration rule)
// follows test/library_builder/stubs/HalStorage.h, which this tree already uses, so the
// fake behaves like the shape the production callers expect: openNextFile() re-derives the
// children of a directory on every call.
//
// What it CANNOT prove is stated in run_book_cache_prune_test.sh: whether SdFat's real
// iterator tolerates removing the entry just opened.  That behaviour comes from
// SDCardManager::removeDir, which does exactly this on this filesystem, and from the
// device check described there -- not from this file.
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fakefs {

struct Node {
  bool directory = false;
  std::vector<uint8_t> bytes;
};

inline std::map<std::string, std::shared_ptr<Node>> nodes;
inline std::vector<std::string> removeDirCalls;
// Which iterator model this fake imitates.  IndexPositioned re-derives the child list on
// every openNextFile(), so a removal shifts later entries below the cursor (the model that
// made the first version of the sweep skip half the directory).  EntryPositioned snapshots
// the child list when the pass starts, which is how SdFat's directory cursor behaves: a
// freed entry is stepped over, not replaced by its neighbour.  The sweep must converge under
// both.
enum class Iterator { IndexPositioned, EntryPositioned };
inline Iterator iterator = Iterator::IndexPositioned;
inline std::vector<std::string> removeCalls;
inline std::string failRemovePath;  // the removal of this path fails
// With it set, whether the failure clears after the first attempt.  A one-shot models a transient
// SD hiccup (a later pass should finish the job); a sticky one models an entry that cannot be
// removed at all, which must be reported AND must not spin the sweep.
inline bool stickyRemoveFailure = false;

inline void reset() {
  nodes.clear();
  removeDirCalls.clear();
  iterator = Iterator::IndexPositioned;
  removeCalls.clear();
  failRemovePath.clear();
  // Every knob has to be cleared here.  Leaving stickyRemoveFailure behind made the next
  // case's one-shot failure permanent, so a transient-failure assertion silently turned into
  // a stuck-entry assertion -- and it only showed up in the second half of the run, because
  // the case that sets it runs after the case that reads it.
  stickyRemoveFailure = false;
}

inline void addDir(const std::string& path) {
  auto node = std::make_shared<Node>();
  node->directory = true;
  nodes[path] = node;
  const size_t slash = path.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    const std::string parent = path.substr(0, slash);
    if (!nodes.count(parent)) addDir(parent);
  }
}

inline void addFile(const std::string& path, const std::string& content = "x") {
  auto node = std::make_shared<Node>();
  node->bytes.assign(content.begin(), content.end());
  nodes[path] = node;
  const size_t slash = path.find_last_of('/');
  if (slash != std::string::npos && slash > 0) {
    const std::string parent = path.substr(0, slash);
    if (!nodes.count(parent)) addDir(parent);
  }
}

}  // namespace fakefs

class HalFile {
 public:
  std::shared_ptr<fakefs::Node> node;
  std::string path;
  size_t cursor = 0;
  // Per-handle, like a real directory cursor: a new pass opens a new handle and must see the
  // entries that still exist, not pass one's list.
  bool snapshotTaken = false;
  std::vector<std::string> snapshot;

  explicit operator bool() const { return bool(node); }
  bool isOpen() const { return bool(node); }
  bool close() {
    node.reset();
    return true;
  }
  bool isDirectory() const { return node && node->directory; }
  void rewindDirectory() { cursor = 0; }
  size_t getName(char* out, const size_t size) {
    const std::string name = path.substr(path.find_last_of('/') + 1);
    if (size > 0) {
      std::snprintf(out, size, "%s", name.c_str());
    }
    return name.size();
  }

  HalFile openNextFile() {
    if (fakefs::iterator == fakefs::Iterator::EntryPositioned) {
      if (!snapshotTaken) {
        snapshotTaken = true;
        for (const auto& [name, value] : fakefs::nodes) {
          const size_t slash = name.find_last_of('/');
          const std::string parent = slash == std::string::npos ? std::string() : name.substr(0, slash);
          if (name != path && parent == path) snapshot.push_back(name);
        }
      }
      while (cursor < snapshot.size()) {
        const std::string candidate = snapshot[cursor++];
        const auto found = fakefs::nodes.find(candidate);
        if (found != fakefs::nodes.end()) {
          HalFile file;
          file.path = candidate;
          file.node = found->second;
          return file;
        }
      }
      return {};
    }
    std::vector<std::string> children;
    for (const auto& [name, value] : fakefs::nodes) {
      const size_t slash = name.find_last_of('/');
      const std::string parent = slash == std::string::npos ? std::string() : name.substr(0, slash);
      if (name != path && parent == path) children.push_back(name);
    }
    if (cursor >= children.size()) return {};
    HalFile file;
    file.path = children[cursor++];
    file.node = fakefs::nodes[file.path];
    return file;
  }
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }

  bool exists(const char* path) { return fakefs::nodes.count(path) != 0; }
  HalFile open(const char* path) {
    HalFile file;
    const auto found = fakefs::nodes.find(path);
    if (found != fakefs::nodes.end()) {
      file.node = found->second;
      file.path = path;
    }
    return file;
  }
  bool mkdir(const char* path, const bool = true) {
    fakefs::addDir(path);
    return true;
  }
  bool remove(const char* path) {
    fakefs::removeCalls.emplace_back(path);
    if (fakefs::failRemovePath == path) {
      if (!fakefs::stickyRemoveFailure) fakefs::failRemovePath.clear();
      return false;
    }
    return fakefs::nodes.erase(path) != 0;
  }
  bool removeDir(const char* path) {
    fakefs::removeDirCalls.emplace_back(path);
    if (fakefs::failRemovePath == path) {
      if (!fakefs::stickyRemoveFailure) fakefs::failRemovePath.clear();
      return false;
    }
    const std::string prefix = std::string(path) + '/';
    for (auto it = fakefs::nodes.begin(); it != fakefs::nodes.end();) {
      if (it->first == path || it->first.rfind(prefix, 0) == 0) {
        it = fakefs::nodes.erase(it);
      } else {
        ++it;
      }
    }
    return true;
  }
};

#define Storage HalStorage::getInstance()

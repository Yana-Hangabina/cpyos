#include "HalStorage.h"

#include "cpyos_sd.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <esp_vfs_fat.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <freertos/semphr.h>

#include <cerrno>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>

HalStorage HalStorage::instance;
HalStorage::HalStorage() { storageMutex = xSemaphoreCreateRecursiveMutex(); }

HalStorage::StorageLock::StorageLock() {
  if (HalStorage::getInstance().storageMutex != nullptr) {
    xSemaphoreTakeRecursive(HalStorage::getInstance().storageMutex, portMAX_DELAY);
  }
}

HalStorage::StorageLock::~StorageLock() {
  if (HalStorage::getInstance().storageMutex != nullptr) {
    xSemaphoreGiveRecursive(HalStorage::getInstance().storageMutex);
  }
}

namespace {
// Every SD write is handed to the vendor SDMMC host, which must see INTERNAL,
// cache-line-aligned memory. Page/section caches and chapter buffers live in
// PSRAM, and handing those pointers to f_write made multi-sector writes time out
// (CMD13 0x107) and then wedge the card until a remount. Copy foreign buffers
// through this one internal 4 KiB buffer instead; it is guarded because the
// reader render task and the network jobs can both write.
constexpr size_t kWriteBounceBytes = 4096;
uint8_t* writeBounceBuffer() {
  static uint8_t* buffer = nullptr;
  if (buffer == nullptr) {
    buffer = static_cast<uint8_t*>(heap_caps_aligned_alloc(64, kWriteBounceBytes,
                                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  return buffer;
}
SemaphoreHandle_t writeBounceMutex() {
  static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  return mutex;
}

std::string configuredStorageRoot() {
  // The Plus BSP mounts the 4-bit SD card here.
  return std::string("/sdcard");
}

bool containsUnsafeSegment(const std::string &path) {
  std::stringstream stream(path);
  std::string segment;
  while (std::getline(stream, segment, '/')) {
    if (segment == "..") {
      return true;
    }
  }
  return false;
}

std::string resolveStoragePath(const char *path) {
  std::string logical = path ? std::string(path) : std::string("/");
  if (logical.empty()) {
    logical = "/";
  }
  if (containsUnsafeSegment(logical)) {
    fprintf(stderr, "[SIM] rejected unsafe storage path: %s\n",
            logical.c_str());
    return {};
  }
  while (!logical.empty() && logical.front() == '/') {
    logical.erase(logical.begin());
  }

  std::string root = configuredStorageRoot();
  while (root.size() > 1 && root.back() == '/') {
    root.pop_back();
  }
  if (logical.empty()) {
    return root;
  }
  return root + "/" + logical;
}

bool ensureParentDirectories(const std::string &full) {
  const size_t slash = full.find_last_of('/');
  if (slash == std::string::npos) {
    return true;
  }
  const std::string parent = full.substr(0, slash);
  if (parent.empty()) {
    return true;
  }
  for (size_t i = 1; i < parent.size(); ++i) {
    if (parent[i] == '/') {
      ::mkdir(parent.substr(0, i).c_str(), 0777);
    }
  }
  return ::mkdir(parent.c_str(), 0777) == 0 || errno == EEXIST;
}

// Defined below, next to the open-file registry they walk. Declared here because
// the sleep helpers above them use them.
size_t countOpenFiles();
bool parkOpenHandles();
bool unparkOpenHandles();
} // namespace

bool HalStorage::begin() {
  // The SD card is normally mounted by the BSP before the shell starts, but the
  // shared SD-error page retries through Storage.begin(): without this the retry
  // could never succeed on this port (it only re-created directories under an
  // unmounted VFS). cpyos_sd_mount() is a no-op when the card is already up.
  if (!cpyos_sd_is_mounted()) {
    cpyos_sd_mount();
  }
  if (!cpyos_sd_is_mounted()) {
    return false;
  }
  const std::string root = configuredStorageRoot();
  for (size_t i = 1; i < root.size(); ++i) {
    if (root[i] == '/') {
      ::mkdir(root.substr(0, i).c_str(), 0777);
    }
  }
  return ::mkdir(root.c_str(), 0777) == 0 || errno == EEXIST;
}
bool HalStorage::ready() const { return true; }

bool HalStorage::prepareForDeepSleep() {
  // The recursive storage mutex serialises this against every other multi-step
  // SD transaction (page/section writes, chapter writers, settings saves).
  StorageLock lock;
  const size_t remaining = countOpenFiles();
  if (remaining != 0) {
    fprintf(stderr, "[SD] quiesce refused: %u file(s) still open\n", static_cast<unsigned>(remaining));
    return false;
  }
  return true;
}

bool HalStorage::quiesceForLightSleep() {
  StorageLock lock;
  // Park (flush + close) every descriptor, then reopen it after wake. Light
  // sleep keeps RAM, so the FatFS window cache survives, but the SDMMC clock
  // stops while the MCU sleeps and the vendor's own policy requires the card to
  // have no live file object at that point.
  //
  // Closing without a reopen path was why standby over a book refused to sleep
  // (the reader's Section keeps one HalFile open for the whole book). Each
  // entry remembers its offset (and directory cursor), so the reader continues
  // exactly where it stopped. If parking itself fails, unpark what was already
  // closed before refusing, so a failed attempt never leaves the app with dead
  // handles.
  if (!parkOpenHandles()) {
    unparkOpenHandles();
    return false;  // fail closed: never sleep with live descriptors
  }
  parkedCardMounted = cpyos_sd_is_mounted();
  parkedForLightSleep = true;
  return true;
}

bool HalStorage::restoreAfterLightSleep() {
  StorageLock lock;
  if (!parkedForLightSleep) {
    return cpyos_sd_is_mounted();
  }
  parkedForLightSleep = false;
  if (!parkedCardMounted) {
    return unparkOpenHandles();  // no card was mounted when we parked
  }
  if (!cpyos_sd_is_mounted()) {
    // The card dropped off the bus while the MCU slept. The ladder releases the
    // pad holds itself and re-runs 40->20->10 MHz / 4-bit->1-bit.
    if (!cpyos_sd_mount()) {
      return false;
    }
  }
  // Reopen at the exact byte offset / directory cursor every handle held. A
  // handle the remounted volume cannot reopen stays invalid; its later reads
  // fail visibly instead of touching a stale descriptor.
  return unparkOpenHandles();
}

bool HalStorage::canParkForLightSleep() const {
  if (!cpyos_sd_is_mounted()) {
    return true;  // no card: quiescence is not required
  }
  if (storageMutex == nullptr) {
    return true;
  }
  // Provable "nothing is in flight": the recursive storage mutex is free right
  // now, so no multi-step SD transaction is mid-way. Open descriptors are not a
  // reason to refuse: quiesceForLightSleep() parks and reopens them. Single
  // HalFile reads do not take this mutex, so light sleep must only be attempted
  // from the idle standby loop (no page render or chapter job is running).
  if (xSemaphoreTakeRecursive(storageMutex, 0) != pdTRUE) {
    return false;
  }
  xSemaphoreGiveRecursive(storageMutex);
  return true;
}

bool HalStorage::beginUsbDrive() { return false; }
bool HalStorage::disconnectUsbDriveHost() { return false; }
void HalStorage::endUsbDrive() {}
UsbDriveState HalStorage::usbDriveState() const {
  return UsbDriveState::Unsupported;
}
bool HalStorage::usbDriveHostSuspended() const { return false; }

bool HalStorage::getSpace(uint64_t &totalBytes, uint64_t &freeBytes) {
  totalBytes = 0;
  freeBytes = 0;

  // FatFS's statvfs path is not wired through this VFS mount on every IDF
  // target (measured: it fails, so the About row showed "--" and the dictionary
  // installer reported "SD 卡空间不足" on an almost empty card). esp_vfs_fat_info
  // is the FatFS-native query and is exact; fall back to statvfs for other
  // storage backends.
  {
    uint64_t total = 0;
    uint64_t free = 0;
    if (::esp_vfs_fat_info(configuredStorageRoot().c_str(), &total, &free) == ESP_OK && total > 0 &&
        free <= total) {
      totalBytes = total;
      freeBytes = free;
      return true;
    }
  }

  struct statvfs space {};
  if (::statvfs(configuredStorageRoot().c_str(), &space) != 0)
    return false;

  const uint64_t blockSize = space.f_frsize ? space.f_frsize : space.f_bsize;
  const uint64_t maxBytes = std::numeric_limits<uint64_t>::max();
  if (blockSize == 0 || space.f_blocks > maxBytes / blockSize ||
      space.f_bavail > maxBytes / blockSize)
    return false;

  const uint64_t total = space.f_blocks * blockSize;
  const uint64_t free = space.f_bavail * blockSize;
  if (total == 0 || free > total)
    return false;

  totalBytes = total;
  freeBytes = free;
  return true;
}

// Open-file registry.
//
// Light sleep stops the SD clock and cuts the peripheral rails, so the card must
// be unmounted with no live FAT file object. The POSIX VFS underneath HalFile
// has no "close everything" call, and this port cannot enumerate descriptors
// from the kernel, so every Impl links itself here while it owns a descriptor or
// a directory stream. prepareForDeepSleep() then walks the list, fsync()s and
// closes each entry, and fails if anything survives. Without the registry the
// only honest answer would be "cannot prove", i.e. never sleep.
namespace {

struct plus_open_file_link {
  int *fd = nullptr;
  DIR **dir = nullptr;
  std::string *path = nullptr;
  int *flags = nullptr;
  size_t *dirPos = nullptr;
  // Light-sleep parking state: what to reopen and where to resume.
  bool parked = false;
  bool parkedDir = false;
  // The fd was already closed before the park began (the registry outlived
  // its file object). Such an entry is dead, not a reason to refuse sleep.
  bool stale = false;
  int parkedFlags = 0;
  size_t parkedOffset = 0;
  plus_open_file_link *next = nullptr;
  bool linked = false;
};

SemaphoreHandle_t openFileMutex() {
  static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  return mutex;
}

plus_open_file_link *s_openFilesHead = nullptr;

// Set between parkOpenHandles() and unparkOpenHandles(): new opens are refused
// so a task that wakes between parking and esp_light_sleep_start() cannot leave
// a fresh FAT file object open on a card whose clock is about to stop.
std::atomic<bool> s_handlesParked{false};

void registerOpenFile(plus_open_file_link *link) {
  SemaphoreHandle_t mutex = openFileMutex();
  if (mutex == nullptr) return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (!link->linked) {
    link->next = s_openFilesHead;
    s_openFilesHead = link;
    link->linked = true;
  }
  xSemaphoreGive(mutex);
}

void unregisterOpenFile(plus_open_file_link *link) {
  SemaphoreHandle_t mutex = openFileMutex();
  if (mutex == nullptr) return;
  xSemaphoreTake(mutex, portMAX_DELAY);
  if (link->linked) {
    plus_open_file_link **slot = &s_openFilesHead;
    while (*slot != nullptr) {
      if (*slot == link) {
        *slot = link->next;
        break;
      }
      slot = &(*slot)->next;
    }
    link->next = nullptr;
    link->linked = false;
  }
  xSemaphoreGive(mutex);
}

// Returns the number of entries that are still open (0 = quiesced).
// Used by prepareForDeepSleep(), which still refuses to leave live handles
// behind (deep sleep ends the process anyway and must not cut a mid-write).
size_t countOpenFiles() {
  size_t remaining = 0;
  SemaphoreHandle_t mutex = openFileMutex();
  if (mutex != nullptr) xSemaphoreTake(mutex, portMAX_DELAY);
  for (plus_open_file_link *it = s_openFilesHead; it != nullptr; it = it->next) {
    if ((it->fd != nullptr && *it->fd >= 0) || (it->dir != nullptr && *it->dir != nullptr)) {
      ++remaining;
    }
  }
  if (mutex != nullptr) xSemaphoreGive(mutex);
  return remaining;
}

// Light-sleep parking: flush + close every open descriptor, remembering what to
// reopen and where to resume. The reader's Section keeps one file open for the
// whole book, so closing without a reopen path is what made standby over a book
// refuse to sleep. Reads/writes are byte-addressed on this VFS, so reopening the
// same path and seeking to the saved offset is loss-free; directories replay
// their raw-entry cursor (a POSIX telldir cookie is not valid for a new stream).
//
// Caller holds the storage lock. The open-file mutex is held while descriptors
// are closed so the registry cannot be mutated mid-walk; POSIX close/fsync are
// short and this only runs on the standby path.
bool parkOpenHandles() {
  bool ok = true;
  // Refuse new opens for the whole park window (cleared by unparkOpenHandles,
  // which the caller runs on failure too).
  s_handlesParked.store(true);
  SemaphoreHandle_t mutex = openFileMutex();
  if (mutex != nullptr) xSemaphoreTake(mutex, portMAX_DELAY);
  for (plus_open_file_link *it = s_openFilesHead; it != nullptr; it = it->next) {
    if (it->parked) continue;
    if (it->fd != nullptr && *it->fd >= 0) {
      errno = 0;
      const off_t pos = lseek(*it->fd, 0, SEEK_CUR);
      if (pos < 0 && errno == EBADF) {
        // The descriptor was closed while this registry entry stayed linked
        // (the log mirror's rotating file did exactly that). Nothing is open on
        // the card, so sleeping is safe; refusing here used to disable light
        // sleep for the rest of the session.
        fprintf(stderr, "[SD] park: dropping already-closed handle (path=%s)\n",
                it->path != nullptr ? it->path->c_str() : "?");
        it->stale = true;
        it->parked = true;
        it->parkedDir = false;
        *it->fd = -1;
        continue;
      }
      if (pos < 0) {
        // Unknown offset: still close (the sleep decision is about the card),
        // but report the loss and resume at 0 rather than seek to garbage.
        fprintf(stderr, "[SD] park: current offset unknown (errno=%d)\n", errno);
        ok = false;
      }
      // Only a writable handle has anything to flush; a read-only file object
      // must not turn a no-op fsync into a refused sleep.
      const bool writable = (it->flags != nullptr && (*it->flags & (O_WRONLY | O_RDWR)) != 0);
      if (writable && fsync(*it->fd) != 0 && errno != EBADF) {
        fprintf(stderr, "[SD] park: fsync failed (errno=%d)\n", errno);
        ok = false;
      }
      if (::close(*it->fd) != 0 && errno != EBADF) {
        fprintf(stderr, "[SD] park: close failed (errno=%d)\n", errno);
        ok = false;
      }
      it->parked = true;
      it->parkedDir = false;
      it->parkedFlags = it->flags != nullptr ? *it->flags : O_RDONLY;
      it->parkedOffset = pos > 0 ? static_cast<size_t>(pos) : 0;
      *it->fd = -1;
    } else if (it->dir != nullptr && *it->dir != nullptr) {
      if (::closedir(*it->dir) != 0) {
        fprintf(stderr, "[SD] park: closedir failed (errno=%d)\n", errno);
        ok = false;
      }
      *it->dir = nullptr;
      it->parked = true;
      it->parkedDir = true;
      it->parkedOffset = it->dirPos != nullptr ? *it->dirPos : 0;
    }
  }
  if (mutex != nullptr) xSemaphoreGive(mutex);
  // The flag stays set until unparkOpenHandles() (also the failure path), so a
  // partially parked state can never be mixed with fresh opens.
  return ok;
}

// Reopens every parked descriptor at its saved offset. Failures are reported
// once per handle; the handle stays invalid, so a later read/write fails
// visibly instead of using a stale descriptor. Returns false if any failed.
bool unparkOpenHandles() {
  bool ok = true;
  size_t failed = 0;
  SemaphoreHandle_t mutex = openFileMutex();
  if (mutex != nullptr) xSemaphoreTake(mutex, portMAX_DELAY);
  for (plus_open_file_link *it = s_openFilesHead; it != nullptr; it = it->next) {
    if (!it->parked) continue;
    if (it->stale) {
      it->parked = false;
      continue;
    }
    const char *path = it->path != nullptr ? it->path->c_str() : "";
    if (it->parkedDir) {
      DIR *dir = *path != '\0' ? ::opendir(path) : nullptr;
      bool restored = dir != nullptr;
      if (restored) {
        for (size_t i = 0; i < it->parkedOffset; ++i) {
          if (::readdir(dir) == nullptr) {
            restored = false;
            break;
          }
        }
      }
      if (restored) {
        if (it->dir != nullptr) *it->dir = dir;
        if (it->dirPos != nullptr) *it->dirPos = it->parkedOffset;
      } else {
        if (dir != nullptr) ::closedir(dir);
        fprintf(stderr, "[SD] rescue: cannot reopen directory %s\n", path);
        ++failed;
      }
    } else {
      const int fd = *path != '\0' ? ::open(path, it->parkedFlags, 0666) : -1;
      bool restored = fd >= 0;
      if (restored && it->parkedOffset > 0) {
        restored = lseek(fd, static_cast<off_t>(it->parkedOffset), SEEK_SET) >= 0;
      }
      if (restored) {
        if (it->fd != nullptr) *it->fd = fd;
      } else {
        if (fd >= 0) ::close(fd);
        fprintf(stderr, "[SD] rescue: cannot reopen %s\n", path);
        ++failed;
      }
    }
    it->parked = false;
  }
  if (mutex != nullptr) xSemaphoreGive(mutex);
  s_handlesParked.store(false);
  if (failed != 0) {
    fprintf(stderr, "[SD] light-sleep resume: %u handle(s) could not be reopened\n",
            static_cast<unsigned>(failed));
    ok = false;
  }
  return ok;
}

}  // namespace

class HalFile::Impl {
public:
  int fd = -1;
  std::string path;
  DIR *dir = nullptr;
  size_t directoryPosition = 0;
  // Creation flags, replayed when light sleep parks and reopens the handle.
  int openFlags = O_RDONLY;
  // Intrusive node in the open-file registry (see below). Every descriptor and
  // directory stream this port opens goes through here, which is what lets
  // prepareForDeepSleep() prove the card is quiesced and quiesceForLightSleep()
  // park/reopen it instead of guessing.
  plus_open_file_link link;

  Impl() {
    link.fd = &fd;
    link.dir = &dir;
    link.path = &path;
    link.flags = &openFlags;
    link.dirPos = &directoryPosition;
  }

  bool open(const char *p, int flags) {
    if (s_handlesParked.load()) {
      fprintf(stderr, "[SD] open refused while parked for light sleep: %s\n", p != nullptr ? p : "?");
      errno = EBUSY;
      return false;
    }
    path = p;
    openFlags = flags;
    // The simulator's FsApiConstants.h just includes <fcntl.h> and typedef int
    // oflag_t, so all O_* constants are already native POSIX values — pass them
    // straight through.
    fd = ::open(path.c_str(), flags, 0666);
    if (fd < 0) {
      fprintf(stderr, "[SIM] open failed: %s (flags=0x%x errno=%d %s)\n",
              path.c_str(), flags, errno, strerror(errno));
      return false;
    }
    registerOpenFile(&link);
    return true;
  }

  bool openAsDir(const char *p) {
    if (s_handlesParked.load()) {
      fprintf(stderr, "[SD] opendir refused while parked for light sleep: %s\n", p != nullptr ? p : "?");
      errno = EBUSY;
      return false;
    }
    path = p;
    dir = opendir(p);
    directoryPosition = 0;
    if (dir == nullptr) {
      return false;
    }
    registerOpenFile(&link);
    return true;
  }

  bool isDir() const { return dir != nullptr; }
  bool isOpen() const { return fd >= 0 || dir != nullptr; }
};

HalFile::HalFile() : impl(new Impl()) {}
HalFile::~HalFile() {
  if (!impl) return;
  if (impl->dir != nullptr) {
    ::closedir(impl->dir);
    impl->dir = nullptr;
  }
  if (impl->fd >= 0) {
    ::close(impl->fd);
    impl->fd = -1;
  }
  unregisterOpenFile(&impl->link);
}
HalFile::HalFile(HalFile &&other) : impl(std::move(other.impl)) {}
HalFile &HalFile::operator=(HalFile &&other) {
  if (this != &other) {
    if (impl) {
      if (impl->dir != nullptr) {
        ::closedir(impl->dir);
        impl->dir = nullptr;
      }
      if (impl->fd >= 0) {
        ::close(impl->fd);
        impl->fd = -1;
      }
      unregisterOpenFile(&impl->link);
    }
    impl = std::move(other.impl);
  }
  return *this;
}

void HalFile::flush() {
  if (impl && impl->fd >= 0)
    fsync(impl->fd);
}
bool HalFile::sync() {
  if (!impl || impl->fd < 0)
    return false;
  return fsync(impl->fd) == 0;
}
size_t HalFile::getName(char *name, size_t len) {
  if (!impl || impl->path.empty())
    return 0;
  size_t slash = impl->path.rfind('/');
  std::string fname =
      (slash == std::string::npos) ? impl->path : impl->path.substr(slash + 1);
  size_t n = std::min(fname.size(), len - 1);
  memcpy(name, fname.c_str(), n);
  name[n] = '\0';
  return n;
}
size_t HalFile::size() {
  if (!impl || impl->fd < 0)
    return 0;
  off_t cur = lseek(impl->fd, 0, SEEK_CUR);
  off_t end = lseek(impl->fd, 0, SEEK_END);
  lseek(impl->fd, cur, SEEK_SET);
  return end < 0 ? 0 : (size_t)end;
}
size_t HalFile::fileSize() { return size(); }
uint64_t HalFile::fileSize64() { return size(); }
uint32_t HalFile::modificationTime() {
  if (!impl || impl->path.empty())
    return 0;

  struct stat info{};
  const int result = (impl->fd >= 0) ? fstat(impl->fd, &info)
                                     : stat(impl->path.c_str(), &info);
  return result == 0 ? static_cast<uint32_t>(info.st_mtime) : 0;
}
bool HalFile::seek(size_t pos) {
  if (!impl || impl->fd < 0)
    return false;
  return lseek(impl->fd, (off_t)pos, SEEK_SET) >= 0;
}
bool HalFile::seek64(uint64_t pos) {
  if (!impl || impl->fd < 0)
    return false;
  if (pos > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
    return false;
  return lseek(impl->fd, static_cast<off_t>(pos), SEEK_SET) >= 0;
}
bool HalFile::seekCur(int64_t offset) {
  if (!impl || impl->fd < 0)
    return false;
  return lseek(impl->fd, (off_t)offset, SEEK_CUR) >= 0;
}
bool HalFile::seekSet(size_t offset) {
  if (!impl) return false;
  if (impl->dir) {
    // LibraryBuilder closes a parent directory while it walks a child, then
    // reopens and resumes it. A POSIX telldir cookie is not valid for a new
    // stream, so replay the stable count of raw directory entries instead.
    rewinddir(impl->dir);
    for (size_t i = 0; i < offset; ++i) {
      if (readdir(impl->dir) == nullptr) return false;
    }
    impl->directoryPosition = offset;
    return true;
  }
  if (impl->fd < 0) return false;
  return lseek(impl->fd, (off_t)offset, SEEK_SET) >= 0;
}
bool HalFile::truncate(uint64_t length) {
  if (!impl || impl->fd < 0 ||
      length > static_cast<uint64_t>(std::numeric_limits<off_t>::max()))
    return false;
  return ftruncate(impl->fd, static_cast<off_t>(length)) == 0;
}
int HalFile::available() const {
  if (!impl || impl->fd < 0)
    return 0;
  off_t cur = lseek(impl->fd, 0, SEEK_CUR);
  off_t end = lseek(impl->fd, 0, SEEK_END);
  lseek(impl->fd, cur, SEEK_SET);
  return (int)(end - cur);
}
size_t HalFile::position() const {
  if (!impl) return 0;
  if (impl->dir) return impl->directoryPosition;
  if (impl->fd < 0) return 0;
  off_t pos = lseek(impl->fd, 0, SEEK_CUR);
  return pos < 0 ? 0 : (size_t)pos;
}
int HalFile::read(void *buf, size_t count) {
  if (!impl || impl->fd < 0)
    return -1;
  // POSIX read() may return fewer bytes than asked for even when more are
  // available; every caller here treats the return value as "all or EOF/error"
  // (JPEGDEC's callback aborts the whole image on a short read, which is why a
  // 200x150 test JPEG decoded and 640x480 failed with decoder error 2). Loop
  // until the buffer is full, the file ends (0) or a real error occurs.
  auto *p = static_cast<uint8_t *>(buf);
  size_t done = 0;
  while (done < count) {
    const ssize_t n = ::read(impl->fd, p + done, count - done);
    if (n > 0) {
      done += static_cast<size_t>(n);
      continue;
    }
    if (n == 0) {
      break;  // EOF
    }
    if (errno == EINTR) {
      continue;
    }
    if (done == 0) {
      return -1;
    }
    break;
  }
  return static_cast<int>(done);
}
int HalFile::read() {
  if (!impl || impl->fd < 0)
    return -1;
  uint8_t c;
  return (::read(impl->fd, &c, 1) == 1) ? c : -1;
}
size_t HalFile::write(const void *buf, size_t count) {
  if (!impl || impl->fd < 0)
    return 0;
  // Short writes are legal for POSIX write() and this VFS does return them
  // (measured: the JPEG->BMP converter wrote a complete header and every row
  // and still reported failure because one call came back short). Arduino's
  // File::write contract, which every caller here assumes, is "all or error",
  // so loop until the buffer is out; only a real error ends the loop early.
  const auto *p = static_cast<const uint8_t *>(buf);
  const bool dmaSafe = esp_ptr_internal(buf) && ((reinterpret_cast<uintptr_t>(buf) & 63U) == 0);
  uint8_t* bounce = dmaSafe ? nullptr : writeBounceBuffer();
  SemaphoreHandle_t bounceLock = nullptr;
  if (!dmaSafe) {
    if (bounce == nullptr) return 0;
    bounceLock = writeBounceMutex();
    if (bounceLock != nullptr) xSemaphoreTake(bounceLock, portMAX_DELAY);
  }
  size_t done = 0;
  while (done < count) {
    const uint8_t* chunk = p + done;
    size_t chunkLen = count - done;
    if (!dmaSafe) {
      if (chunkLen > kWriteBounceBytes) chunkLen = kWriteBounceBytes;
      memcpy(bounce, chunk, chunkLen);
      chunk = bounce;
    }
    const ssize_t n = ::write(impl->fd, chunk, chunkLen);
    if (n > 0) {
      done += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    fprintf(stderr, "[SDW] write %s failed at %u/%u errno=%d (%s)\n", impl->path.c_str(),
            static_cast<unsigned>(done), static_cast<unsigned>(count), errno, strerror(errno));
    break;
  }
  if (bounceLock != nullptr) xSemaphoreGive(bounceLock);
  return done;
}
size_t HalFile::write(const uint8_t *buf, size_t count) {
  return write(static_cast<const void *>(buf), count);
}
size_t HalFile::write(uint8_t b) {
  if (!impl || impl->fd < 0)
    return 0;
  return (::write(impl->fd, &b, 1) == 1) ? 1 : 0;
}
bool HalFile::rename(const char *newPath) {
  if (!impl || impl->path.empty()) {
    return false;
  }
  const std::string resolved = resolveStoragePath(newPath);
  if (resolved.empty()) {
    return false;
  }
  close();
  ensureParentDirectories(resolved);
  return ::rename(impl->path.c_str(), resolved.c_str()) == 0;
}
bool HalFile::isDirectory() const { return impl && impl->isDir(); }
void HalFile::rewindDirectory() {
  if (impl && impl->dir) {
    rewinddir(impl->dir);
    impl->directoryPosition = 0;
  }
}
bool HalFile::close() {
  if (!impl)
    return true;
  if (impl->dir) {
    closedir(impl->dir);
    impl->dir = nullptr;
  }
  if (impl->fd >= 0) {
    // Commit before the descriptor disappears: the host resets this SoC by
    // opening the USB-Serial/JTAG port, and a reset that lands while the card
    // still holds dirty write cache can leave it needing a power cycle.
    ::fsync(impl->fd);
    ::close(impl->fd);
    impl->fd = -1;
  }
  unregisterOpenFile(&impl->link);
  return true;
}
HalFile HalFile::openNextFile() {
  if (!impl || !impl->dir)
    return HalFile();
  while (true) {
    struct dirent *entry = readdir(impl->dir);
    if (!entry)
      return HalFile();
    ++impl->directoryPosition;
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue; // skip . and ..

    std::string childFsPath = impl->path;
    if (childFsPath.back() != '/')
      childFsPath += '/';
    childFsPath += entry->d_name;

    HalFile child;
    struct stat st;
    if (stat(childFsPath.c_str(), &st) != 0)
      continue;

    if (S_ISDIR(st.st_mode)) {
      child.impl->openAsDir(childFsPath.c_str());
    } else {
      child.impl->open(childFsPath.c_str(), O_RDONLY);
    }
    return child;
  }
}
bool HalFile::isOpen() const {
  if (!impl)
    return false;
  return impl->isOpen();
}
HalFile::operator bool() const { return isOpen(); }

HalFile HalStorage::open(const char *path, const oflag_t oflag) {
  std::string full = resolveStoragePath(path);
  HalFile f;
  if (full.empty()) {
    return f;
  }
  if ((oflag & O_CREAT) != 0) {
    ensureParentDirectories(full);
  }
  struct stat st;
  if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
    f.impl->openAsDir(full.c_str());
  } else {
    f.impl->open(full.c_str(), oflag);
  }
  return f;
}
bool HalStorage::mkdir(const char *path, const bool /*pFlag*/) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  // Create all intermediate directories (mkdir -p semantics).
  for (size_t i = 1; i < full.size(); ++i) {
    if (full[i] == '/') {
      ::mkdir(full.substr(0, i).c_str(),
              0777); // ignore errors (may already exist)
    }
  }
  return ::mkdir(full.c_str(), 0777) == 0 || errno == EEXIST;
}
bool HalStorage::exists(const char *path) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  struct stat buffer;
  return (stat(full.c_str(), &buffer) == 0);
}
bool HalStorage::remove(const char *path) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  return ::remove(full.c_str()) == 0;
}
bool HalStorage::rename(const char *oldPath, const char *newPath) {
  std::string o = resolveStoragePath(oldPath);
  std::string n = resolveStoragePath(newPath);
  if (o.empty() || n.empty()) {
    return false;
  }
  ensureParentDirectories(n);
  return ::rename(o.c_str(), n.c_str()) == 0;
}
bool HalStorage::replaceFile(const char *tmpPath, const char *path) {
  return rename(tmpPath, path);
}
static bool removeDirRecursive(const std::string &full) {
  DIR *d = opendir(full.c_str());
  if (!d)
    return ::remove(full.c_str()) == 0; // might be a plain file
  struct dirent *entry;
  while ((entry = readdir(d)) != nullptr) {
    if (entry->d_name[0] == '.')
      continue;
    std::string child = full + "/" + entry->d_name;
    struct stat st;
    if (stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      removeDirRecursive(child);
    } else {
      ::remove(child.c_str());
    }
  }
  closedir(d);
  return ::rmdir(full.c_str()) == 0;
}

bool HalStorage::rmdir(const char *path) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  return removeDirRecursive(full);
}
bool HalStorage::removeDir(const char *path) {
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return false;
  }
  return removeDirRecursive(full);
}

String HalStorage::readFile(const char *path) {
  HalFile f = open(path, O_RDONLY);
  if (!f)
    return String("");
  size_t s = f.size();
  std::string content(s, '\0');
  f.read((void *)content.data(), s);
  return String(content);
}
bool HalStorage::readFileToStream(const char *path, Print &out,
                                  size_t chunkSize) {
  HalFile f = open(path, O_RDONLY);
  if (!f)
    return false;
  std::vector<char> buf(chunkSize);
  int n;
  while ((n = f.read(buf.data(), chunkSize)) > 0) {
    out.write(reinterpret_cast<const uint8_t *>(buf.data()), n);
  }
  return true;
}
size_t HalStorage::readFileToBuffer(const char *path, char *buffer,
                                    size_t bufferSize, size_t maxBytes) {
  HalFile f = open(path, O_RDONLY);
  if (!f)
    return 0;
  size_t toRead = bufferSize - 1;
  if (maxBytes > 0 && maxBytes < toRead)
    toRead = maxBytes;
  int n = f.read(buffer, toRead);
  if (n < 0)
    n = 0;
  buffer[n] = '\0';
  return n;
}
bool HalStorage::readFileToString(const char *moduleName,
                                  const std::string &path, size_t cap,
                                  std::string &out) {
  out.clear();
  HalFile file;
  if (!openFileForRead(moduleName, path, file) || file.isDirectory())
    return false;
  const size_t size = file.fileSize();
  if (size == 0 || size > cap)
    return false;
  out.resize(size);
  if (file.read(out.data(), size) != static_cast<int>(size)) {
    out.clear();
    return false;
  }
  return true;
}
bool HalStorage::writeFile(const char *path, const String &content) {
  HalFile f = open(path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f)
    return false;
  f.write(content.c_str(), content.length());
  return true;
}
bool HalStorage::ensureDirectoryExists(const char *path) { return mkdir(path); }

bool HalStorage::openFileForRead(const char *moduleName, const char *path,
                                 HalFile &file) {
  file = open(path, O_RDONLY);
  return file.isOpen();
}
bool HalStorage::openFileForRead(const char *moduleName,
                                 const std::string &path, HalFile &file) {
  return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForRead(const char *moduleName, const String &path,
                                 HalFile &file) {
  return openFileForRead(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char *moduleName, const char *path,
                                  HalFile &file) {
  // O_RDWR, not O_WRONLY, to match SDCardManager::openFileForWrite on device
  // (O_RDWR | O_CREAT | O_TRUNC). Firmware reads back from a write handle while
  // it is still open -- Section::loadPageDuringBuild seeks into the .bin it is
  // building to render an already-written page. On a write-only fd that read
  // fails and the page renders blank.
  file = open(path, O_RDWR | O_CREAT | O_TRUNC);
  return file.isOpen();
}
bool HalStorage::openFileForWrite(const char *moduleName,
                                  const std::string &path, HalFile &file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}
bool HalStorage::openFileForWrite(const char *moduleName, const String &path,
                                  HalFile &file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

std::vector<String> HalStorage::listFiles(const char *path, int maxFiles) {
  std::vector<String> result;
  std::string full = resolveStoragePath(path);
  if (full.empty()) {
    return result;
  }
  DIR *dir = opendir(full.c_str());
  if (!dir)
    return result;
  struct dirent *entry;
  while ((entry = readdir(dir)) != nullptr && (int)result.size() < maxFiles) {
    if (entry->d_name[0] == '.')
      continue; // skip . and ..
    result.push_back(String(entry->d_name));
  }
  closedir(dir);
  return result;
}

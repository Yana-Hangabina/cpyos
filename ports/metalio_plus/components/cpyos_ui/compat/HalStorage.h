#pragma once

#include <Print.h>
#include <Stream.h>
#include <common/FsApiConstants.h> // for oflag_t
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "WString.h"

class HalFile;

enum class UsbDriveState : uint8_t {
  Unsupported,
  WaitingForHost,
  Connected,
  Ejected,
  Disconnected,
  IoError,
};

class HalStorage {
public:
  HalStorage();
  bool begin();
  bool ready() const;
  // Debug aid: the shared file layer has no reported mutex holder.
  const char* debugMutexHolderName() const { return "(none)"; }
  bool getSpace(uint64_t &totalBytes, uint64_t &freeBytes);
  // Quiesce the card for a sleep that keeps the MCU alive (standby light sleep)
  // or for the power-down path: flush and close every open file (regular files
  // and directory streams) while holding the storage lock. Returns false when a
  // descriptor could not be flushed/closed: the card is then NOT quiesced and
  // light sleep must not park. The open HalFiles are invalid afterwards and
  // their owners reopen them.
  //
  // The volume stays mounted on purpose. This board powers the card from the
  // always-on rail and light sleep retains RAM, so FatFS's cached metadata is
  // never lost; unmounting would also make the SD health watchdog (raw fd, 15 s
  // probe, reboot after two misses) tear the device down during a long standby.
  // It refuses while any HalFile/DIR is open, because deep sleep ends the
  // process and must not abandon a handle mid-write.
  bool prepareForDeepSleep();
  // Sleep-park entry point: flushes and closes every open descriptor (the
  // reader's Section keeps one for the whole book) and records the byte offset
  // or directory cursor. New opens are refused until restoreAfterLightSleep()
  // so a task running between parking and sleep cannot leave a fresh handle on
  // the card. False = do not sleep (fail closed).
  bool quiesceForLightSleep();
  // Counterpart used after wake: remounts a card that vanished during sleep and
  // reopens every parked handle at its saved offset. False = at least one handle
  // could not be reopened (it stays invalid; later I/O fails visibly).
  bool restoreAfterLightSleep();
  // True when parking needs nothing (no card mounted) or is provably possible
  // right now: no SD transaction holds the lock. Open descriptors are fine —
  // they are parked and reopened.
  bool canParkForLightSleep() const;
  bool beginUsbDrive();
  bool disconnectUsbDriveHost();
  void endUsbDrive();
  UsbDriveState usbDriveState() const;
  bool usbDriveHostSuspended() const;
  std::vector<String> listFiles(const char *path = "/", int maxFiles = 200);
  // Read the entire file at `path` into a String. Returns empty string on
  // failure.
  String readFile(const char *path);
  // Low-memory helpers:
  // Stream the file contents to a `Print` (e.g. `Serial`, or any
  // `Print`-derived object). Returns true on success, false on failure.
  bool readFileToStream(const char *path, Print &out, size_t chunkSize = 256);
  // Read up to `bufferSize-1` bytes into `buffer`, null-terminating it. Returns
  // bytes read.
  size_t readFileToBuffer(const char *path, char *buffer, size_t bufferSize,
                          size_t maxBytes = 0);
  bool readFileToString(const char *moduleName, const std::string &path,
                        size_t cap, std::string &out);
  // Write a string to `path` on the SD card. Overwrites existing file.
  // Returns true on success.
  bool writeFile(const char *path, const String &content);
  // Ensure a directory exists, creating it if necessary. Returns true on
  // success.
  bool ensureDirectoryExists(const char *path);

  HalFile open(const char *path, const oflag_t oflag = O_RDONLY);
  bool mkdir(const char *path, const bool pFlag = true);
  bool exists(const char *path);
  bool remove(const char *path);
  bool rename(const char *oldPath, const char *newPath);
  bool replaceFile(const char *tmpPath, const char *path);
  bool rmdir(const char *path);

  bool openFileForRead(const char *moduleName, const char *path, HalFile &file);
  bool openFileForRead(const char *moduleName, const std::string &path,
                       HalFile &file);
  bool openFileForRead(const char *moduleName, const String &path,
                       HalFile &file);
  bool openFileForWrite(const char *moduleName, const char *path,
                        HalFile &file);
  bool openFileForWrite(const char *moduleName, const std::string &path,
                        HalFile &file);
  bool openFileForWrite(const char *moduleName, const String &path,
                        HalFile &file);
  bool removeDir(const char *path);

  static HalStorage &getInstance() { return instance; }

  // RAII guard serializing multi-step SD transactions. The shared font pack
  // streams glyph tables across several reads; without this a render task can
  // interleave and corrupt the block device state.
  class StorageLock {
   public:
    StorageLock();
    ~StorageLock();
    StorageLock(const StorageLock&) = delete;
    StorageLock& operator=(const StorageLock&) = delete;
  };

private:
  static HalStorage instance;

  bool initialized = false;
  SemaphoreHandle_t storageMutex = nullptr;
  // Set by quiesceForLightSleep(): the wake path must run
  // restoreAfterLightSleep() before the app relies on the volume again.
  bool parkedForLightSleep = false;
  // Whether a card was actually mounted when we parked (a card-less device has
  // nothing to restore and must not fail the wake).
  bool parkedCardMounted = false;
};

#define Storage HalStorage::getInstance()

class HalFile : public Print {
  friend class HalStorage;
  class Impl;
  std::unique_ptr<Impl> impl;
  explicit HalFile(std::unique_ptr<Impl> impl);

public:
  HalFile();
  ~HalFile();
  HalFile(HalFile &&);
  HalFile &operator=(HalFile &&);
  HalFile(const HalFile &) = delete;
  HalFile &operator=(const HalFile &) = delete;

  void flush() override;
  size_t getName(char *name, size_t len);
  size_t size();
  size_t fileSize();
  uint64_t fileSize64();
  uint32_t modificationTime();
  bool seek(size_t pos);
  bool seek64(uint64_t pos);
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset);
  bool truncate(uint64_t length);
  int available() const;
  size_t position() const;
  int read(void *buf, size_t count);
  int read(); // read a single byte
  size_t write(const void *buf, size_t count);
  size_t write(const uint8_t *buf, size_t count) override;
  size_t write(uint8_t b) override;
  bool sync();
  bool rename(const char *newPath);
  bool isDirectory() const;
  void rewindDirectory();
  bool close();
  HalFile openNextFile();
  bool isOpen() const;
  operator bool() const;
};

// Only do renaming FsFile to HalFile if this header is included by downstream
// code The renaming is to allow using the thread-safe HalFile instead of the
// raw FsFile, without needing to change the downstream code
#ifndef HAL_STORAGE_IMPL
using FsFile = HalFile;
#endif

// Downstream code must use Storage instead of SdMan
#ifdef SdMan
#undef SdMan
#endif

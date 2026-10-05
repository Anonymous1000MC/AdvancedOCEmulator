#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace ocemu {

// Downloads the OpenOS EEPROM bootloader on a worker thread and reports progress,
// so the UI thread never stalls behind a network round-trip.
//
// The payload is what the emulator's "Restore to OpenOS" button writes back into
// the machine's EEPROM. That file is the bootloader: overwrite it (typically by
// flashing another OS installer) and the machine stops booting OpenOS.
class OpenOsRestore {
 public:
  ~OpenOsRestore();

  // Starts a download. Does nothing if one is already in flight.
  void start(const std::string& url);

  [[nodiscard]] bool running() const { return running_.load(); }
  // 0..1; indeterminate (<0) while the size is not yet known.
  [[nodiscard]] float progress() const { return progress_.load(); }
  [[nodiscard]] const std::string& status() const { return status_; }
  [[nodiscard]] const std::string& error() const { return error_; }

  // True once a download finished successfully; resets the latch.
  [[nodiscard]] bool succeeded() { return succeeded_.exchange(false); }
  [[nodiscard]] std::string takePayload();

  // Blocks until the worker finishes. Used on shutdown.
  void join();

  // Called from the libcurl progress callback on the worker thread.
  void setProgress(float f) { progress_ = f; }

 private:
  void run(std::string url);

  std::thread worker_;
  std::atomic<bool> running_{false};
  std::atomic<bool> succeeded_{false};
  std::atomic<float> progress_{-1.0f};
  std::string status_;
  std::string error_;
  std::string payload_;
};

// The default restore source. Overridable with OCEMU_OPENOS_URL.
const char* defaultOpenOsUrl();

// Locates the machine's EEPROM `code.lua` inside an OCEmu machine-data
// directory by reading the `eeprom` entry out of ocemu.cfg. Returns an empty
// string when it cannot be found.
std::string findEepromCodePath(const std::string& machineDataDir);

}  // namespace ocemu

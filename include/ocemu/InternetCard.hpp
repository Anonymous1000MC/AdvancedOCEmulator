// ---------------------------------------------------------------------------
//  Out-of-the-box `internet` component, backed by the host's libcurl.
//
//  Requests are driven by curl's multi interface, which is polled from the
//  render loop rather than run on a worker thread. That is deliberate:
//
//    * libcurl easy handles are not thread-safe, and
//    * the Lua state may only be touched from one thread.
//
//  Lua therefore never blocks: `internet.request()` starts a transfer and
//  returns a handle immediately, `internet.poll()` drains body chunks as
//  libcurl delivers them, and `host.sleep()` pumps the multi handle so a guest
//  can write simple synchronous-looking code.
// ---------------------------------------------------------------------------
#pragma once

#include <curl/curl.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

namespace ocemu {

enum class InternetStatus {
  Pending,
  Complete,
  Failed,
  InvalidHandle,
};

[[nodiscard]] const char* internetStatusName(InternetStatus s);

// Plain-old-data result so the Lua bindings can fill it without any
// non-trivial destructor that a Lua panic (longjmp) could skip.
struct InternetPoll {
  InternetStatus status = InternetStatus::Pending;
  bool hasChunk = false;
  char chunk[4096]{};
  std::size_t chunkLen = 0;
  long httpCode = 0;
  char error[256]{};
};

class InternetCard {
 public:
  // Caps that keep a runaway download from exhausting host memory.
  static constexpr std::size_t kMaxBodyBytes = 2u * 1024u * 1024u;
  static constexpr std::size_t kMaxChunksQueued = 512;
  static constexpr std::size_t kMaxChunksBuffered = 4u * 1024u * 1024u;
  static constexpr long kTimeoutMs = 20000;
  static constexpr long kConnectTimeoutMs = 8000;

  InternetCard() = default;
  ~InternetCard();
  InternetCard(const InternetCard&) = delete;
  InternetCard& operator=(const InternetCard&) = delete;

  bool init(std::string* error = nullptr);
  void shutdown();
  [[nodiscard]] bool ready() const { return multi_ != nullptr; }

  // --- component surface (called from Lua) --------------------------------
  // Starts a transfer. Returns a non-zero handle, or 0 on failure with the
  // reason written into `errBuf`.
  std::uint64_t request(const char* url, char* errBuf, std::size_t errCap);
  void poll(std::uint64_t handle, InternetPoll* out);
  // Copies up to `cap` bytes of the accumulated body; `totalLen` always
  // receives the full length so Lua can detect truncation.
  std::size_t copyResponse(std::uint64_t handle, char* dst, std::size_t cap,
                           std::size_t* totalLen);
  void close(std::uint64_t handle);

  // --- host loop ----------------------------------------------------------
  // Run pending transfers forward. Called once per frame.
  void pump();
  // Pump for up to `seconds`, returning early once `stopFlag` is set. Lets a
  // guest do `host.sleep(2)` while downloads continue in the background.
  double pumpFor(double seconds, const bool* stopFlag);

  [[nodiscard]] std::size_t activeRequests() const { return transfers_.size(); }
  [[nodiscard]] std::size_t totalRequests() const { return totalStarted_; }
  [[nodiscard]] std::size_t totalBytes() const { return totalBytes_; }
  [[nodiscard]] std::size_t totalFailures() const { return totalFailures_; }
  [[nodiscard]] std::size_t queuedChunkBytes() const;

  static bool isValidUrl(const char* url);

 private:
  struct Transfer {
    CURL* easy = nullptr;
    std::uint64_t id = 0;
    std::deque<std::string> chunks;
    std::string body;
    std::string error;
    long httpCode = 0;
    bool done = false;
    bool failed = false;
    bool truncated = false;
    std::size_t received = 0;
    std::size_t chunkBytes = 0;
  };

  [[nodiscard]] Transfer* find(std::uint64_t handle);
  void reap();
  void finish(Transfer* t, CURL* easy, CURLcode result, long code);
  static std::size_t writeSink(char* data, std::size_t size, std::size_t nmemb, void* ud);

  CURLM* multi_ = nullptr;
  bool globalsReady_ = false;
  std::unordered_map<std::uint64_t, Transfer> transfers_;
  std::uint64_t nextHandle_ = 1;
  std::size_t totalStarted_ = 0;
  std::size_t totalBytes_ = 0;
  std::size_t totalFailures_ = 0;
};

}  // namespace ocemu
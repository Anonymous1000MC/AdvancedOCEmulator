#include "ocemu/InternetCard.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace ocemu {
namespace {

void setErr(char* buf, std::size_t cap, const char* msg) {
  if (buf != nullptr && cap > 0) {
    std::snprintf(buf, cap, "%s", msg != nullptr ? msg : "unknown error");
  }
}

}  // namespace

const char* internetStatusName(InternetStatus s) {
  switch (s) {
    case InternetStatus::Pending:      return "pending";
    case InternetStatus::Complete:     return "complete";
    case InternetStatus::Failed:       return "failed";
    case InternetStatus::InvalidHandle: return "invalid";
  }
  return "unknown";
}

InternetCard::~InternetCard() { shutdown(); }

bool InternetCard::init(std::string* error) {
  if (multi_ != nullptr) return true;

  if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
    if (error) *error = "curl_global_init() failed";
    return false;
  }
  globalsReady_ = true;

  multi_ = curl_multi_init();
  if (multi_ == nullptr) {
    curl_global_cleanup();
    globalsReady_ = false;
    if (error) *error = "curl_multi_init() failed";
    return false;
  }

  curl_multi_setopt(multi_, CURLMOPT_MAX_TOTAL_CONNECTIONS, 8L);
  curl_multi_setopt(multi_, CURLMOPT_MAXCONNECTS, 8L);
#ifdef CURLPIPE_MULTIPLEX
  curl_multi_setopt(multi_, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
#endif
  return true;
}

void InternetCard::shutdown() {
  if (multi_ != nullptr) {
    for (auto& kv : transfers_) {
      if (kv.second.easy != nullptr) {
        curl_multi_remove_handle(multi_, kv.second.easy);
        curl_easy_cleanup(kv.second.easy);
        kv.second.easy = nullptr;
      }
    }
    transfers_.clear();
    curl_multi_cleanup(multi_);
    multi_ = nullptr;
  }
  if (globalsReady_) {
    curl_global_cleanup();
    globalsReady_ = false;
  }
}

bool InternetCard::isValidUrl(const char* url) {
  if (url == nullptr) return false;

  static const char* kSchemes[] = {"http://", "https://"};
  bool schemeOk = false;
  for (const char* s : kSchemes) {
    const std::size_t n = std::strlen(s);
    if (std::strncmp(url, s, n) == 0) {
      schemeOk = true;
      break;
    }
  }
  if (!schemeOk) return false;

  // Require at least one host character after the scheme.
  const char* host = std::strstr(url, "://");
  if (host == nullptr) return false;
  host += 3;
  if (*host == '\0') return false;

  // Reject an embedded NUL-ish / whitespace host, which is almost always a
  // malformed URL rather than something worth handing to libcurl.
  for (const char* p = host; *p != '\0'; ++p) {
    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') return false;
  }
  return true;
}

InternetCard::Transfer* InternetCard::find(std::uint64_t handle) {
  const auto it = transfers_.find(handle);
  return it == transfers_.end() ? nullptr : &it->second;
}

std::size_t InternetCard::writeSink(char* data, std::size_t size, std::size_t nmemb, void* ud) {
  auto* t = static_cast<Transfer*>(ud);
  if (t == nullptr) return 0;

  const std::size_t n = size * nmemb;
  if (n == 0) return 0;

  // Over budget: keep consuming so the transfer still terminates cleanly, but
  // stop growing the buffer. The guest is told the body was truncated.
  if (t->received >= kMaxBodyBytes) {
    t->truncated = true;
    return n;
  }

  const std::size_t room = kMaxBodyBytes - t->received;
  const std::size_t take = std::min(n, room);
  if (take < n) t->truncated = true;

  try {
    // Chunked so Lua can consume the response incrementally rather than
    // waiting for the whole body.
    std::string chunk;
    chunk.assign(data, take);
    t->body.append(data, take);
    t->chunks.push_back(std::move(chunk));
    if (t->chunks.size() > kMaxChunksQueued) t->chunks.pop_front();
  } catch (...) {
    // Out of host memory: abort the transfer rather than corrupt state.
    return 0;
  }

  t->received += take;
  t->chunkBytes += take;

  // Must report the full byte count or libcurl treats the write as an error.
  return n;
}

std::uint64_t InternetCard::request(const char* url, char* errBuf, std::size_t errCap) {
  setErr(errBuf, errCap, "");

  if (multi_ == nullptr) {
    setErr(errBuf, errCap, "internet component is not initialised");
    return 0;
  }
  if (!isValidUrl(url)) {
    setErr(errBuf, errCap, "malformed URL (expected http:// or https://)");
    return 0;
  }

  CURL* easy = curl_easy_init();
  if (easy == nullptr) {
    setErr(errBuf, errCap, "curl_easy_init() failed");
    return 0;
  }

  const std::uint64_t id = nextHandle_++;
  try {
    transfers_.emplace(id, Transfer{});
  } catch (...) {
    curl_easy_cleanup(easy);
    setErr(errBuf, errCap, "host out of memory while queueing the request");
    return 0;
  }

  // unordered_map is node-based, so this pointer stays valid across inserts and
  // is exactly what CURLOPT_PRIVATE will hand us back in reap().
  Transfer* slot = &transfers_.find(id)->second;
  slot->id = id;
  slot->easy = easy;

  curl_easy_setopt(easy, CURLOPT_URL, url);
  curl_easy_setopt(easy, CURLOPT_PRIVATE, reinterpret_cast<char*>(slot));
  curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &InternetCard::writeSink);
  curl_easy_setopt(easy, CURLOPT_WRITEDATA, reinterpret_cast<char*>(slot));
  curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(easy, CURLOPT_TIMEOUT_MS, static_cast<long>(kTimeoutMs));
  curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(kConnectTimeoutMs));
  curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");  // transparent gzip/deflate
  curl_easy_setopt(easy, CURLOPT_USERAGENT, "ocemu/1.0 (OpenComputers emulator)");
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);

  const CURLMcode rc = curl_multi_add_handle(multi_, easy);
  if (rc != CURLM_OK) {
    transfers_.erase(id);
    curl_easy_cleanup(easy);
    setErr(errBuf, errCap, curl_multi_strerror(rc));
    return 0;
  }

  ++totalStarted_;
  return id;
}

void InternetCard::finish(Transfer* t, CURL* easy, CURLcode result, long code) {
  if (t == nullptr) return;

  if (easy != nullptr) curl_multi_remove_handle(multi_, easy);

  t->done = true;
  t->httpCode = code;

  if (result != CURLE_OK) {
    t->failed = true;
    const char* msg = curl_easy_strerror(result);
    try {
      t->error = msg != nullptr ? msg : "transfer failed";
      if (code > 0) t->error += " (HTTP " + std::to_string(code) + ")";
    } catch (...) {
      t->error.clear();
    }
    ++totalFailures_;
  } else if (code >= 400) {
    t->failed = true;
    try {
      t->error = "HTTP " + std::to_string(code);
    } catch (...) {
      t->error.clear();
    }
    ++totalFailures_;
  }
}

void InternetCard::reap() {
  int msgsLeft = 0;
  CURLMsg* msg = nullptr;
  while ((msg = curl_multi_info_read(multi_, &msgsLeft)) != nullptr) {
    if (msg->msg != CURLMSG_DONE) continue;

    CURL* easy = msg->easy_handle;
    char* priv = nullptr;
    curl_easy_getinfo(easy, CURLINFO_PRIVATE, &priv);

    long code = 0;
    curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &code);

    finish(reinterpret_cast<Transfer*>(priv), easy, msg->data.result, code);
  }
}

void InternetCard::pump() {
  if (multi_ == nullptr) return;

  int running = 0;
  const CURLMcode mc = curl_multi_perform(multi_, &running);
  if (mc != CURLM_OK) return;

  reap();
}

double InternetCard::pumpFor(double seconds, const bool* stopFlag) {
  if (multi_ == nullptr || seconds <= 0.0) return 0.0;

  const auto start = std::chrono::steady_clock::now();

  for (;;) {
    pump();

    if (stopFlag != nullptr && *stopFlag) break;
    if (transfers_.empty()) break;  // nothing left in flight

    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - start).count();
    if (elapsed >= seconds) break;

    // Sleep in short slices: responsive to cancellation, but never a busy loop.
    const double slice = std::min(seconds - elapsed, 0.05);
    const int ms = static_cast<int>(slice * 1000.0);
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  }

  const auto end = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(end - start).count();
}

void InternetCard::poll(std::uint64_t handle, InternetPoll* out) {
  if (out == nullptr) return;
  *out = InternetPoll{};

  Transfer* t = find(handle);
  if (t == nullptr) {
    out->status = InternetStatus::InvalidHandle;
    std::snprintf(out->error, sizeof(out->error), "unknown request handle %llu",
                  static_cast<unsigned long long>(handle));
    return;
  }

  // Let libcurl make progress even if the guest polls in a tight loop.
  pump();

  if (!t->chunks.empty()) {
    std::string& front = t->chunks.front();
    const std::size_t take = std::min(front.size(), sizeof(out->chunk) - 1);
    if (take > 0) {
      std::memcpy(out->chunk, front.data(), take);
      out->chunk[take] = '\0';
      out->chunkLen = take;
      out->hasChunk = true;

      front.erase(0, take);
      if (front.empty()) t->chunks.pop_front();
      t->chunkBytes -= std::min(take, t->chunkBytes);
    }
  }

  out->httpCode = t->httpCode;

  if (t->failed) {
    out->status = InternetStatus::Failed;
    std::snprintf(out->error, sizeof(out->error), "%s",
                  t->error.empty() ? "transfer failed" : t->error.c_str());
    return;
  }

  // Only report completion once every queued chunk has been handed to the
  // guest, so a plain `while status == "pending"` loop cannot truncate a body.
  if (t->done && t->chunks.empty()) {
    out->status = InternetStatus::Complete;
    if (t->truncated) {
      std::snprintf(out->error, sizeof(out->error), "response truncated at %zu bytes",
                    kMaxBodyBytes);
    }
    return;
  }

  out->status = InternetStatus::Pending;
}

std::size_t InternetCard::copyResponse(std::uint64_t handle, char* dst, std::size_t cap,
                                       std::size_t* totalLen) {
  if (totalLen != nullptr) *totalLen = 0;

  Transfer* t = find(handle);
  if (t == nullptr) return 0;

  if (totalLen != nullptr) *totalLen = t->body.size();
  if (dst == nullptr || cap == 0) return 0;

  const std::size_t take = std::min(cap, t->body.size());
  std::memcpy(dst, t->body.data(), take);
  return take;
}

void InternetCard::close(std::uint64_t handle) {
  const auto it = transfers_.find(handle);
  if (it == transfers_.end()) return;

  Transfer& t = it->second;
  if (t.easy != nullptr) {
    if (multi_ != nullptr) curl_multi_remove_handle(multi_, t.easy);
    curl_easy_cleanup(t.easy);
    t.easy = nullptr;
  }
  transfers_.erase(it);
}

std::size_t InternetCard::queuedChunkBytes() const {
  std::size_t total = 0;
  for (const auto& kv : transfers_) total += kv.second.chunkBytes;
  return total;
}

}  // namespace ocemu
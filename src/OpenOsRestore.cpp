#include "ocemu/OpenOsRestore.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

extern "C" {
#include <curl/curl.h>
}

namespace ocemu {
namespace {

constexpr const char* kDefaultUrl =
    "https://raw.githubusercontent.com/Anonymous1000MC/AdvancedOCEmulator/"
    "main/restore/bootloader.lua";

size_t sink(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

int progressSink(void* clientp, curl_off_t dlTotal, curl_off_t dlNow, curl_off_t, curl_off_t) {
  auto* p = static_cast<OpenOsRestore*>(clientp);
  if (dlTotal > 0) {
    const double frac = static_cast<double>(dlNow) / static_cast<double>(dlTotal);
    p->setProgress(static_cast<float>(frac));
  }
  return 0;
}

}  // namespace

const char* defaultOpenOsUrl() {
  const char* env = std::getenv("OCEMU_OPENOS_URL");
  return (env != nullptr && *env != '\0') ? env : kDefaultUrl;
}

OpenOsRestore::~OpenOsRestore() { join(); }

void OpenOsRestore::start(const std::string& url) {
  if (running_) return;
  join();
  running_ = true;
  succeeded_ = false;
  progress_ = -1.0f;
  error_.clear();
  payload_.clear();
  status_ = "contacting GitHub...";
  worker_ = std::thread(&OpenOsRestore::run, this, url);
}

void OpenOsRestore::join() {
  if (worker_.joinable()) worker_.join();
}

std::string OpenOsRestore::takePayload() {
  std::string out;
  out.swap(payload_);
  return out;
}

void OpenOsRestore::run(std::string url) {
  // The UI reads status_/error_ while this runs, so keep writes on this thread
  // and reads short; they are only ever short strings.
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    error_ = "libcurl could not be initialised";
    running_ = false;
    return;
  }

  std::string body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "AdvancedOCEmulator/1.0");
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &sink);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &progressSink);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, nullptr);

  status_ = "downloading bootloader...";
  const CURLcode res = curl_easy_perform(curl);
  long code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    error_ = std::string("download failed: ") + curl_easy_strerror(res);
    running_ = false;
    return;
  }
  if (code != 200) {
    error_ = "unexpected HTTP status " + std::to_string(code);
    running_ = false;
    return;
  }
  if (body.empty()) {
    error_ = "downloaded payload was empty";
    running_ = false;
    return;
  }
  // A bootloader that cannot even parse is worse than none; check before we
  // overwrite a working EEPROM with it.
  if (body.find("while true do") == std::string::npos) {
    error_ = "payload does not look like an OpenOS bootloader";
    running_ = false;
    return;
  }

  payload_ = std::move(body);
  progress_ = 1.0f;
  status_ = "restored";
  succeeded_ = true;
  running_ = false;
}

std::string findEepromCodePath(const std::string& machineDataDir) {
  namespace fs = std::filesystem;
  const fs::path cfg = fs::path(machineDataDir) / "ocemu.cfg";
  std::ifstream in(cfg, std::ios::binary);
  if (!in) return {};

  // Pull the address out of the eeprom entry in the components list, e.g.
  //   {"eeprom", "52f69764-fa8b-4158-830e-aec2d1bcb67c", 9, "lua/bios.lua"},
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  const std::size_t at = text.find("\"eeprom\"");
  if (at == std::string::npos) return {};
  const std::size_t open = text.find('"', text.find(',', at));
  if (open == std::string::npos) return {};
  const std::size_t close = text.find('"', open + 1);
  if (close == std::string::npos) return {};
  const std::string address = text.substr(open + 1, close - open - 1);

  const fs::path code = fs::path(machineDataDir) / address / "code.lua";
  std::error_code ec;
  if (!fs::is_directory(code.parent_path(), ec)) return {};
  return code.string();
}

}  // namespace ocemu

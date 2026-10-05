#include "ocemu/Config.hpp"

#include <fstream>
#include <system_error>
#include <utility>

namespace ocemu {
namespace {

// Schema version so a future field migration has something to key off.
constexpr int kConfigVersion = 1;

}  // namespace

ConfigStore::ConfigStore(std::filesystem::path path) : path_(std::move(path)) {}

nlohmann::json ConfigStore::toJson(const EmulatorConfig& cfg) {
  nlohmann::json j;
  j["version"] = kConfigVersion;
  j["gpu_tier"] = cfg.gpuTier;
  j["screen_tier"] = cfg.screenTier;
  j["internet_card"] = cfg.internetCard;
  j["infinite_memory"] = cfg.infiniteMemory;
  // The sentinel the emulator core expects: -1 means "no cap".
  j["ram_size_kb"] = cfg.infiniteMemory ? -1 : cfg.ramSizeKb;
  return j;
}

EmulatorConfig ConfigStore::fromJson(const nlohmann::json& j) {
  EmulatorConfig c;
  if (!j.is_object()) return c;

  c.gpuTier = j.value("gpu_tier", c.gpuTier);
  c.screenTier = j.value("screen_tier", c.screenTier);
  c.internetCard = j.value("internet_card", c.internetCard);
  c.infiniteMemory = j.value("infinite_memory", c.infiniteMemory);
  c.ramSizeKb = j.value("ram_size_kb", c.ramSizeKb);

  // A config that only carries the sentinel is still a valid "infinite" state,
  // so read ram_size_kb before sanitising (sanitise would clamp -1 to kMinRamKb).
  if (j.contains("ram_size_kb") && j["ram_size_kb"].is_number_integer()) {
    const int raw = j["ram_size_kb"].get<int>();
    if (raw < 0) {
      c.infiniteMemory = true;
      c.ramSizeKb = EmulatorConfig::kMinRamKb;
    }
  }

  c.sanitize();
  return c;
}

bool ConfigStore::load() {
  lastError_.clear();

  std::error_code ec;
  if (!std::filesystem::exists(path_, ec)) {
    // Fresh workspace: defaults are perfectly valid, there is just no file yet.
    cfg_ = EmulatorConfig{};
    onDisk_ = cfg_;
    haveDiskCopy_ = true;
    dirty_ = false;
    return false;
  }

  std::ifstream in(path_);
  if (!in) {
    lastError_ = "cannot open " + path_.string();
    return false;
  }

  nlohmann::json j;
  try {
    in >> j;
  } catch (const std::exception& e) {
    lastError_ = std::string("malformed config.json: ") + e.what();
    cfg_ = EmulatorConfig{};
    onDisk_ = cfg_;
    haveDiskCopy_ = true;
    dirty_ = false;
    return false;
  }

  cfg_ = fromJson(j);
  onDisk_ = cfg_;
  haveDiskCopy_ = true;
  dirty_ = false;
  return true;
}

bool ConfigStore::save() {
  lastError_.clear();

  std::filesystem::path tmp = path_;
  tmp += ".tmp";

  {
    std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
    if (!out) {
      lastError_ = "cannot write " + tmp.string();
      return false;
    }
    try {
      out << toJson(cfg_).dump(2) << '\n';
    } catch (const std::exception& e) {
      lastError_ = std::string("serialisation failed: ") + e.what();
      out.close();
      std::error_code ignored;
      std::filesystem::remove(tmp, ignored);
      return false;
    }
    if (!out) {
      lastError_ = "short write to " + tmp.string();
      out.close();
      std::error_code ignored;
      std::filesystem::remove(tmp, ignored);
      return false;
    }
  }

  std::error_code ec;
  std::filesystem::rename(tmp, path_, ec);
  if (ec) {
    // Cross-device or permission problem: fall back to a direct overwrite.
    std::error_code ignored;
    std::filesystem::copy_file(tmp, path_, std::filesystem::copy_options::overwrite_existing,
                               ignored);
    std::filesystem::remove(tmp, ignored);
    if (ignored) {
      lastError_ = "cannot replace " + path_.string() + ": " + ignored.message();
      return false;
    }
  }

  onDisk_ = cfg_;
  haveDiskCopy_ = true;
  dirty_ = false;
  return true;
}

void ConfigStore::resetToDefaults() {
  cfg_ = EmulatorConfig{};
  dirty_ = true;
}

bool ConfigStore::differsFromDisk() const {
  if (!haveDiskCopy_) return true;
  return cfg_.gpuTier != onDisk_.gpuTier || cfg_.screenTier != onDisk_.screenTier ||
         cfg_.internetCard != onDisk_.internetCard || cfg_.ramSizeKb != onDisk_.ramSizeKb ||
         cfg_.infiniteMemory != onDisk_.infiniteMemory;
}

void ConfigStore::markSaved() {
  onDisk_ = cfg_;
  haveDiskCopy_ = true;
  dirty_ = false;
}

}  // namespace ocemu
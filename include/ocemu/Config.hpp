// ---------------------------------------------------------------------------
//  Persistent emulator configuration (config.json).
//
//  This is the single source of truth that the "Component Manager" ImGui panel
//  reads from and writes to. The Lua core never touches it directly: the panel
//  mutates the struct, and App pushes the result into the running emulator when
//  the user presses "Reboot / Apply Settings".
// ---------------------------------------------------------------------------
#pragma once

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "ocemu/Tiers.hpp"

namespace ocemu {

struct EmulatorConfig {
  int gpuTier = 2;                    // 1..3
  int screenTier = 2;                 // 1..3
  bool internetCard = false;          // bind component.internet when true
  int ramSizeKb = 512;                // slider range; ignored when infinite
  bool infiniteMemory = false;        // true -> ram_size_kb == -1

  static constexpr int kMinRamKb = 64;
  static constexpr int kMaxRamKb = 8192;

  // -1 is the sentinel the Lua core understands as "no cap".
  [[nodiscard]] int effectiveRamKb() const { return infiniteMemory ? -1 : ramSizeKb; }

  [[nodiscard]] TierSpec displayLimits() const {
    return resolveDisplayLimits(gpuTier, screenTier);
  }

  // Clamp every field into its legal range. Called after any external edit.
  void sanitize() {
    gpuTier = clampTier(gpuTier);
    screenTier = clampTier(screenTier);
    ramSizeKb = std::clamp(ramSizeKb, kMinRamKb, kMaxRamKb);
  }
};

class ConfigStore {
 public:
  explicit ConfigStore(std::filesystem::path path);

  // Reads config.json. A missing or malformed file is not an error: we fall
  // back to defaults and report it through `lastError()` for the UI to show.
  bool load();

  // Writes config.json atomically (temp file + rename) so a crash mid-write
  // cannot leave a truncated config behind.
  bool save();

  void resetToDefaults();

  [[nodiscard]] EmulatorConfig& values() { return cfg_; }
  [[nodiscard]] const EmulatorConfig& values() const { return cfg_; }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }
  [[nodiscard]] const std::string& lastError() const { return lastError_; }
  [[nodiscard]] bool dirty() const { return dirty_; }

  // True when the current values differ from what is on disk.
  [[nodiscard]] bool differsFromDisk() const;

  void markSaved();

  static nlohmann::json toJson(const EmulatorConfig& cfg);
  static EmulatorConfig fromJson(const nlohmann::json& j);

 private:
  std::filesystem::path path_;
  EmulatorConfig cfg_;
  EmulatorConfig onDisk_;
  bool dirty_ = false;
  bool haveDiskCopy_ = false;
  std::string lastError_;
};

}  // namespace ocemu
#include "ocemu/ui/ComponentManagerPanel.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "ocemu/Config.hpp"
#include "ocemu/GlyphAtlas.hpp"
#include "ocemu/InternetCard.hpp"
#include "ocemu/LuaMachine.hpp"
#include "ocemu/MemoryAllocator.hpp"
#include "ocemu/ScreenBuffer.hpp"

namespace ocemu {
namespace {

constexpr const char* kTierNames[] = {"Tier 1", "Tier 2", "Tier 3"};

void sectionTitle(const char* text) {
  ImGui::Spacing();
  ImGui::TextUnformatted(text);
  ImGui::Separator();
}

void formatKb(std::size_t bytes, char* buf, std::size_t cap) {
  if (bytes >= 1024u * 1024u) {
    std::snprintf(buf, cap, "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024u) {
    std::snprintf(buf, cap, "%.1f KB", static_cast<double>(bytes) / 1024.0);
  } else {
    std::snprintf(buf, cap, "%zu B", bytes);
  }
}

const char* machineStateName(MachineState s) {
  switch (s) {
    case MachineState::Running:     return "running";
    case MachineState::OutOfMemory: return "out of memory";
    case MachineState::Faulted:     return "faulted";
    case MachineState::Stopped:     break;
  }
  return "stopped";
}

ImVec4 machineStateColor(MachineState s) {
  switch (s) {
    case MachineState::Running:     return ImVec4(0.45f, 0.85f, 0.45f, 1.0f);
    case MachineState::OutOfMemory: return ImVec4(0.95f, 0.45f, 0.35f, 1.0f);
    case MachineState::Faulted:     return ImVec4(0.95f, 0.65f, 0.25f, 1.0f);
    case MachineState::Stopped:     break;
  }
  return ImVec4(0.70f, 0.70f, 0.70f, 1.0f);
}

}  // namespace

void drawComponentManager(PanelContext& ctx) {
  if (ctx.config == nullptr) return;

  ConfigStore& store = *ctx.config;
  EmulatorConfig& cfg = store.values();

  // ---------------------------------------------------------------- state ---
  const MachineState state = (ctx.lua != nullptr) ? ctx.lua->state() : MachineState::Stopped;
  ImGui::TextColored(machineStateColor(state), "Lua state: %s", machineStateName(state));
  if (state == MachineState::OutOfMemory && ctx.allocator != nullptr) {
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "  refused %zu request(s)",
                       ctx.allocator->refusedCount());
  }
  if (ctx.lua != nullptr && !ctx.lua->lastError().empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.35f, 1.0f));
    ImGui::TextWrapped("  %s", ctx.lua->lastError().c_str());
    ImGui::PopStyleColor();
  }

  // -------------------------------------------------------------- display ---
  sectionTitle("Display");

  int gpuIdx = cfg.gpuTier - 1;
  if (ImGui::Combo("GPU Tier", &gpuIdx, kTierNames, IM_ARRAYSIZE(kTierNames))) {
    cfg.gpuTier = gpuIdx + 1;
  }

  int screenIdx = cfg.screenTier - 1;
  if (ImGui::Combo("Screen Tier", &screenIdx, kTierNames, IM_ARRAYSIZE(kTierNames))) {
    cfg.screenTier = screenIdx + 1;
  }

  // The intersection of the two tiers is what the machine actually gets.
  const TierSpec effective = cfg.displayLimits();
  ImGui::TextDisabled("Granted: %d x %d cells, %d-bit colour", effective.cols, effective.rows,
                      effective.depth);

  if (ctx.screen != nullptr && ctx.screen->valid()) {
    ImGui::TextDisabled("Buffer : %d x %d cells, %zu cells", ctx.screen->cols(),
                        ctx.screen->rows(), ctx.screen->cellCount());
    if (ctx.screen->requestWasClamped()) {
      ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "Guest asked %d x %d (clamped)",
                         ctx.screen->requestedCols(), ctx.screen->requestedRows());
    }
  }

  // --------------------------------------------------------------- memory ---
  sectionTitle("Memory");

  ImGui::Checkbox("Infinite Memory", &cfg.infiniteMemory);

  // With infinite memory the size control is meaningless, so disable it and let
  // the core see the -1 sentinel instead.
  ImGui::BeginDisabled(cfg.infiniteMemory);
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::SliderInt("##ramSlider", &cfg.ramSizeKb, EmulatorConfig::kMinRamKb,
                       EmulatorConfig::kMaxRamKb, "%d KB")) {
    cfg.ramSizeKb = std::clamp(cfg.ramSizeKb, EmulatorConfig::kMinRamKb,
                               EmulatorConfig::kMaxRamKb);
  }
  ImGui::SameLine(0.0f, 6.0f);
  ImGui::SetNextItemWidth(96.0f);
  ImGui::InputInt("##ramInput", &cfg.ramSizeKb, 64, 512);
  ImGui::EndDisabled();

  const int effectiveRamKb = cfg.effectiveRamKb();
  if (effectiveRamKb < 0) {
    ImGui::TextDisabled("Limit  : unlimited (-1)");
  } else {
    ImGui::TextDisabled("Limit  : %d KB", effectiveRamKb);
  }

  if (ctx.allocator != nullptr) {
    const MemoryAllocator& alloc = *ctx.allocator;
    char usedBuf[48];
    formatKb(alloc.usedBytes(), usedBuf, sizeof(usedBuf));

    if (alloc.infinite()) {
      ImGui::Text("Used    : %s (uncapped)", usedBuf);
    } else {
      char limitBuf[48];
      formatKb(alloc.limitBytes(), limitBuf, sizeof(limitBuf));

      char overlay[112];
      std::snprintf(overlay, sizeof(overlay), "%s / %s", usedBuf, limitBuf);

      const float frac = static_cast<float>(std::clamp(alloc.usedFraction(), 0.0, 1.0));
      ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), overlay);

      char peakBuf[48];
      formatKb(alloc.peakBytes(), peakBuf, sizeof(peakBuf));
      ImGui::TextDisabled("Peak    : %s", peakBuf);
    }

    if (alloc.refusedCount() > 0) {
      char refusedBuf[48];
      formatKb(alloc.refusedBytes(), refusedBuf, sizeof(refusedBuf));
      ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "Blocked : %s over %zu request(s)",
                         refusedBuf, alloc.refusedCount());
    }
  }

  // ----------------------------------------------------------- components ---
  sectionTitle("Components");

  ImGui::Checkbox("Enable Internet Card", &cfg.internetCard);
  ImGui::SameLine();
  if (!cfg.internetCard) {
    ImGui::TextDisabled("(component.internet not bound)");
  } else if (ctx.internet != nullptr && !ctx.internet->ready()) {
    ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f), "libcurl unavailable");
  }

  if (ctx.internet != nullptr) {
    char bytesBuf[48];
    formatKb(ctx.internet->totalBytes(), bytesBuf, sizeof(bytesBuf));
    ImGui::TextDisabled("Net    : %zu active, %zu total, %s received", ctx.internet->activeRequests(),
                        ctx.internet->totalRequests(), bytesBuf);
    if (ctx.internet->totalFailures() > 0) {
      ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f), "         %zu failed",
                         ctx.internet->totalFailures());
    }
  }

  // -------------------------------------------------------------- actions ---
  sectionTitle("Actions");

  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.26f, 0.70f, 0.40f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.45f, 0.25f, 1.0f));
  if (ImGui::Button("Reboot / Apply Settings", ImVec2(-1.0f, 0.0f)) &&
      ctx.rebootRequested != nullptr) {
    *ctx.rebootRequested = true;
  }
  ImGui::PopStyleColor(3);

  ImGui::Spacing();
  if (ImGui::Button("Save config.json", ImVec2(-1.0f, 0.0f)) && ctx.saveRequested != nullptr) {
    *ctx.saveRequested = true;
  }

  if (store.differsFromDisk()) {
    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.35f, 1.0f), "Unsaved changes on disk.");
  }
  if (!store.lastError().empty()) {
    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1.0f), "%s", store.lastError().c_str());
  }

  ImGui::Spacing();
  ImGui::TextDisabled("%s", store.path().filename().string().c_str());
}

}  // namespace ocemu
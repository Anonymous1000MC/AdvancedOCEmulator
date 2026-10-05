// ---------------------------------------------------------------------------
//  The "Component Manager" ImGui panel.
//
//  Reads and writes the live EmulatorConfig, presents the tier selectors, the
//  internet-card toggle and the RAM slider (with its "Infinite Memory"
//  companion), and exposes the reboot/apply action that soft-reboots the Lua
//  state with the new limits.
// ---------------------------------------------------------------------------
#pragma once

namespace ocemu {

class ConfigStore;
class LuaMachine;
class MemoryAllocator;
class ScreenBuffer;
class InternetCard;
struct HostInfo;

// Everything the panel needs to read or poke.
struct PanelContext {
  ConfigStore* config = nullptr;
  LuaMachine* lua = nullptr;
  MemoryAllocator* allocator = nullptr;
  ScreenBuffer* screen = nullptr;
  InternetCard* internet = nullptr;
  const HostInfo* hostInfo = nullptr;

  // --- outputs ---
  bool* rebootRequested = nullptr;  // set when the user applies settings
  bool* saveRequested = nullptr;    // set when the user saves config.json
  bool* showPanel = nullptr;        // panel visibility toggle (title-bar X)
};

// Draws the panel. Call between ImGui::Begin/ImGui::End.
void drawComponentManager(PanelContext& ctx);

}  // namespace ocemu
// ---------------------------------------------------------------------------
//  OcEmuHost -- runs OCEmu's Lua emulation core inside our SDL2/ImGui host.
//
//  WHY THIS EXISTS
//  ---------------
//  OCEmu (zenith391) is a complete, working OpenComputers emulator written in
//  Lua, but its front end is plain SDL2 driven through luaffifb. Its emulation
//  layer is good; its UI is the weak part, and it is awkward to customise.
//
//  Fortunately the architecture has one clean seam. Everything host-side lives
//  in a single table called `elsa`:
//
//      elsa.filesystem.*   elsa.timer.getTime   elsa.system.getOS
//      elsa.getError()     elsa.quit()          elsa.draw()  elsa.update()
//      elsa.args / elsa.opts / elsa.handlers   elsa.SDL  (optional!)
//
//  We supply that table from C++ instead. OCEmu's machine.lua, apis/*.lua and
//  component/*.lua then run unmodified inside our embedded Lua 5.2, and only
//  the two genuinely-SDL components (screen, keyboard) are replaced.
//
//  THE elsa.SDL TRICK
//  ------------------
//  main.lua guards all of its SDL audio behind `if not machine.beep and elsa.SDL`
//  and only defines machine.sleep from SDL when elsa.SDL exists. By leaving
//  elsa.SDL nil we drop luaffifb out of the dependency graph entirely, rather
//  than stubbing dozens of SDL entry points. We predefine machine.beep and
//  machine.sleep ourselves so those fallbacks never run.
//
//  THE COMPONENT INJECTION POINT
//  ----------------------------
//  apis/component.lua does, for each entry of settings.components:
//
//      local fn = elsa.filesystem.load("component/" .. info[1] .. ".lua")
//      local proxy, cec, mai, di = fn(table.unpack(info, 2, info.n))
//
//  Because elsa.filesystem.load is ours, a spec naming a native component
//  ("screen_native", "keyboard_native") gets a C closure handed back instead of
//  a file on disk. The (proxy, cec, mai, di) contract is identical, so
//  apis/component.lua needs no changes at all.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <string>

#include "ocemu/LuaMachine.hpp"

struct lua_State;

namespace ocemu {

// OC palette tiers, matching the depth table in gpu.lua/screen_sdl2.lua:
// depth 1 = OneBit, 4 = FourBit, 8 = EightBit.
inline constexpr int kOcTierDepth[4] = {0, 1, 4, 8};

// One emulated machine's identity, mirroring OCEmu's settings.components.
struct ComponentSpec {
  std::string type;      // "gpu", "eeprom", "filesystem", "screen_native", ...
  std::string address;   // empty means "derive from slot"
  int slot = -1;
  std::vector<std::string> args;  // trailing component-specific arguments
};

class OcEmuHost {
 public:
  OcEmuHost() = default;
  ~OcEmuHost();
  OcEmuHost(const OcEmuHost&) = delete;
  OcEmuHost& operator=(const OcEmuHost&) = delete;

  // Installs the elsa table into an existing LuaMachine. Must be called after
  // the machine reboots (so a state exists) and before main.lua runs.
  // `ocsrcDir` is OCEmu's src/ directory; `machineDir` is the emulated machine
  // filesystem (the directory that holds init.lua, boot/, lib/, ...).
  void install(lua_State* L, std::string ocsrcDir, std::string machineDir);

  // Pushes the configured RAM into the guest's `machine.totalMemory`, which
  // computer.totalMemory() reports. main.lua hardcodes it to 2 MiB, so without
  // this the guest always believed it had 2048K no matter what was configured.
  // Pass ramKb <= 0 for "no cap configured".
  void applyRamSpec(int ramKb);

  // Needed so host-driven dispatches can run inside the VM's panic guard.
  void setMachine(LuaMachine* m) { machine_ = m; }
  // Diagnostics needs to ask the guest what it can actually see, so the host
  // and guest component lists can be diffed against each other.
  [[nodiscard]] LuaMachine* machine() const { return machine_; }

  // --- component registration --------------------------------------------
  // Marker: component type names that this host provides natively instead of
  // loading from OCEmu's src/component/<name>.lua.
  static constexpr const char* kNativeScreen = "screen_native";
  static constexpr const char* kNativeKeyboard = "keyboard_native";
  [[nodiscard]] static bool isNativeComponent(const std::string& type);

  // Builds the component list for settings.components from our tiers.
  [[nodiscard]] static std::vector<ComponentSpec> buildComponentList(int tier, bool internetCard);

  // Rewrites the `components { ... }` block of OCEmu's ocemu.cfg with our list.
  //
  // This has to go through OCEmu's own config file rather than being injected
  // in memory: apis/component.lua reads settings.components at load time, and
  // settings.lua populates it from config.get("emulator.components"), which
  // main.lua refreshes with config.load(). Everything else in the file is left
  // untouched.
  // `dataDir` is OCEmu's machine-data root (where ocemu.cfg lives); it is passed
  // explicitly because this runs before install() has a chance to record it.
  bool syncOcEmuConfig(const std::string& dataDir, int tier, bool internetCard,
                       std::string* error = nullptr);

  // Serialises a component list in OCEmu's literal syntax.
  [[nodiscard]] std::string renderComponentList(const std::vector<ComponentSpec>& list) const;

  // Serialises the component list to the Lua table form main.lua expects:
  //   { {"gpu", nil, 0, 160, 50, 3}, ... }
  void pushComponentList(lua_State* L) const;

  // --- host callbacks (called from our frame loop) ------------------------
  void update(double dtSeconds);

  // Diagnostics: how many host ticks the machine has received, and how much
  // virtual time that amounts to.
  unsigned long long tickCount() const { return tickCount_; }
  double virtualTime() const { return virtualTime_; }

  // "<state>=<n> lastError=<msg>" for the boot diagnostics.
  std::string machineStatus() const;
  void draw();

  // Queues a real key press for the guest. `name` is a GLFW key name ("a",
  // "enter", "F1", ...). Returns false if the key was not queued.
  bool pushKey(const std::string& name);

  // How many OCEmu update ticks to run per host frame.
  //
  // main.lua gives the guest `maxCallBudget = 1.5` call units PER TICK and
  // resets it in elsa.update. One tick per rendered frame starves a busy guest:
  // cost-accounted component methods (filesystem.exists among them) then hit
  // `if not machine.consumeCallBudget(...) then return end` and silently return
  // nothing, so OpenOS's package.lua concludes a module does not exist. Running
  // several ticks per frame hands the guest proportionally more compute per
  // real second, which is what a dedicated emulator thread would give it.
  void setTickMultiplier(int n);

  // Replacement for machine.consumeCallBudget; always grants the call.
  static int luaConsumeCallBudget(lua_State* L);
  [[nodiscard]] int tickMultiplier() const { return tickMultiplier_; }

  // The Lua state to drive, or nullptr when the machine is stopped or faulted.
  [[nodiscard]] lua_State* liveState() const;

  // Tracks modifier/held-key state so the guest's isControlDown() and friends
  // answer correctly. `name` uses the same GLFW naming as pushKey.
  void setKeyState(const std::string& name, bool down);
  void setModifier(const std::string& which, bool down);

  // OC input events -> the guest's signal queue.
  void pushKey(int scancode, bool down);
  void pushChar(uint32_t codepoint);
  void pushWindowClose();

  [[nodiscard]] const std::string& ocsrcDir() const { return ocsrcDir_; }
  [[nodiscard]] const std::string& machineDir() const { return machineDir_; }
  [[nodiscard]] bool quitRequested() const { return quitRequested_; }

 private:
  // Implemented as static thunks so they can be handed to Lua directly.
  static int luaLoad(lua_State* L);
  static int luaFsExists(lua_State* L);
  static int luaFsLoad(lua_State* L);
  static int luaFsRead(lua_State* L);
  static int luaFsWrite(lua_State* L);
  static int luaFsNewFile(lua_State* L);
  static int luaFsLines(lua_State* L);
  static int luaFsIsDirectory(lua_State* L);
  static int luaFsCreateDirectory(lua_State* L);
  static int luaFsGetDirectoryItems(lua_State* L);
  static int luaFsGetLastModified(lua_State* L);
  static int luaFsGetSize(lua_State* L);
  static int luaFsRemove(lua_State* L);
  static int luaFsGetSaveDirectory(lua_State* L);
  static int luaTimerGetTime(lua_State* L);
  static int luaSystemGetOS(lua_State* L);
  static int luaGetError(lua_State* L);
  static int luaKeyDown(lua_State* L);
  static int luaKeyUp(lua_State* L);
  static int luaTextInput(lua_State* L);
  static int luaWindowClose(lua_State* L);

  [[nodiscard]] std::string resolvePath(const std::string& relative) const;

  // Invokes every handler registered under `key` in elsa.handlers. This is how
  // a frame is driven: elsa.handlers.draw / .update, exactly as boot.lua does.
  void callHandlers(const char* key, bool passDt);

  lua_State* L_ = nullptr;
  LuaMachine* machine_ = nullptr;
  unsigned long long tickCount_ = 0;
  int tickMultiplier_ = 4;
  bool budgetPatched_ = false;
  std::string ocsrcDir_;
  std::string machineDir_;
  std::vector<ComponentSpec> components_;
  std::string lastError_;
  double startTime_ = 0.0;
  double lastDt_ = 0.0;
  double virtualTime_ = 0.0;  // accumulated from update(dt), see luaTimerGetTime
  bool quitRequested_ = false;
};

}  // namespace ocemu
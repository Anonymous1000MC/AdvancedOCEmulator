// ---------------------------------------------------------------------------
//  Application shell: SDL2 window, OpenGL3 context, ImGui setup, main loop.
//
//  Layout:
//    * a borderless full-screen ImGui window hosts the emulated OC screen, which
//      is drawn as an ImDrawList grid of character cells;
//    * the "Component Manager" panel floats above it on the left.
// ---------------------------------------------------------------------------
#pragma once

#include <chrono>
#include <SDL.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ocemu/Config.hpp"
#include "ocemu/GlyphAtlas.hpp"
#include "ocemu/OcFont.hpp"
#include "ocemu/OpenOsRestore.hpp"
#include "ocemu/InternetCard.hpp"
#include "ocemu/LuaMachine.hpp"
#include "ocemu/MemoryAllocator.hpp"
#include "ocemu/OcEmuHost.hpp"
#include "ocemu/ScreenBuffer.hpp"

namespace ocemu {

class App {
 public:
  // Exposed so the pointer-signal helper can reach the machine. Read-only.
  [[nodiscard]] OcEmuHost* ocEmuHost() const { return ocemu_.get(); }
  App();
  ~App();
  App(const App&) = delete;
  App& operator=(const App&) = delete;

  int run();

  // Command-line overrides, applied before run().
  void setConfigPath(std::filesystem::path p) { config_ = ConfigStore(std::move(p)); }
  void setRomPath(std::filesystem::path p) { romPath_ = std::move(p); }

 private:
  bool initSdl(std::string& err);
  bool initWindow(std::string& err);
  void initImGui(std::string& err);
  void shutdown();

  void processEvents();
  void applyConfigToCore();
  void performReboot();
  void drawScreen();
  // --- OpenComputers ---
  [[nodiscard]] bool detectOcEmu(std::string* err);
  bool bootOcEmu();
  void drawOcScreen();
  void drainAlerts();
  void drawOverlay();
  void drawConsole();
  void drawRestoreControls();

  [[nodiscard]] std::filesystem::path resolveConfigPath() const;
  [[nodiscard]] std::filesystem::path resolveRomPath() const;

  // --- host resources ---
  SDL_Window* window_ = nullptr;
  SDL_GLContext gl_ = nullptr;
  bool sdlReady_ = false;
  bool imguiReady_ = false;

  // --- emulated machine ---
  ConfigStore config_;
  ScreenBuffer screen_;
  MemoryAllocator allocator_;
  // The OpenComputers machine gets its OWN accounting allocator, deliberately
  // uncapped. OCEmu does not meter guest RAM at all - machine RAM is a fixed
  // spec - so the Component Manager's RAM slider must not bound this heap:
  // doing so bricked the machine outright ("out of memory: the allocator
  // refused a request of 128 bytes, cap 8388608 bytes"). We still count it, so
  // diagnostics can report real Lua heap usage.
  MemoryAllocator ocemuAllocator_;
  InternetCard internet_;
  GlyphAtlas atlas_;
  std::unique_ptr<LuaMachine> lua_;
  std::unique_ptr<OcEmuHost> ocemu_;

  // OpenComputers host (OCEmu's Lua core) wiring.
  bool ocEmuMode_ = false;
  std::string ocSrc_;
  std::string ocData_;

  // "Restore to OpenOS": refetch the EEPROM bootloader and reboot into it.
  OpenOsRestore restore_;
  std::string restoreMessage_;
  // Screen geometry of the last draw, so mouse coordinates can be mapped back
  // to OC cells for selection and clipboard work.
  struct CellRect {
    ImVec2 origin;
    float cellW = 1.0f;
    float cellH = 1.0f;
    int cols = 0;
    int rows = 0;
  };
  CellRect grid_{};
  bool haveGrid_ = false;

  // OC's own bitmap font, rasterised into a texture. This is what real
  // OpenComputers draws; using the host's monospace font instead made every
  // GUI program look subtly wrong, and cost one ImGui draw call per cell per
  // frame (8000 at tier 3).
  OcFont ocFont_;
  bool useOcFont_ = false;
  std::vector<std::uint32_t> screenPx_;
  unsigned screenTex_ = 0;
  int texW_ = 0;
  int texH_ = 0;
  std::uint64_t texRev_ = 0;
  std::vector<std::uint8_t> glyphScratch_;
  bool composeScreen(int cellW, int cellH);
  void uploadScreenTexture();
  void destroyScreenTexture();
  // Selection, in cell coordinates, inclusive. anchor is where the drag began.
  bool selecting_ = false;
  // Which button is currently held down, so motion can be reported as a drag.
  int dragButton_ = 0;
  int selAnchorX_ = 0, selAnchorY_ = 0;
  int selX0_ = 0, selY0_ = 0, selX1_ = 0, selY1_ = 0;

  // Maps a window position to a cell, or false when outside the grid.
  bool cellAt(ImVec2 pos, int* col, int* row) const;
  // Diagnostics: ask the guest what components it can see, so the host's own
  // list can be diffed against it.
  void refreshGuestComponents();
  std::vector<std::string> guestComponents_;
  std::string guestComponentsError_;
  bool guestComponentsQueried_ = false;
  std::string guestRamBytes_;
  bool pendingGuestQuery_ = false;
  void pasteClipboard();

  std::uint64_t lastOcRevision_ = 0;
  bool ocDiagDone_ = false;
  std::chrono::steady_clock::time_point lastOcDiag_{};
  HostInfo hostInfo_{};

  ImFont* screenFont_ = nullptr;
  ImFont* uiFont_ = nullptr;

  std::vector<std::string> alerts_;
  std::vector<std::string> logLines_;

  bool running_ = true;
  bool rebootRequested_ = false;
  bool quitRequested_ = false;
  bool saveRequested_ = false;
  bool showPanel_ = true;
  bool showStats_ = false;
  bool showConsole_ = false;

  std::filesystem::path romPath_;
  std::string glRenderer_;
  std::string glVersion_;
  std::string startupError_;

  double lastFrameTime_ = 0.0;
  double uptime_ = 0.0;
  float zoom_ = 1.0f;
  unsigned int frameCount_ = 0;
};

}  // namespace ocemu
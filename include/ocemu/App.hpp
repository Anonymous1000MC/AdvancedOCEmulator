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
#include "ocemu/OpenOsRestore.hpp"
#include "ocemu/InternetCard.hpp"
#include "ocemu/LuaMachine.hpp"
#include "ocemu/MemoryAllocator.hpp"
#include "ocemu/OcEmuHost.hpp"
#include "ocemu/ScreenBuffer.hpp"

namespace ocemu {

class App {
 public:
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
  // Selection, in cell coordinates, inclusive. anchor is where the drag began.
  bool selecting_ = false;
  int selAnchorX_ = 0, selAnchorY_ = 0;
  int selX0_ = 0, selY0_ = 0, selX1_ = 0, selY1_ = 0;

  // Maps a window position to a cell, or false when outside the grid.
  bool cellAt(ImVec2 pos, int* col, int* row) const;
  std::string selectedText() const;
  void copySelection();
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
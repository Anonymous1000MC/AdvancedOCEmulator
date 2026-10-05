#include "ocemu/App.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <utility>

#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <GL/glew.h>

#include "ocemu/OcScreen.hpp"
#include "ocemu/ui/ComponentManagerPanel.hpp"

namespace ocemu {
namespace fs = std::filesystem;

namespace {

constexpr const char* kWindowTitle = "OpenComputers Emulator";
constexpr int kGlMajor = 3;
constexpr int kGlMinor = 3;

}  // namespace

App::App()
    : config_(resolveConfigPath()), romPath_(resolveRomPath()) {
}

App::~App() { shutdown(); }

std::filesystem::path App::resolveConfigPath() const {
  // Prefer the source tree's config.json so a freshly cloned workspace picks up
  // the committed defaults; fall back to the working directory.
#if defined(OCEMU_SOURCE_DIR)
  const std::filesystem::path candidate =
      std::filesystem::path(OCEMU_SOURCE_DIR) / "config.json";
  std::error_code ec;
  if (std::filesystem::exists(candidate, ec)) return candidate;
  if (std::filesystem::exists(candidate.parent_path(), ec)) return candidate;
#endif
  return std::filesystem::current_path() / "config.json";
}

std::filesystem::path App::resolveRomPath() const {
#if defined(OCEMU_ASSET_DIR)
  return std::filesystem::path(OCEMU_ASSET_DIR) / "lua" / "boot.lua";
#else
  return std::filesystem::current_path() / "assets" / "lua" / "boot.lua";
#endif
}

// ---------------------------------------------------------------------------
//  SDL / GL / ImGui bring-up
// ---------------------------------------------------------------------------

bool App::initSdl(std::string& err) {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
    err = std::string("SDL_Init failed: ") + SDL_GetError();
    return false;
  }
  sdlReady_ = true;
  return true;
}

bool App::initWindow(std::string& err) {
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, kGlMajor);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, kGlMinor);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

  const Uint32 winFlags =
      SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_MAXIMIZED;

  window_ = SDL_CreateWindow(kWindowTitle, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 800,
                             winFlags);
  if (window_ == nullptr) {
    err = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
    return false;
  }

  gl_ = SDL_GL_CreateContext(window_);
  if (gl_ == nullptr) {
    err = std::string("SDL_GL_CreateContext failed: ") + SDL_GetError();
    return false;
  }

  SDL_GL_MakeCurrent(window_, gl_);
  SDL_GL_SetSwapInterval(1);  // vsync

  // GLEW must be initialised after the context exists. glewExperimental silences
  // the spurious GL_INVALID_ENUM that core profiles otherwise report from
  // glewInit's own probe of the extension strings.
  glewExperimental = GL_TRUE;
  const GLenum glewStatus = glewInit();
  if (glewStatus != GLEW_OK) {
    err = std::string("glewInit failed: ") +
          reinterpret_cast<const char*>(glewGetErrorString(glewStatus));
    return false;
  }
  glGetError();  // swallow glewInit's harmless probe error

  const auto* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
  const auto* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
  glRenderer_ = renderer != nullptr ? renderer : "unknown";
  glVersion_ = version != nullptr ? version : "unknown";

  return true;
}

void App::initImGui(std::string& err) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;  // no imgui.ini litter in the workspace
  io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;

  if (!atlas_.configure(32.0f)) {
    err = "failed to build the glyph atlas";
    return;
  }
  screenFont_ = atlas_.screenFont();
  uiFont_ = atlas_.uiFont();

  ImGui::StyleColorsDark();

  if (!ImGui_ImplSDL2_InitForOpenGL(window_, gl_)) {
    err = "ImGui_ImplSDL2_InitForOpenGL failed";
    return;
  }
  if (!ImGui_ImplOpenGL3_Init("#version 150")) {
    err = "ImGui_ImplOpenGL3_Init failed";
    return;
  }

  ImGui_ImplOpenGL3_CreateFontsTexture();

  imguiReady_ = true;
}

void App::shutdown() {
  if (imguiReady_) {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    imguiReady_ = false;
  }
  if (lua_) {
    lua_->shutdown();
    lua_.reset();
  }
  internet_.shutdown();
  if (gl_ != nullptr) {
    SDL_GL_DeleteContext(gl_);
    gl_ = nullptr;
  }
  if (window_ != nullptr) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
  }
  if (sdlReady_) {
    SDL_Quit();
    sdlReady_ = false;
  }
}

// ---------------------------------------------------------------------------
//  Core wiring
// ---------------------------------------------------------------------------

void App::applyConfigToCore() {
  const EmulatorConfig& cfg = config_.values();

  // Infinite memory maps to the -1 sentinel, which the allocator reads as
  // "no cap at all".
  const int ramKb = cfg.effectiveRamKb();
  allocator_.setLimitBytes(MemoryAllocator::bytesFromKb(ramKb));

  const TierSpec limits = cfg.displayLimits();
  screen_.applyLimits(limits);

  hostInfo_.gpuTier = cfg.gpuTier;
  hostInfo_.screenTier = cfg.screenTier;
  hostInfo_.ramLimitKb = ramKb;
  hostInfo_.internetEnabled = cfg.internetCard;
  hostInfo_.cols = limits.cols;
  hostInfo_.rows = limits.rows;
  hostInfo_.depth = limits.depth;

  // libcurl stays initialised either way so toggling the card is instant; the
  // binding is what comes and goes, decided in LuaMachine::registerApi().
  if (cfg.internetCard && !internet_.ready()) {
    std::string err;
    if (!internet_.init(&err)) {
      alerts_.push_back("internet card failed to start: " + err);
      std::fprintf(stderr, "ocemu: %s\n", err.c_str());
    }
  }
}

bool App::detectOcEmu(std::string* err) {
  // OCEmu's src/ holds main.lua and apis/; its machine data holds ocemu.cfg.
  const char* srcEnv = std::getenv("OCEMU_OC_SRC");
  const char* dataEnv = std::getenv("OCEMU_OC_DATA");

  // OCEmu's location, in order of preference:
  //   1. OCEMU_OC_SRC
  //   2. third_party/OCEmu/src, i.e. right next to this checkout, which is where
  //      `git clone --recursive` puts it
  //   3. alongside the machine data directory
  //   4. a few conventional install locations
  namespace fs = std::filesystem;
  const fs::path exeDir = fs::current_path();

  std::vector<fs::path> srcCandidates;
  if (srcEnv != nullptr && *srcEnv != '\0') srcCandidates.emplace_back(srcEnv);
  srcCandidates.push_back(exeDir / "third_party" / "OCEmu" / "src");
  srcCandidates.push_back(exeDir / ".." / "OCEmu" / "src");

  const char* home = std::getenv("HOME");
  if (home != nullptr) {
    srcCandidates.push_back(fs::path(home) / "OCEmu" / "src");
    srcCandidates.push_back(fs::path(home) / ".local" / "share" / "ocemu" / "src");
  }

  std::error_code ec;
  std::string src;
  for (const auto& c : srcCandidates) {
    if (fs::is_regular_file(c / "main.lua", ec)) { src = c.string(); break; }
  }
  if (src.empty()) {
    if (err != nullptr) {
      *err = "OCEmu sources not found. Set OCEMU_OC_SRC, or clone with "
             "`git clone --recursive` so third_party/OCEmu is present. Looked in:";
      for (const auto& c : srcCandidates) *err += " " + c.string();
    }
    return false;
  }

  // Machine data (ocemu.cfg plus the virtual disks) likewise.
  std::vector<fs::path> dataCandidates;
  if (dataEnv != nullptr && *dataEnv != '\0') dataCandidates.emplace_back(dataEnv);
  if (home != nullptr) dataCandidates.push_back(fs::path(home) / ".local" / "share" / "ocemu");
  dataCandidates.push_back(exeDir / "third_party" / "OCEmu" / "share" / "ocemu");

  std::string data;
  for (const auto& c : dataCandidates) {
    if (fs::is_directory(c, ec)) { data = c.string(); break; }
  }
  if (data.empty()) {
    if (err != nullptr) {
      *err = "OCEmu machine data not found. Set OCEMU_OC_DATA. Looked in:";
      for (const auto& c : dataCandidates) *err += " " + c.string();
    }
    return false;
  }

  ocSrc_ = src;
  ocData_ = data;
  return true;
}

bool App::bootOcEmu() {
  if (!ocemu_) ocemu_ = std::make_unique<OcEmuHost>();

  // Our tiers drive OCEmu's component list, which has to be written into
  // ocemu.cfg because apis/component.lua reads it at load time.
  std::string cfgErr;
  const EmulatorConfig& cfg = config_.values();
  if (!ocemu_->syncOcEmuConfig(ocData_, cfg.gpuTier, cfg.internetCard, &cfgErr)) {
    alerts_.push_back("component list: " + cfgErr);
  }

  if (lua_ == nullptr) return false;

  // elsa can only exist once a live lua_State does, so the VM is booted with a
  // trivial ROM first and OCEmu is started afterwards through runChunk.
  // LuaMachine::reboot() takes a path, so materialise the stub on disk.
  const fs::path stubRom = fs::temp_directory_path() / "ocemu_stub_boot.lua";
  {
    std::ofstream out(stubRom, std::ios::binary | std::ios::trunc);
    out << "-- placeholder ROM: the OpenComputers core is injected afterwards\n";
    if (!out) {
      alerts_.push_back("cannot write " + stubRom.string());
      return false;
    }
  }

  lua_->attach(LuaContext{});
  std::string err;
  if (!lua_->reboot(stubRom.string(), &err)) {
    alerts_.push_back("VM boot: " + err);
    return false;
  }

  ocemu_->install(lua_->L(), ocSrc_, ocData_);
  ocemu_->setMachine(lua_.get());

  // main.lua requires "ffi" unconditionally, but every use of it sits behind
  // `if not machine.beep and elsa.SDL` and we leave elsa.SDL false. A stub keeps
  // luaffifb out of the dependency graph.
  const fs::path compat = fs::path(OCEMU_ASSET_DIR) / "ocemu" / "component_compat.lua";
  const fs::path boot = fs::path(OCEMU_ASSET_DIR) / "ocemu" / "boot_openos.lua";

  const std::string stub =
      "package.preload[\"ffi\"] = function() return { os = \"Linux\", NULL = 0,"
      " new = function() return {} end, cast = function() return {} end,"
      " string = function(s) return tostring(s) end, typeof = function() return 0 end,"
      " C = function() end, copy = function(t) return t end } end\n"
      "_G.__ocemuCompatPath = [[" + compat.string() + "]]\n";

  if (!lua_->runChunk(stub, "=ocemu_ffi_stub", &err)) {
    alerts_.push_back("ffi stub: " + err);
    return false;
  }

  const std::string src = "assert(loadfile([[" + boot.string() + "]]))()";
  if (!lua_->runChunk(src, "=ocemu_boot", &err)) {
    alerts_.push_back("OpenComputers boot: " + err);
    std::fprintf(stderr, "ocemu: OpenComputers boot failed: %s\n", err.c_str());
    return false;
  }

  std::printf("ocemu: OpenComputers core started (%s)\n", ocSrc_.c_str());
  return true;
}

void App::performReboot() {
  applyConfigToCore();

  if (ocEmuMode_) {
    bootOcEmu();
    return;
  }

  if (lua_) {
    lua_->setHostInfo(hostInfo_);

    LuaContext ctx;
    ctx.screen = &screen_;
    ctx.allocator = &allocator_;
    ctx.internet = &internet_;
    ctx.alerts = &alerts_;
    ctx.logLines = &logLines_;
    ctx.rebootRequested = &rebootRequested_;
    ctx.quitRequested = &quitRequested_;
    lua_->attach(ctx);

    std::string err;
    if (lua_->reboot(romPath_.string(), &err)) {
      screen_.markDrawn();
    } else {
      alerts_.push_back(err.empty() ? "Lua reboot failed" : err);
      std::fprintf(stderr, "ocemu: reboot failed: %s\n", err.c_str());
    }
  }
}

// ---------------------------------------------------------------------------
//  Rendering
// ---------------------------------------------------------------------------

void App::drawOcScreen() {
  OcScreen* oc = OcScreen::active();
  if (oc == nullptr || oc->cells().empty()) return;

  // Load OC's bitmap font on first use. It ships with OCEmu next to its Lua
  // sources. Without it every GUI program renders in the host's monospace font,
  // which is both wrong-looking and, for wide glyphs, wrong-width.
  if (!ocFont_.loaded() && ocemu_ != nullptr) {
    useOcFont_ = ocFont_.load(ocemu_->ocsrcDir() + "/font.hex");
    // Hand it to the screen so screen.set() advances by the real glyph width
    // instead of guessing one cell per codepoint.
    if (useOcFont_) oc->setFont(&ocFont_);
  }

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();

  const float baseW = std::max(1.0f, atlas_.cellWidth());
  const float baseH = std::max(1.0f, atlas_.cellHeight());

  // The OC screen's own resolution, not our emulated char-matrix config.
  const float gridW = baseW * static_cast<float>(oc->width());
  const float gridH = baseH * static_cast<float>(oc->height());
  const float fit = std::min(1.0f, std::min(avail.x / std::max(1.0f, gridW),
                                            avail.y / std::max(1.0f, gridH)));
  const float cellH = std::max(1.0f, std::floor(baseH * fit * zoom_));
  const float cellW = std::max(1.0f, std::floor(baseW * (cellH / baseH)));

  const float totalW = cellW * static_cast<float>(oc->width());
  const float totalH = cellH * static_cast<float>(oc->height());
  const ImVec2 gridOrigin(origin.x + std::floor((avail.x - totalW) * 0.5f),
                          origin.y + std::floor((avail.y - totalH) * 0.5f));

  dl->AddRectFilled(gridOrigin, ImVec2(gridOrigin.x + totalW, gridOrigin.y + totalH),
                    IM_COL32(8, 8, 10, 255));
  dl->AddRect(gridOrigin, ImVec2(gridOrigin.x + totalW, gridOrigin.y + totalH),
              IM_COL32(70, 70, 80, 255));

  // Remember the geometry so mouse input can be mapped back to OC cells.
  grid_.origin = gridOrigin;
  grid_.cellW = cellW;
  grid_.cellH = cellH;
  grid_.cols = oc->width();
  grid_.rows = oc->height();
  haveGrid_ = true;

  const auto& cells = oc->cells();

  // Prefer OC's own bitmap font. It is the authentic rendering, and it turns
  // 8000 per-cell ImGui draw calls into a single textured quad.
  if (useOcFont_ && ocFont_.loaded() &&
      composeScreen(static_cast<int>(std::lround(cellW)),
                    static_cast<int>(std::lround(cellH)))) {
    uploadScreenTexture();
    dl->AddImage(static_cast<ImTextureID>(static_cast<std::intptr_t>(screenTex_)),
                 gridOrigin, ImVec2(gridOrigin.x + totalW, gridOrigin.y + totalH));
  } else {
    const int cols = oc->width();
    // Constant for the whole frame; computing it per cell was pure waste.
    const auto metrics = atlas_.cellMetrics(cellW, cellH);
    for (int y = 0; y < oc->height(); ++y) {
      for (int x = 0; x < cols; ++x) {
        const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(cols) +
                              static_cast<std::size_t>(x);
        if (i >= cells.size()) break;
        const OcScreen::Cell& c = cells[i];
        if (c.continuation) continue;
        const ImVec2 tl(gridOrigin.x + static_cast<float>(x) * cellW,
                        gridOrigin.y + static_cast<float>(y) * cellH);
        drawCharacterCell(dl, atlas_, tl, cellW, cellH,
                          static_cast<std::uint16_t>(c.value),
                          oc->resolveColor(c.fg, c.fgPalette),
                          oc->resolveColor(c.bg, c.bgPalette), metrics);
      }
    }
  }
  // Selection highlight, drawn over the cells.
  if (haveGrid_ && (selX0_ != selX1_ || selY0_ != selY1_)) {
    const ImVec2 a(grid_.origin.x + static_cast<float>(selX0_) * grid_.cellW,
                   grid_.origin.y + static_cast<float>(selY0_) * grid_.cellH);
    const ImVec2 b(grid_.origin.x + static_cast<float>(selX1_ + 1) * grid_.cellW,
                   grid_.origin.y + static_cast<float>(selY1_ + 1) * grid_.cellH);
    dl->AddRectFilled(a, b, IM_COL32(80, 130, 220, 90));
  }

  lastOcRevision_ = oc->revision();

  // Throttled live counter: proves the guest is actually drawing, and how far
  // along the boot is.
  const auto now = std::chrono::steady_clock::now();
  if (!ocDiagDone_ || now - lastOcDiag_ >= std::chrono::seconds(10)) {
    ocDiagDone_ = true;
    lastOcDiag_ = now;
    std::size_t nonBlank = 0;
    for (const auto& c : cells) {
      if (c.value != ' ' && c.value != 0) ++nonBlank;
    }
    std::printf("ocemu: screen %dx%d tier-depth %d, %zu/%zu non-blank cells"
                "  ticks=%llu vtime=%.2fs cells=%zu/%zu %s\n",
                oc->width(), oc->height(), oc->tier(), nonBlank, cells.size(),
                ocemu_ ? ocemu_->tickCount() : 0ull,
                ocemu_ ? ocemu_->virtualTime() : 0.0, nonBlank, cells.size(),
                ocemu_ ? ocemu_->machineStatus().c_str() : "");
    std::fflush(stdout);
  }
}

void App::drawScreen() {
  if (ocEmuMode_) {
    drawOcScreen();
    return;
  }
  if (!screen_.valid()) return;

  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();

  // Fit the grid inside the available area, then centre it.
  const float baseCellW = std::max(1.0f, std::round(atlas_.cellWidth()));
  const float baseCellH = std::max(1.0f, std::round(atlas_.cellHeight()));

  const float gridW = baseCellW * static_cast<float>(screen_.cols());
  const float gridH = baseCellH * static_cast<float>(screen_.rows());

  const float fit = std::min(1.0f, std::min(avail.x / std::max(1.0f, gridW),
                                            avail.y / std::max(1.0f, gridH)));
  // Fit on the limiting axis, then derive the cell's width from its height via
  // the font's own aspect ratio. Computing both independently lets rounding
  // drift the ratio, which is what makes glyphs overrun their cell sideways.
  const float cellH = std::max(1.0f, std::floor(baseCellH * fit * zoom_));
  const float cellW = std::max(1.0f, std::floor(baseCellW * (cellH / baseCellH)));

  const float totalW = cellW * static_cast<float>(screen_.cols());
  const float totalH = cellH * static_cast<float>(screen_.rows());

  const ImVec2 gridOrigin(origin.x + std::floor((avail.x - totalW) * 0.5f),
                          origin.y + std::floor((avail.y - totalH) * 0.5f));
  const ImVec2 gridEnd(gridOrigin.x + totalW, gridOrigin.y + totalH);

  // The screen is opaque: a CRT-style black surround with a thin bezel.
  dl->AddRectFilled(gridOrigin, gridEnd, IM_COL32(8, 8, 10, 255));
  dl->AddRect(gridOrigin, gridEnd, IM_COL32(70, 70, 80, 255));

  const int cols = screen_.cols();
  const int rows = screen_.rows();
  const auto metrics = atlas_.cellMetrics(cellW, cellH);

  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; ++x) {
      const Cell& c = screen_.cell(x, y);
      const ImVec2 tl(gridOrigin.x + static_cast<float>(x) * cellW,
                      gridOrigin.y + static_cast<float>(y) * cellH);
      drawCharacterCell(dl, atlas_, tl, cellW, cellH, c.glyph, c.fg, c.bg, metrics);
    }
  }

  screen_.markDrawn();
}

void App::drainAlerts() {
  // Alerts queued by Lua during this frame surface as a toast strip.
  if (alerts_.empty()) return;

  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 420.0f, 16.0f),
                          ImGuiCond_Always, ImVec2(1.0f, 0.0f));
  ImGui::SetNextWindowSize(ImVec2(404.0f, 0.0f), ImGuiCond_Always);

  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;

  if (ImGui::Begin("##ocemu-alerts", nullptr, flags)) {
    for (std::size_t i = 0; i < alerts_.size(); ++i) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
      ImGui::BulletText("%s", alerts_[i].c_str());
      ImGui::PopStyleColor();
    }
  }
  ImGui::End();

  alerts_.clear();
}

// ---------------------------------------------------------------------------
//  Restore to OpenOS
// ---------------------------------------------------------------------------

// The machine boots from its EEPROM's code.lua. Flash another OS over it (their
// installers do exactly that) and OpenOS is gone; this puts it back. The
// download runs on a worker thread with a progress bar, and a successful restore
// reboots the machine so the new bootloader is what actually runs.
void App::drawRestoreControls() {
  if (!ocEmuMode_) return;

  const bool busy = restore_.running();
  ImGui::SeparatorText("OpenOS");
  ImGui::TextWrapped("Restores the OpenOS bootloader into the machine's EEPROM "
                     "and reboots. Use this after another OS installer has "
                     "overwritten it.");

  if (busy) {
    const float p = restore_.progress();
    char overlay[32];
    if (p < 0.0f) {
      std::snprintf(overlay, sizeof(overlay), "%s", restore_.status().c_str());
    } else {
      std::snprintf(overlay, sizeof(overlay), "%.0f%%", static_cast<double>(p) * 100.0);
    }
    ImGui::ProgressBar(p, ImVec2(-1.0f, 0.0f), overlay);
    ImGui::TextDisabled("%s", restore_.status().c_str());
  } else {
    if (ImGui::Button("Restore to OpenOS", ImVec2(180.0f, 0.0f))) {
      restoreMessage_.clear();
      restore_.start(defaultOpenOsUrl());
    }
    if (!restoreMessage_.empty()) {
      ImGui::TextWrapped("%s", restoreMessage_.c_str());
    }
  }

  // Latch the result on the UI thread: the payload is written here, not on the
  // worker, so the filesystem write and the reboot stay on one thread.
  if (!busy && restore_.succeeded()) {
    const std::string payload = restore_.takePayload();
    const std::string codePath = findEepromCodePath(ocData_);
    if (codePath.empty()) {
      restoreMessage_ = "Restore failed: no EEPROM found in " + ocData_;
    } else {
      std::error_code ec;
      const fs::path target(codePath);
      const fs::path tmp = target.string() + ".new";
      {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
          restoreMessage_ = "Restore failed: cannot write " + tmp.string();
        } else {
          out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
          out.close();
          fs::rename(tmp, target, ec);
          if (ec) {
            restoreMessage_ = "Restore failed: " + ec.message();
          } else {
            restoreMessage_ = "OpenOS bootloader restored (" +
                              std::to_string(payload.size()) + " bytes). Rebooting...";
            alerts_.push_back(restoreMessage_);
            rebootRequested_ = true;
          }
        }
      }
    }
  }
}

void App::drawOverlay() {
  if (showStats_) {
    const MemoryAllocator& alloc = allocator_;
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("Diagnostics", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("GL        : %s", glRenderer_.c_str());
      ImGui::Text("Version   : %s", glVersion_.c_str());
      ImGui::Text("Font      : %s",
                  atlas_.usingSystemFont() ? atlas_.fontPath().c_str() : "ImGui built-in");
      ImGui::Text("Grid      : %d x %d @ %d-bit", screen_.cols(), screen_.rows(),
                  screen_.depth());
      ImGui::Text("RAM       : %zu / ", alloc.usedBytes());
      ImGui::SameLine();
      if (alloc.infinite()) {
        ImGui::TextUnformatted("infinite");
      } else {
        ImGui::Text("%zu bytes", alloc.limitBytes());
      }
      ImGui::Text("Peak      : %zu bytes", alloc.peakBytes());
      ImGui::Text("Requests  : %zu active / %zu total", internet_.activeRequests(),
                  internet_.totalRequests());
      ImGui::Text("Uptime    : %.1f s", uptime_);
      ImGui::Text("Frames    : %u", frameCount_);
      ImGui::Spacing();
      ImGui::TextUnformatted("Zoom");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(120.0f);
      ImGui::SliderFloat("##zoom", &zoom_, 0.25f, 3.0f, "%.2fx");
    }
          drawRestoreControls();
ImGui::End();
  }
}


// ---------------------------------------------------------------------------
//  Clipboard: selection and host clipboard passthrough
// ---------------------------------------------------------------------------

bool App::composeScreen(int cellW, int cellH) {
  OcScreen* oc = OcScreen::active();
  if (oc == nullptr || cellW <= 0 || cellH <= 0) return false;
  const int cols = oc->width();
  const int rows = oc->height();
  const int w = cols * cellW;
  const int h = rows * cellH;
  // A resize changes the texture dimensions, so force a re-upload.
  if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return false;
  if (static_cast<int>(screenPx_.size()) != w * h) {
    screenPx_.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    texRev_ = 0;
  }

  const auto& cells = oc->cells();
  const std::uint64_t rev = oc->revision();
  if (rev == texRev_ && texW_ == w && texH_ == h) return true;

  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(cols) +
                            static_cast<std::size_t>(x);
      if (i >= cells.size()) break;
      const OcScreen::Cell& c = cells[i];

      // A continuation cell is the second half of a double-width glyph. The
      // glyph that owns it already painted across both cells, so skipping it is
      // what stops us overwriting half of every wide character with a blank.
      if (c.continuation) continue;

      const auto pack = [](const Rgb& c) -> std::uint32_t {
        return static_cast<std::uint32_t>(c.r) | (static_cast<std::uint32_t>(c.g) << 8) |
               (static_cast<std::uint32_t>(c.b) << 16) | 0xFF000000u;
      };
      const std::uint32_t bg = pack(oc->resolveColor(c.bg, c.bgPalette));
      const std::uint32_t fg = pack(oc->resolveColor(c.fg, c.fgPalette));
      const int gw = (c.value != 0 && ocFont_.isWide(c.value)) ? 2 : 1;
      const int spanW = gw * cellW;
      if (x * cellW + spanW > w) continue;

      // Glyphs are 16px tall and gw*8 wide; scale to the cell with
      // nearest-neighbour sampling so the bitmap font stays crisp.
      const int fw = (c.value != 0) ? ocFont_.bitmap(c.value, &glyphScratch_) * 8 : 0;
      std::uint32_t* base = &screenPx_[static_cast<std::size_t>(y * cellH) *
                                        static_cast<std::size_t>(w)];
      for (int py = 0; py < cellH; ++py) {
        const int fy = std::min(15, py * 16 / cellH);
        std::uint32_t* dst = base + static_cast<std::size_t>(py) * static_cast<std::size_t>(w) +
                             static_cast<std::size_t>(x * cellW);
        const std::uint8_t* srcRow =
            glyphScratch_.data() + static_cast<std::size_t>(fy) * 16u;
        for (int px = 0; px < spanW; ++px) {
          const int fx = fw > 0 ? std::min(fw - 1, px * fw / spanW) : -1;
          dst[px] = (fx >= 0 && srcRow[fx] != 0) ? fg : bg;
        }
      }
    }
  }
  texRev_ = rev;
  texW_ = w;
  texH_ = h;
  return true;
}

void App::uploadScreenTexture() {
  if (screenTex_ == 0) {
    glGenTextures(1, &screenTex_);
    glBindTexture(GL_TEXTURE_2D, screenTex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  glBindTexture(GL_TEXTURE_2D, screenTex_);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, texW_, texH_, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, screenPx_.data());
}

void App::destroyScreenTexture() {
  if (screenTex_ != 0) {
    glDeleteTextures(1, &screenTex_);
    screenTex_ = 0;
  }
}

bool App::cellAt(ImVec2 pos, int* col, int* row) const {
  if (!haveGrid_ || grid_.cellW <= 0.0f || grid_.cellH <= 0.0f) return false;
  const float fx = (pos.x - grid_.origin.x) / grid_.cellW;
  const float fy = (pos.y - grid_.origin.y) / grid_.cellH;
  if (fx < 0.0f || fy < 0.0f) return false;
  const int x = static_cast<int>(fx);
  const int y = static_cast<int>(fy);
  if (x < 0 || y < 0 || x >= grid_.cols || y >= grid_.rows) return false;
  *col = x;
  *row = y;
  return true;
}

// The selected block of text, with trailing spaces trimmed per line and
// interior blank runs collapsed, the way a terminal selection reads.
std::string App::selectedText() const {
  OcScreen* oc = OcScreen::active();
  if (oc == nullptr || oc->cells().empty()) return {};
  const int cols = oc->width();
  const auto& cells = oc->cells();

  std::string out;
  for (int y = selY0_; y <= selY1_; ++y) {
    std::string line;
    for (int x = selX0_; x <= selX1_; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * static_cast<std::size_t>(cols) +
                            static_cast<std::size_t>(x);
      if (i >= cells.size()) break;
      const std::uint32_t g = cells[i].value;
      line.push_back(g == 0 ? ' ' : static_cast<char>(g));
    }
    while (!line.empty() && line.back() == ' ') line.pop_back();
    if (y != selY0_) line.push_back('\n');
    out += line;
  }
  return out;
}

void App::copySelection() {
  const std::string text = selectedText();
  if (text.empty()) return;
  SDL_SetClipboardText(text.c_str());
}

void App::pasteClipboard() {
  if (ocemu_ == nullptr) return;
  char* clip = SDL_GetClipboardText();
  if (clip == nullptr) return;
  // Feed the clipboard through as real key events so the guest's own key
  // handling (layout, cursor, shell readline) sees it exactly as if typed.
  for (const char* p = clip; *p != '\0'; ++p) {
    switch (*p) {
      case '\n':
      case '\r':
        ocemu_->pushKey("Return");
        break;
      case '\t':
        ocemu_->pushKey("Tab");
        break;
      default:
        ocemu_->pushKey(std::string(1, *p));
        break;
    }
  }
  SDL_free(clip);
}

void App::processEvents() {
  SDL_Event ev;
  while (SDL_PollEvent(&ev) != 0) {
    ImGui_ImplSDL2_ProcessEvent(&ev);

    if (ev.type == SDL_QUIT) {
      running_ = false;
    } else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE &&
               ev.window.windowID == SDL_GetWindowID(window_)) {
      running_ = false;
    } else if (ocEmuMode_ && ocemu_ != nullptr) {
      // Forward real keyboard input to the guest. We use SDL_TEXTINPUT for
      // printable characters (it already applies the user's keyboard layout)
      // and SDL_KEYDOWN for named/navigation keys.
      switch (ev.type) {
        case SDL_TEXTINPUT:
          // SDL_TEXTINPUT already reflects the user's keyboard layout, so
          // printable characters go through as-is.
          if (ev.text.text[0] != '\0') ocemu_->pushKey(std::string(1, ev.text.text[0]));
          break;
        case SDL_MOUSEBUTTONDOWN:
          if (ev.button.button == SDL_BUTTON_MIDDLE) {
            pasteClipboard();
          } else if (ev.button.button == SDL_BUTTON_LEFT && ocEmuMode_) {
            int c = 0, r = 0;
            if (cellAt(ImVec2(ev.button.x, ev.button.y), &c, &r)) {
              if (OcScreen* oc = OcScreen::active()) oc->setMouseCell(c, r);
              selecting_ = true;
              selAnchorX_ = c;
              selAnchorY_ = r;
              selX0_ = selX1_ = c;
              selY0_ = selY1_ = r;
            } else {
              selecting_ = false;
            }
            if (OcScreen* oc = OcScreen::active()) {
              const int b = ev.button.button == SDL_BUTTON_LEFT ? 1
                             : ev.button.button == SDL_BUTTON_RIGHT ? 2 : 3;
              oc->setMouseButton(b, true);
            }
          }
          break;
        case SDL_MOUSEBUTTONUP:
          if (ev.button.button == SDL_BUTTON_LEFT) selecting_ = false;
          if (ocEmuMode_) {
            if (OcScreen* oc = OcScreen::active()) {
              const int b = ev.button.button == SDL_BUTTON_LEFT ? 1
                             : ev.button.button == SDL_BUTTON_RIGHT ? 2 : 3;
              oc->setMouseButton(b, false);
            }
          }
          break;
        case SDL_MOUSEMOTION:
          // Keep the guest's pointer in sync with the host cursor. Real
          // OpenComputers exposes screen.getMousePosition(); without this a GUI
          // program has no way to know where the pointer is.
          if (ocEmuMode_) {
            if (OcScreen* oc = OcScreen::active()) {
              int c = 0, r = 0;
              if (cellAt(ImVec2(ev.motion.x, ev.motion.y), &c, &r)) {
                oc->setMouseCell(c, r);
              } else {
                oc->setMouseCell(-1, -1);
              }
            }
          }
          if (selecting_ && ocEmuMode_) {
            int c = 0, r = 0;
            if (cellAt(ImVec2(ev.motion.x, ev.motion.y), &c, &r)) {
              selX0_ = std::min(selAnchorX_, c);
              selX1_ = std::max(selAnchorX_, c);
              selY0_ = std::min(selAnchorY_, r);
              selY1_ = std::max(selAnchorY_, r);
            }
          }
          break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
          const char* name = SDL_GetKeyName(ev.key.keysym.sym);
          if (name == nullptr || name[0] == '\0') break;
          const std::string key(name);
          const bool down = ev.type == SDL_KEYDOWN;
          // Modifier state is tracked even on auto-repeat; the key event itself
          // is not, or holding a key would spam the guest.
          const std::uint16_t mod = SDL_GetModState();
          ocemu_->setModifier("shift", (mod & KMOD_SHIFT) != 0);
          ocemu_->setModifier("control", (mod & KMOD_CTRL) != 0);
          ocemu_->setModifier("alt", (mod & KMOD_ALT) != 0);
          // Only forward keys that produce NO text here. Printable characters
          // already arrive via SDL_TEXTINPUT, which applies the user's layout,
          // Shift and CapsLock correctly; forwarding them again from KEYDOWN
          // would deliver every letter twice.
          // Clipboard passthrough. Ctrl+C / Ctrl+V work whether or not a
          // selection exists; middle-click pastes, as terminals traditionally
          // do. Ctrl+Shift+C/V are accepted too, since Ctrl+C alone is usually
          // SIGINT inside the guest.
          if (down && ev.key.repeat == 0) {
            const std::uint16_t mod = SDL_GetModState();
            const bool ctrl = (mod & KMOD_CTRL) != 0;
            const bool shift = (mod & KMOD_SHIFT) != 0;
            const bool copy = (ctrl && key == "c") || (ctrl && shift && key == "C");
            const bool paste = (ctrl && key == "v") || (ctrl && shift && key == "V");
            if (copy) {
              copySelection();
            } else if (paste) {
              pasteClipboard();
            } else {
              const bool printable = ev.key.keysym.sym >= 32 && ev.key.keysym.sym < 127;
              if (!printable) ocemu_->pushKey(key);
            }
          }
          ocemu_->setKeyState(key, down);
          break;
        }
        default:
          break;
      }
    }
  }
}

// ---------------------------------------------------------------------------
//  Main loop
// ---------------------------------------------------------------------------

int App::run() {
  std::string err;
  if (!initSdl(err)) {
    std::fprintf(stderr, "ocemu: %s\n", err.c_str());
    return 1;
  }
  if (!initWindow(err)) {
    std::fprintf(stderr, "ocemu: %s\n", err.c_str());
    shutdown();
    return 1;
  }

  initImGui(err);
  if (!imguiReady_) {
    std::fprintf(stderr, "ocemu: %s\n", err.c_str());
    shutdown();
    return 1;
  }

  // --- load config, then bring the machine up ---
  if (!config_.load() && !config_.lastError().empty()) {
    alerts_.push_back(config_.lastError());
  }

  lua_ = std::make_unique<LuaMachine>();

  // Prefer running a real OpenComputers OS: OCEmu's Lua core is the emulator and
  // its SDL shell is what we replace. Fall back to the bundled demo ROM.
  std::string ocErr;
  if (detectOcEmu(&ocErr)) {
    ocEmuMode_ = true;
    std::printf("ocemu: using OpenComputers core (%s)\n", ocSrc_.c_str());
  } else {
    std::printf("ocemu: %s -- falling back to the demo ROM\n", ocErr.c_str());
  }

  performReboot();

  std::printf("ocemu: config  %s\n", config_.path().string().c_str());
  std::printf("ocemu: rom     %s\n", romPath_.string().c_str());
  std::printf("ocemu: gpu     %s\n", glRenderer_.c_str());
  std::fprintf(stdout, "ocemu: display %dx%d @ %d-bit\n", screen_.cols(), screen_.rows(),
               screen_.depth());
  std::fflush(stdout);

  const Uint64 perfFreq = SDL_GetPerformanceFrequency();
  lastFrameTime_ = static_cast<double>(SDL_GetPerformanceCounter()) / static_cast<double>(perfFreq);

  while (running_) {
    const Uint64 nowCounter = SDL_GetPerformanceCounter();
    const double now = static_cast<double>(nowCounter) / static_cast<double>(perfFreq);
    double dt = now - lastFrameTime_;
    lastFrameTime_ = now;
    if (dt < 0.0) dt = 0.0;
    if (dt > 0.25) dt = 0.25;  // a long stall must not turn into a huge step
    uptime_ += dt;
    ++frameCount_;

    processEvents();

    // --- host-side updates ---
    internet_.pump();
    if (lua_) lua_->update(dt);

    // The OpenComputers machine is ticked through elsa.update, exactly as
    // OCEmu's own boot loop does.
    if (ocemu_) ocemu_->update(dt);

    if (rebootRequested_) {
      rebootRequested_ = false;
      performReboot();
    }
    if (saveRequested_) {
      saveRequested_ = false;
      if (config_.save()) {
        alerts_.push_back("saved " + config_.path().filename().string());
      } else {
        alerts_.push_back(config_.lastError());
      }
    }
    if (quitRequested_) {
      quitRequested_ = false;
      running_ = false;
    }

    // --- render ---
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);

    const ImGuiWindowFlags screenFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoBringToFrontOnFocus |
                                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.07f, 1.0f));
    if (ImGui::Begin("##ocemu-screen", nullptr, screenFlags)) {
      drawScreen();
    }
    ImGui::End();
    ImGui::PopStyleColor();

    // --- the Component Manager panel ---
    ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_FirstUseEver);

    if (ImGui::Begin("Component Manager", &showPanel_)) {
      PanelContext pc;
      pc.config = &config_;
      pc.lua = lua_.get();
      pc.allocator = &allocator_;
      pc.screen = &screen_;
      pc.internet = &internet_;
      pc.hostInfo = &hostInfo_;
      pc.rebootRequested = &rebootRequested_;
      pc.saveRequested = &saveRequested_;
      pc.showPanel = &showPanel_;
      drawComponentManager(pc);

      ImGui::Separator();
      if (ImGui::Button("Diagnostics", ImVec2(120.0f, 0.0f))) showStats_ = !showStats_;
      ImGui::SameLine();
      if (ImGui::Button("Quit", ImVec2(80.0f, 0.0f))) running_ = false;
    }
    ImGui::End();

    drainAlerts();
    drawOverlay();

    ImGui::Render();

    int drawableWidth = 0;
    int drawableHeight = 0;
    SDL_GL_GetDrawableSize(window_, &drawableWidth, &drawableHeight);
    glViewport(0, 0, drawableWidth, drawableHeight);
    glClearColor(0.04f, 0.04f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    SDL_GL_SwapWindow(window_);
  }

  shutdown();
  return 0;
}

}  // namespace ocemu
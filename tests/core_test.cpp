// ---------------------------------------------------------------------------
//  Headless tests for the emulator core.
//
//  These cover the parts whose failures are invisible in the GUI: the tier
//  intersection table, colour quantisation, screen geometry, the custom
//  allocator's accounting, config persistence, and -- most importantly -- the
//  Lua out-of-memory path, whose panic/longjmp handling is the easiest thing in
//  this codebase to get subtly wrong.
//
//  No external test framework: this is a self-contained main() that exits
//  non-zero on failure, wired up with CTest in the top-level CMakeLists.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <cmath>

#include "ocemu/Config.hpp"
#include "ocemu/GlyphAtlas.hpp"
#include "ocemu/LuaMachine.hpp"
#include "ocemu/MemoryAllocator.hpp"
#include "ocemu/OcEmuHost.hpp"
#include <chrono>

#include "ocemu/OcScreen.hpp"
#include "ocemu/OcFont.hpp"
#include "ocemu/OcSound.hpp"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "ocemu/ScreenBuffer.hpp"
#include "ocemu/Tiers.hpp"

using namespace ocemu;

namespace {

// Locates OCEmu's source tree the same way the app does: OCEMU_OC_SRC, then
// third_party/OCEmu/src, then a couple of conventional places. Keeps the tests
// runnable on any machine and inside a fresh clone.
std::string findOcEmuSrc() {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (const char* env = std::getenv("OCEMU_OC_SRC"); env != nullptr && *env != '\0' &&
      fs::is_regular_file(fs::path(env) / "main.lua", ec)) {
    return env;
  }
  const fs::path base = fs::current_path();
  for (const fs::path& c : {base / "third_party" / "OCEmu" / "src", base / ".." / "OCEmu" / "src"}) {
    if (fs::is_regular_file(c / "main.lua", ec)) return c.string();
  }
  if (const char* home = std::getenv("HOME"); home != nullptr) {
    for (const fs::path& c : {fs::path(home) / "OCEmu" / "src",
                              fs::path(home) / ".local" / "share" / "ocemu" / "src"}) {
      if (fs::is_regular_file(c / "main.lua", ec)) return c.string();
    }
  }
  return {};
}

std::string findOcEmuData() {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (const char* env = std::getenv("OCEMU_OC_DATA"); env != nullptr && *env != '\0' &&
      fs::is_directory(fs::path(env), ec)) {
    return env;
  }
  const fs::path base = fs::current_path();
  if (const char* home = std::getenv("HOME"); home != nullptr) {
    const fs::path d = fs::path(home) / ".local" / "share" / "ocemu";
    if (fs::is_directory(d, ec)) return d.string();
  }
  if (fs::is_directory(base / "third_party" / "OCEmu" / "share" / "ocemu", ec)) {
    return (base / "third_party" / "OCEmu" / "share" / "ocemu").string();
  }
  return {};
}


int g_checks = 0;
int g_failures = 0;
const char* g_group = "";

void group(const char* name) {
  g_group = name;
  std::printf("\n[%s]\n", name);
}

void check(bool ok, const char* expr, int line) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL (line %d): %s\n", line, expr);
  }
}

template <typename A, typename B>
void checkEq(const A& got, const B& want, const char* expr, int line) {
  ++g_checks;
  if (!(got == want)) {
    ++g_failures;
    std::printf("  FAIL (line %d): %s == %s\n", line, expr, "value");
  }
}

#define CHECK(expr) check((expr), #expr, __LINE__)
#define CHECK_EQ(got, want) checkEq((got), (want), #got " == " #want, __LINE__)

std::string writeTempRom(const std::string& name, const std::string& body) {
  const std::filesystem::path p = std::filesystem::temp_directory_path() / name;
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << body;
  out.close();
  return p.string();
}

// ---------------------------------------------------------------------------

// OC's bitmap font: the decoder must reproduce a recognisable glyph and the
// variable widths that OC's screen.set relies on.
void testOcFont() {
  const std::filesystem::path hexPath = std::filesystem::path(findOcEmuSrc() + "/font.hex");
  if (!std::filesystem::exists(hexPath)) {
    std::printf("[OC font] SKIPPED (font.hex not found)\n");
    return;
  }
  OcFont font;
  CHECK(font.load(hexPath.string()));
  CHECK(font.loaded());

  std::vector<std::uint8_t> bits;
  font.bitmap('A', &bits);
  CHECK_EQ(bits.size(), static_cast<std::size_t>(OcFont::kGlyphHeight * OcFont::kCellWidth * 2));

  // 'A' is 8x16 half width in OC's font.
  CHECK_EQ(font.width('A'), 1);
  CHECK(!font.isWide('A'));

  // Row 7 of 'A' is the crossbar ".######.": 6 contiguous set pixels starting
  // at column 1.
  bool crossbar = true;
  for (int x = 0; x < 8; ++x) {
    const std::size_t i = static_cast<std::size_t>(7) * OcFont::kCellWidth * 2 +
                          static_cast<std::size_t>(x);
    const bool want = x >= 1 && x <= 6;
    if ((bits[i] != 0) != want) crossbar = false;
  }
  CHECK(crossbar);

  // OC's font is variable width: getCharWidth() is #hexdigits/32, so a 32-digit
  // entry is one cell and a 64-digit entry is two. Box drawing (U+2550, U+2502) is
  // single cell width -- OpenOS's banner frame is drawn one character per cell.
  CHECK_EQ(font.width(0x2550), 1);  // BOX DRAWING DOUBLE HORIZONTAL
  CHECK(!font.isWide(0x2550));
  CHECK_EQ(font.width(0x2502), 1);  // BOX DRAWINGS LIGHT VERTICAL
  CHECK_EQ(font.width('W'), 1);
  // A 64-digit entry really is double width, and its bitmap is 16px across.
  CHECK_EQ(font.width(0x01), 2);
  CHECK(font.isWide(0x01));
  font.bitmap(0x01, &bits);
  CHECK_EQ(bits.size(), static_cast<std::size_t>(OcFont::kGlyphHeight * OcFont::kCellWidth * 2));
}

// Guard against the screen regressing back to O(cells) per write.
//
// This was a real bug, not a hypothetical: the renderer used to keep a second,
// palette-resolved copy of the cell array and rebuild ALL of it after every
// single cell write, so repainting a 160x50 screen cost 254 ms and any GUI
// program crawled. Writes are O(1) now and the host resolves colours once per
// drawn frame, so a full repaint is effectively free.
void testScreenWriteThroughput() {
  std::printf("[screen write throughput]\n");
  for (const int tier : {2, 3}) {
    const int w = tier == 3 ? 160 : 80;
    const int h = tier == 3 ? 50 : 25;

    OcScreen screen;
    screen.configure(w, h, 3, 0);

    constexpr int kRepaints = 50;
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < kRepaints; ++r) {
      for (int y = 1; y <= h; ++y) {
        for (int x = 1; x <= w; ++x) screen.writeForTest(x, y, 'A');
      }
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / kRepaints;

    std::printf("  tier %d (%dx%d): full repaint %8.4f ms\n", tier, w, h, ms);
    // Generous, but far below the 16 ms (tier 2) / 254 ms (tier 3) we used to
    // take. A rebuild-per-write regression blows straight through this.
    CHECK(ms < 5.0);
  }
}

// Double-width glyphs must claim the cell to their right, exactly as
// screen.set does when it advances by getCharWidth().
void testWideGlyphLayout() {
  OcFont font;
  const std::filesystem::path hexPath(findOcEmuSrc() + "/font.hex");
  if (!std::filesystem::exists(hexPath)) {
    std::printf("[wide glyphs] SKIPPED (font.hex not found)\n");
    return;
  }
  CHECK(font.load(hexPath.string()));

  // Pick a codepoint the font actually stores as double width.
  std::uint32_t wide = 0;
  for (std::uint32_t c = 1; c < 0x300; ++c) {
    if (font.width(c) == 2) { wide = c; break; }
  }
  CHECK(wide != 0);

  OcScreen screen;
  screen.configure(20, 4, 3, 0);
  screen.setFont(&font);
  // Place the wide glyph at column 2 (1-based), i.e. index 1.
  screen.writeForTest(2, 1, wide);

  screen.markWideTails();
  const auto& cells = screen.cells();
  CHECK(!cells[1].continuation);            // the glyph's own cell
  CHECK(cells[2].continuation);             // its tail
  CHECK_EQ(cells[2].value, 0u);

  // A narrow glyph must not claim anything.
  screen.writeForTest(5, 2, 'A');
  screen.markWideTails();
  const auto& after = screen.cells();
  CHECK(!after[4].continuation);
  CHECK(!after[5].continuation);
}

// The pointer API real OpenComputers exposes. OCEmu's screen_sdl2 implements
// neither, so without it here every GUI program on the machine is blind.

// OCEmu's sound_card.lua is a stub that cannot even load without a real
// elsa.SDL audio device, so the card is ours. Check the guest-visible contract:
// the sample rate we advertise, that the component returns the four tables
// api/component.lua expects, and that speaker handles are usable.
void testSoundCard() {
  lua_State* L = luaL_newstate();
  luaL_openlibs(L);

  lua_pushcfunction(L, &ocemu::OcSound::luaComponent);
  lua_call(L, 0, 4);
  // (proxy, cec, mai, di): api/component.lua indexes mai[k] for every function
  // on the proxy, so a short return makes connecting the component throw.
  for (int i = 1; i <= 4; ++i) CHECK(lua_istable(L, -i));

  lua_getfield(L, -4, "type");
  CHECK_EQ(std::string(lua_tostring(L, -1)), "sound");
  lua_pop(L, 1);

  lua_getfield(L, -4, "getSampleRate");
  lua_pushinteger(L, 0);
  lua_call(L, 1, 1);
  CHECK_EQ(static_cast<int>(lua_tointeger(L, -1)), ocemu::OcSound::kSampleRate);
  lua_pop(L, 1);

  lua_getfield(L, -4, "openSpeaker");
  lua_pushinteger(L, 0);
  lua_pushinteger(L, 16667);
  lua_pushnumber(L, 440.0);
  lua_pushnumber(L, 1.0);
  lua_pushinteger(L, 0);
  lua_pushinteger(L, 0);
  lua_call(L, 6, 1);
  const int handle = static_cast<int>(lua_tointeger(L, -1));
  CHECK(handle >= 1);
  lua_pop(L, 1);

  // Pushing a sample must synthesise, not throw.
  lua_getfield(L, -4, "pushSample");
  lua_pushinteger(L, handle);
  lua_pushnumber(L, 0.5);
  CHECK(lua_pcall(L, 2, 0, 0) == 0);

  lua_close(L);
}

void testScreenPointer() {
  OcScreen screen;
  screen.configure(40, 10, 3, 0);

  // Off-screen by default.
  CHECK_EQ(screen.mouseX(), -1);

  // 1-based in, stored 0-based, reported back 1-based.
  screen.setMouseCell(7, 3);
  CHECK_EQ(screen.mouseX(), 7);
  CHECK_EQ(screen.mouseY(), 3);

  // Left/middle/right, 1-based like OpenComputers.
  CHECK(!screen.mouseButtonDown(1));
  screen.setMouseButton(1, true);
  screen.setMouseButton(3, true);
  CHECK(screen.mouseButtonDown(1));
  CHECK(!screen.mouseButtonDown(2));
  CHECK(screen.mouseButtonDown(3));
  screen.setMouseButton(1, false);
  CHECK(!screen.mouseButtonDown(1));
  // Out-of-range buttons are simply not down.
  CHECK(!screen.mouseButtonDown(0));
  CHECK(!screen.mouseButtonDown(9));

  screen.setMouseCell(-1, -1);
  CHECK_EQ(screen.mouseX(), -1);
}

void testTiers() {
  group("tier table");

  CHECK_EQ(tierSpec(1).cols, 50);
  CHECK_EQ(tierSpec(1).rows, 16);
  CHECK_EQ(tierSpec(1).depth, 1);

  CHECK_EQ(tierSpec(2).cols, 80);
  CHECK_EQ(tierSpec(2).rows, 25);
  CHECK_EQ(tierSpec(2).depth, 4);

  CHECK_EQ(tierSpec(3).cols, 160);
  CHECK_EQ(tierSpec(3).rows, 50);
  CHECK_EQ(tierSpec(3).depth, 8);

  // Out-of-range tiers clamp rather than corrupt the buffer.
  CHECK_EQ(tierSpec(0).cols, 50);
  CHECK_EQ(tierSpec(99).cols, 160);
  CHECK_EQ(clampTier(-5), 1);
  CHECK_EQ(clampTier(7), 3);
}

void testDisplayLimits() {
  group("display limits (GPU AND screen intersection)");

  for (int gpu = 1; gpu <= 3; ++gpu) {
    for (int scr = 1; scr <= 3; ++scr) {
      const TierSpec lim = resolveDisplayLimits(gpu, scr);
      const int wantTier = std::min(gpu, scr);
      CHECK_EQ(lim.cols, tierSpec(wantTier).cols);
      CHECK_EQ(lim.rows, tierSpec(wantTier).rows);
      CHECK_EQ(lim.depth, tierSpec(wantTier).depth);
    }
  }

  // Explicit spot checks of the documented cases.
  CHECK_EQ(resolveDisplayLimits(1, 3).cols, 50);   // cheap GPU, good monitor
  CHECK_EQ(resolveDisplayLimits(3, 1).depth, 1);   // good GPU, cheap monitor
  CHECK_EQ(resolveDisplayLimits(3, 3).cols, 160);
}

void testQuantisation() {
  group("colour quantisation");

  // 1 bit per channel collapses to pure black or pure white.
  for (int v = 0; v <= 255; ++v) {
    const int q = quantizeChannel(v, 1);
    CHECK(q == 0 || q == 255);
  }
  CHECK_EQ(quantizeChannel(0, 1), 0);
  CHECK_EQ(quantizeChannel(255, 1), 255);
  CHECK_EQ(quantizeChannel(127, 1), 0);
  CHECK_EQ(quantizeChannel(128, 1), 255);

  // 8 bit is a pass-through.
  for (int v = 0; v <= 255; v += 17) CHECK_EQ(quantizeChannel(v, 8), v);

  // 4 bit per channel means 16 levels, so at most 16 distinct values.
  std::vector<int> seen;
  for (int v = 0; v <= 255; ++v) seen.push_back(quantizeChannel(v, 4));
  std::sort(seen.begin(), seen.end());
  seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
  CHECK_EQ(seen.size(), std::size_t{16});
}

void testScreenBuffer() {
  group("screen buffer");

  ScreenBuffer sb;
  sb.applyLimits(2, 2);
  CHECK_EQ(sb.cols(), 80);
  CHECK_EQ(sb.rows(), 25);
  CHECK_EQ(sb.depth(), 4);

  // Requesting more than the tier allows is clamped and reported.
  const bool exact = sb.requestResolution(200, 60, 8);
  CHECK(!exact);
  CHECK(sb.requestWasClamped());
  // Limits here are tier 2 (80x25 @ 4-bit), so a 200x60 request clamps to them.
  CHECK_EQ(sb.cols(), 80);
  CHECK_EQ(sb.rows(), 25);
  CHECK_EQ(sb.depth(), 4);

  // Requesting within the limits is honoured exactly.
  const bool exact2 = sb.requestResolution(40, 10, 4);
  CHECK(exact2);
  CHECK(!sb.requestWasClamped());
  CHECK_EQ(sb.cols(), 40);
  CHECK_EQ(sb.rows(), 10);
  CHECK_EQ(sb.cellCount(), std::size_t{400});

  // Out-of-range cell access is ignored, not a crash.
  sb.setCell(999, 999, 'X', Rgb{255, 255, 255}, Rgb{0, 0, 0});
  sb.setCell(-1, -1, 'X', Rgb{255, 255, 255}, Rgb{0, 0, 0});
  CHECK_EQ(sb.cell(0, 0).glyph, static_cast<std::uint16_t>(' '));
}

void testScreenDepthChange() {
  group("screen depth downgrade keeps content");

  ScreenBuffer sb;
  sb.applyLimits(3, 3);  // 160x50 @ 8-bit
  CHECK_EQ(sb.depth(), 8);

  sb.setCursor(0, 0);
  sb.setPalette(Rgb{200, 100, 50}, Rgb{10, 20, 30});
  sb.writeUtf8("HELLO", 5);
  CHECK_EQ(sb.cell(0, 0).glyph, static_cast<std::uint16_t>('H'));

  // Downgrading to 1-bit must dim the colours, not blank the screen.
  sb.requestResolution(sb.cols(), sb.rows(), 1);
  CHECK_EQ(sb.depth(), 1);
  CHECK_EQ(sb.cell(0, 0).glyph, static_cast<std::uint16_t>('H'));
  const Rgb fg = sb.cell(0, 0).fg;
  CHECK((fg.r == 0 || fg.r == 255) && (fg.g == 0 || fg.g == 255) &&
        (fg.b == 0 || fg.b == 255));
}

void testScreenText() {
  group("screen text handling");

  ScreenBuffer sb;
  sb.applyLimits(1, 1);  // 50x16

  sb.setCursor(0, 0);
  sb.writeUtf8("ab\ncd", 5);
  CHECK_EQ(sb.cell(0, 0).glyph, static_cast<std::uint16_t>('a'));
  CHECK_EQ(sb.cell(1, 0).glyph, static_cast<std::uint16_t>('b'));
  CHECK_EQ(sb.cell(0, 1).glyph, static_cast<std::uint16_t>('c'));

  // Cursor wraps at the right edge.
  sb.setCursor(sb.cols() - 1, 2);
  sb.writeUtf8("XY", 2);
  CHECK_EQ(sb.cell(sb.cols() - 1, 2).glyph, static_cast<std::uint16_t>('X'));
  CHECK_EQ(sb.cell(0, 3).glyph, static_cast<std::uint16_t>('Y'));

  // Writing past the bottom scrolls the buffer up.
  sb.setCursor(0, sb.rows() - 1);
  sb.writeUtf8("\nZ", 2);
  CHECK_EQ(sb.cell(0, sb.rows() - 1).glyph, static_cast<std::uint16_t>('Z'));

  // UTF-8 multi-byte decoding: U+2588 FULL BLOCK is three bytes.
  std::size_t i = 0;
  const char block[] = "\xE2\x96\x88";
  CHECK_EQ(ScreenBuffer::decodeUtf8(block, 3, i), std::uint16_t{0x2588});
  CHECK_EQ(i, std::size_t{3});

  // Malformed byte must advance rather than loop forever.
  std::size_t j = 0;
  const char bad[] = "\xFF";
  ScreenBuffer::decodeUtf8(bad, 1, j);
  CHECK_EQ(j, std::size_t{1});
}

void testAllocator() {
  group("memory allocator accounting");

  CHECK_EQ(MemoryAllocator::blockSizeFor(1), std::size_t{32});
  CHECK_EQ(MemoryAllocator::blockSizeFor(16), std::size_t{32});
  CHECK_EQ(MemoryAllocator::blockSizeFor(17), std::size_t{64});
  CHECK_EQ(MemoryAllocator::blockSizeFor(48), std::size_t{64});

  MemoryAllocator a;
  a.setLimitBytes(1024);
  CHECK(!a.infinite());

  void* p = a.alloc(nullptr, 0, 48);  // 64-byte block
  CHECK(p != nullptr);
  CHECK_EQ(a.usedBytes(), std::size_t{64});
  CHECK(!a.outOfMemory());

  // Growing within the existing block must not charge anything new. The block
  // is 64 bytes including a 16-byte header, so any request <= 48 still fits.
  void* q = a.alloc(p, 48, 40);
  CHECK(q == p);
  CHECK_EQ(a.usedBytes(), std::size_t{64});

  // Freeing returns exactly what was taken.
  a.alloc(p, 40, 0);
  CHECK_EQ(a.usedBytes(), std::size_t{0});

  // Exceeding the cap is refused and flagged, and the old block survives.
  MemoryAllocator b;
  b.setLimitBytes(64);
  void* r = b.alloc(nullptr, 0, 200);  // needs a 256-byte block
  CHECK(r == nullptr);
  CHECK(b.outOfMemory());
  CHECK_EQ(b.refusedCount(), std::uint64_t{1});
  CHECK_EQ(b.usedBytes(), std::size_t{0});

  // A refused realloc leaves the original block intact and still accounted for.
  MemoryAllocator c;
  c.setLimitBytes(128);
  void* s = c.alloc(nullptr, 0, 16);  // 32-byte block
  CHECK(s != nullptr);
  CHECK_EQ(c.usedBytes(), std::size_t{32});
  void* t = c.alloc(s, 16, 4000);    // would need 4096
  CHECK(t == nullptr);
  CHECK(c.outOfMemory());
  CHECK_EQ(c.usedBytes(), std::size_t{32});  // untouched
  c.alloc(s, 16, 0);
  CHECK_EQ(c.usedBytes(), std::size_t{0});
}

void testAllocatorInfinite() {
  group("infinite memory bypasses the cap");

  MemoryAllocator inf;
  inf.setInfinite(true);
  CHECK(inf.infinite());
  CHECK_EQ(inf.limitBytes(), std::size_t{0});

  void* p = inf.alloc(nullptr, 0, 1000000);
  CHECK(p != nullptr);
  CHECK(!inf.outOfMemory());  // no refusal recorded
  CHECK(inf.usedBytes() > 1000000);
  inf.alloc(p, 1000000, 0);
  CHECK_EQ(inf.usedBytes(), std::size_t{0});

  // bytesFromKb maps the panel's -1 sentinel onto "no cap".
  CHECK_EQ(MemoryAllocator::bytesFromKb(-1), MemoryAllocator::kInfiniteLimit);
  CHECK_EQ(MemoryAllocator::bytesFromKb(0), MemoryAllocator::kInfiniteLimit);
  CHECK_EQ(MemoryAllocator::bytesFromKb(64), std::size_t{65536});
}

void testConfig() {
  group("config persistence");

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "ocemu_test_config.json";
  std::filesystem::remove(path);

  EmulatorConfig cfg;
  cfg.gpuTier = 3;
  cfg.screenTier = 1;
  cfg.internetCard = true;
  cfg.ramSizeKb = 2048;
  cfg.infiniteMemory = false;

  ConfigStore store(path);
  store.values() = cfg;
  CHECK(store.differsFromDisk());
  CHECK(store.save());
  CHECK(!store.differsFromDisk());
  CHECK(std::filesystem::exists(path));

  ConfigStore reloaded(path);
  CHECK(reloaded.load());
  CHECK_EQ(reloaded.values().gpuTier, 3);
  CHECK_EQ(reloaded.values().screenTier, 1);
  CHECK_EQ(reloaded.values().internetCard, true);
  CHECK_EQ(reloaded.values().ramSizeKb, 2048);
  CHECK_EQ(reloaded.values().effectiveRamKb(), 2048);

  // A config carrying the -1 sentinel must come back as "infinite".
  {
    std::ofstream out(path, std::ios::trunc);
    out << R"({"gpu_tier":1,"screen_tier":3,"internet_card":false,
               "infinite_memory":true,"ram_size_kb":-1})";
  }
  ConfigStore sentinel(path);
  CHECK(sentinel.load());
  CHECK(sentinel.values().infiniteMemory);
  CHECK_EQ(sentinel.values().effectiveRamKb(), -1);

  // A malformed file must degrade to defaults rather than throw.
  {
    std::ofstream out(path, std::ios::trunc);
    out << "{ this is not json ";
  }
  ConfigStore broken(path);
  CHECK(!broken.load());
  CHECK(!broken.lastError().empty());
  CHECK_EQ(broken.values().gpuTier, 2);

  // Out-of-range values are sanitised on the way in.
  {
    std::ofstream out(path, std::ios::trunc);
    out << R"({"gpu_tier":99,"screen_tier":0,"ram_size_kb":999999})";
  }
  ConfigStore wild(path);
  CHECK(wild.load());
  CHECK_EQ(wild.values().gpuTier, 3);
  CHECK_EQ(wild.values().screenTier, 1);
  CHECK_EQ(wild.values().ramSizeKb, EmulatorConfig::kMaxRamKb);

  std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
//  Lua machine: the OOM path is the reason these tests exist.
// ---------------------------------------------------------------------------

struct Rig {
  ScreenBuffer screen;
  MemoryAllocator allocator;
  LuaMachine machine;
  std::vector<std::string> alerts;
  std::vector<std::string> logs;
  bool rebootRequested = false;
  bool quitRequested = false;

  Rig(int ramKb, bool infinite, int gpuTier = 2) {
    screen.applyLimits(gpuTier, gpuTier);
    allocator.setInfinite(infinite);
    if (!infinite) allocator.setLimitBytes(MemoryAllocator::bytesFromKb(ramKb));

    HostInfo info;
    info.gpuTier = gpuTier;
    info.screenTier = gpuTier;
    info.ramLimitKb = infinite ? -1 : ramKb;
    info.internetEnabled = false;
    info.cols = screen.cols();
    info.rows = screen.rows();
    info.depth = screen.depth();
    machine.setHostInfo(info);

    LuaContext ctx;
    ctx.screen = &screen;
    ctx.allocator = &allocator;
    ctx.internet = nullptr;  // no network in tests
    ctx.alerts = &alerts;
    ctx.logLines = &logs;
    ctx.rebootRequested = &rebootRequested;
    ctx.quitRequested = &quitRequested;
    machine.attach(ctx);
  }
};

void testLuaBoot() {
  group("lua boot (healthy)");

  const std::string rom = writeTempRom("ocemu_test_boot.lua", R"(
host.log("hello from lua")
local c, r, d = screen.getResolution()
host.log(string.format("%dx%d@%d", c, r, d))
)");

  Rig rig(512, false);
  std::string err;
  CHECK(rig.machine.reboot(rom, &err));
  CHECK(rig.machine.state() == MachineState::Running);
  CHECK(err.empty());
  CHECK_EQ(rig.logs.size(), std::size_t{2});
  CHECK(rig.logs[0] == "hello from lua");
  CHECK(!rig.machine.lastError().empty() ? false : true);  // no error expected
  CHECK_EQ(rig.allocator.usedBytes() > 0, true);
}

void testLuaTierClamping() {
  group("lua sees the tier clamp");

  const std::string rom = writeTempRom("ocemu_test_clamp.lua", R"(
local exact, c, r, d = screen.requestResolution(500, 200, 8)
host.log(string.format("exact=%s %dx%d@%d", tostring(exact), c, r, d))
)");

  Rig rig(512, false, 1);  // tier 1 -> 50x16 @ 1-bit
  std::string err;
  CHECK(rig.machine.reboot(rom, &err));
  CHECK(rig.machine.state() == MachineState::Running);
  CHECK_EQ(rig.logs.size(), std::size_t{1});
  if (!rig.logs.empty()) {
    // exact must be false, and the grant must be the tier-1 geometry.
    CHECK(rig.logs[0].find("exact=false") != std::string::npos);
    CHECK(rig.logs[0].find("50x16@1") != std::string::npos);
  }
  CHECK_EQ(rig.screen.cols(), 50);
  CHECK_EQ(rig.screen.rows(), 16);
  CHECK_EQ(rig.screen.depth(), 1);
}

void testLuaOutOfMemory() {
  group("lua out-of-memory is detected (the key regression)");

  // Ask for far more than the budget allows.
  const std::string rom = writeTempRom("ocemu_test_oom.lua", R"(
local blocks = {}
for i = 1, 100000 do
  blocks[i] = string.rep("x", 8192)
end
host.log("allocated everything without running out of memory")
)");

  Rig rig(64, false);
  std::string err;
  const bool ok = rig.machine.reboot(rom, &err);

  // Must NOT report success.
  CHECK(!ok);
  CHECK(rig.machine.state() == MachineState::OutOfMemory);
  CHECK(!err.empty());
  CHECK(err.find("out of memory") != std::string::npos);
  CHECK(err.find("65536") != std::string::npos);  // mentions the cap

  // The allocator must have recorded the refusal.
  CHECK(rig.allocator.outOfMemory());
  CHECK(rig.allocator.refusedCount() > 0);
  CHECK(rig.allocator.usedBytes() <= rig.allocator.limitBytes());

  // The failed ROM must not have logged success.
  for (const auto& l : rig.logs) {
    CHECK(l.find("without running out of memory") == std::string::npos);
  }

  // And the machine must still be safe to poke afterwards.
  rig.machine.update(0.016);
  CHECK(rig.machine.state() == MachineState::OutOfMemory);
}

void testLuaInfiniteMemory() {
  group("infinite memory lets the same ROM finish");

  const std::string rom = writeTempRom("ocemu_test_inf.lua", R"(
local blocks = {}
for i = 1, 20000 do
  blocks[i] = string.rep("x", 4096)
end
host.log("done")
)");

  Rig rig(0, true);
  std::string err;
  CHECK(rig.machine.reboot(rom, &err));
  CHECK(rig.machine.state() == MachineState::Running);
  CHECK(rig.allocator.infinite());
  CHECK(!rig.allocator.outOfMemory());
  CHECK(rig.logs.size() == 1 && rig.logs[0] == "done");
  CHECK(rig.allocator.usedBytes() > 80u * 1024u * 1024u);
}

void testLuaErrorHandling() {
  group("lua errors are recovered, not fatal");

  const std::string rom = writeTempRom("ocemu_test_err.lua", R"(
error("intentional guest failure")
)");

  Rig rig(512, false);
  std::string err;
  // A plain Lua error is catchable, so the state stays up with the error recorded.
  rig.machine.reboot(rom, &err);
  CHECK(rig.machine.state() == MachineState::Running);
  CHECK(rig.machine.lastError().find("intentional guest failure") != std::string::npos);

  // A syntax error is reported the same way.
  const std::string bad = writeTempRom("ocemu_test_syntax.lua", "this is not lua ((");
  Rig rig2(512, false);
  rig2.machine.reboot(bad, &err);
  CHECK(rig2.machine.lastError().find("rom/boot.lua") != std::string::npos);
}

void testMissingRom() {
  group("missing ROM is reported cleanly");

  Rig rig(512, false);
  std::string err;
  CHECK(!rig.machine.reboot("/nonexistent/path/to/rom.lua", &err));
  CHECK(!err.empty());
  CHECK(err.find("cannot open ROM") != std::string::npos);
  CHECK(rig.machine.state() == MachineState::Stopped);
}

void testRebootIsClean() {
  group("reboot cycles cleanly");

  const std::string rom = writeTempRom("ocemu_test_cycle.lua", R"(
host.log("cycle")
)");

  Rig rig(512, false);
  std::string err;
  CHECK(rig.machine.reboot(rom, &err));
  const std::size_t usedAfterFirst = rig.allocator.usedBytes();
  CHECK(usedAfterFirst > 0);

  // Repeated reboots must not leak: usage should be identical each time.
  for (int i = 0; i < 5; ++i) {
    CHECK(rig.machine.reboot(rom, &err));
    CHECK(rig.machine.state() == MachineState::Running);
    CHECK_EQ(rig.allocator.usedBytes(), usedAfterFirst);
    CHECK_EQ(rig.allocator.refusedCount(), std::uint64_t{0});
  }

  // Rebooting after a failed boot must also recover.
  Rig rig2(64, false);
  const std::string oom = writeTempRom("ocemu_test_cycle_oom.lua", R"(
local b = {}
for i = 1, 100000 do b[i] = string.rep("x", 8192) end
)");
  CHECK(!rig2.machine.reboot(oom, &err));
  CHECK(rig2.machine.state() == MachineState::OutOfMemory);

  // Give it plenty of room and it must come back to life.
  rig2.allocator.setLimitBytes(MemoryAllocator::bytesFromKb(4096));
  rig2.allocator.clearOutOfMemory();
  CHECK(rig2.machine.reboot(rom, &err));
  CHECK(rig2.machine.state() == MachineState::Running);
  CHECK(rig2.machine.lastError().empty());
}

void testHostTick() {
  group("host.onTick hook");

  const std::string rom = writeTempRom("ocemu_test_tick.lua", R"(
local ticks = 0
function host.onTick(dt)
  ticks = ticks + 1
  host.log("tick " .. tostring(ticks))
end
)");

  Rig rig(512, false);
  std::string err;
  CHECK(rig.machine.reboot(rom, &err));

  for (int i = 0; i < 5; ++i) rig.machine.update(0.016);
  CHECK_EQ(rig.logs.size(), std::size_t{5});
  CHECK(rig.logs[0] == "tick 1");
  CHECK(rig.logs[4] == "tick 5");
}

// A glyph must never be drawn larger than the cell it lives in. This is the
// invariant behind the Tier 3 "text overlaps itself" bug, where the glyph was
// drawn at the atlas bake size (32px) into a cell the fit calculation had
// scaled down to ~14px.
void testCellGeometry() {
  group("screen cell geometry (glyph must fit its cell)");

  // Representative of a baked DejaVu Sans Mono at 32px: 32px tall, ~19px wide.
  constexpr float kBaseFontSize = 32.0f;
  constexpr float kBaseAdvance = 19.0f;
  const float baseW = kBaseAdvance;
  const float baseH = kBaseFontSize;

  for (int tier = 1; tier <= 3; ++tier) {
    const TierSpec lim = tierSpec(tier);
    const float gridW = baseW * static_cast<float>(lim.cols);
    const float gridH = baseH * static_cast<float>(lim.rows);

    // Reproduce App::drawScreen()'s fit for a few viewport sizes.
    for (const float avail : {1440.0f, 1920.0f, 900.0f, 600.0f}) {
      const float fitX = avail / std::max(1.0f, gridW);
      const float fitY = avail / std::max(1.0f, gridH);
      const float fit = std::min(1.0f, std::min(fitX, fitY));

      // Mirrors App::drawScreen(): height from the fit, width derived from it.
      const float cellH = std::max(1.0f, std::floor(baseH * fit));
      const float cellW = std::max(1.0f, std::floor(baseW * (cellH / baseH)));

      const GlyphAtlas::CellMetrics m =
          GlyphAtlas::fitToCell(cellW, cellH, kBaseFontSize, kBaseAdvance);

      // THE invariant: the drawn glyph is exactly one cell tall, so rows can
      // never bleed into each other.
      CHECK(m.fontSize > 0.0f);
      CHECK(std::fabs(m.fontSize - cellH) < 0.001f);
      CHECK(m.offsetY >= -0.001f && m.offsetY <= 0.001f);

      // Horizontally, flooring the cell width leaves the glyph up to one pixel
      // wider than the cell. That is benign: a monospace glyph has side
      // bearings, so it never actually touches the neighbour. The VERTICAL
      // invariant above is the strict one -- that is the bug that was fixed.
      CHECK(m.glyphW <= cellW + 1.0f);
      CHECK(m.offsetX >= -1.0f && m.offsetX <= 1.0f);
    }
  }

  // Explicitly pin the bug: a cell far smaller than the bake size must still
  // produce a font size matching the cell, not the bake size.
  // Cell width must be derived, not guessed: 32px tall -> 14px tall scales the
  // 19px advance to 19 * 14/32 = 8.3125.
  const float cellH = 14.0f;
  const float cellW = std::floor(kBaseAdvance * (cellH / kBaseFontSize));
  const GlyphAtlas::CellMetrics small =
      GlyphAtlas::fitToCell(cellW, cellH, kBaseFontSize, kBaseAdvance);

  // This is the exact Tier 3 case from the bug report: the atlas is baked at
  // 32px but the fit calculation produced a 14px cell.
  CHECK(std::fabs(small.fontSize - 14.0f) < 0.001f);
  CHECK(small.fontSize < baseH);
  CHECK(small.offsetY >= -0.001f && small.offsetY <= 0.001f);
  CHECK(small.glyphW <= cellW + 1.0f);
}

// The elsa host shim: OCEmu's entire SDL2 coupling is this one table, so its
// filesystem surface and its handler-registration metatable both need cover.
void testElsaHost() {
  group("elsa host shim");

  namespace fs = std::filesystem;
  const fs::path root = fs::temp_directory_path() / "ocemu_elsa_test";
  const fs::path sub = root / "subdir";
  const fs::path file = root / "hello.txt";
  const fs::path chunkFile = root / "chunk.lua";

  fs::remove_all(root);
  fs::create_directories(sub);
  {
    std::ofstream o(file, std::ios::binary);
    o << "OCEmu";
  }
  {
    std::ofstream o(chunkFile, std::ios::binary);
    o << "return marker\n";
  }

  // Paths are emitted as quoted Lua literals, and the body is one raw string
  // with a unique delimiter so no ")" inside it can terminate it early.
  auto q = [](const std::string& p) { return "\"" + p + "\""; };
  std::ostringstream rom;
  rom << "local root = " << q(root.string()) << "\n"
      << "local sub = " << q(sub.string()) << "\n"
      << "local file = " << q(file.string()) << "\n"
      << "local chunkFile = " << q(chunkFile.string()) << "\n"
      << R"LUA(
local function check(name, ok, extra)
  host.log((ok and "ok " or "FAIL ") .. name .. (extra and (" " .. tostring(extra)) or ""))
end

check("exists-file",  elsa.filesystem.exists(file))
check("exists-dir",   elsa.filesystem.isDirectory(sub))
check("exists-no",    not elsa.filesystem.exists(root .. "/nope.txt"))

local data, len = elsa.filesystem.read(file)
check("read",         data == "OCEmu" and len == 5, len)

check("write",        elsa.filesystem.write(root .. "/out.txt", "xyz") == true)
check("write-size",   elsa.filesystem.getSize(root .. "/out.txt") == 3)
check("getsize-miss", elsa.filesystem.getSize(root .. "/none") == nil)

local items = elsa.filesystem.getDirectoryItems(root)
table.sort(items)
check("listdir",      items[1] == "chunk.lua" and items[4] == "subdir",
      table.concat(items, ","))

check("mkdir",        elsa.filesystem.createDirectory(root .. "/made") == true)
check("mkdir-idem",   elsa.filesystem.isDirectory(root .. "/made"))

check("savedir-type", type(elsa.filesystem.getSaveDirectory()) == "string")
check("getos",        elsa.system.getOS() == "Linux")
check("geterror",     type(elsa.getError()) == "string")
check("timer",        type(elsa.timer.getTime()) == "number")
-- elsa.SDL must be a real false: __index would otherwise hand back a dispatcher
-- function, which is truthy, and main.lua would take the FFI path.
check("no-sdl",       elsa.SDL == false)
check("remove",       elsa.filesystem.remove(root .. "/out.txt") == true)
check("removed",      not elsa.filesystem.exists(root .. "/out.txt"))

-- The metatable: assigning elsa.draw must append to elsa.handlers.draw rather
-- than replace a field, which is how components register per-frame work.
local mt = getmetatable(elsa)
check("handlers-tbl", type(elsa.handlers) == "table")
check("has-mt",       mt ~= nil)
check("has-newindex", mt ~= nil and mt.__newindex ~= nil)
check("no-draw-field", rawget(elsa, "draw") == nil)
local before = elsa.handlers.draw and #elsa.handlers.draw or 0
elsa.draw = function() end
elsa.draw = function() end
local after = elsa.handlers.draw and #elsa.handlers.draw or 0
check("handler-append", after == before + 2, before .. "->" .. after)

-- A chunk compiled with a custom environment must see that environment's
-- globals: `return marker` yields 7 only if the env was installed.
check("load-missing", elsa.filesystem.load("does-not-exist.lua") == nil)
local env = { marker = 7 }
local fn = elsa.filesystem.load(chunkFile, "t", env)
check("load-env",     fn ~= nil and fn() == 7)
local fn2 = elsa.filesystem.load(chunkFile)
check("load-noenv",   fn2 ~= nil and fn2() == nil)
)LUA";

  OcEmuHost host;
  ScreenBuffer screen;
  MemoryAllocator allocator;
  LuaMachine machine;
  std::vector<std::string> alerts, logs;
  bool rebootReq = false, quitReq = false;

  allocator.setInfinite(true);
  screen.applyLimits(2, 2);

  HostInfo info;
  info.gpuTier = 2;
  info.screenTier = 2;
  info.ramLimitKb = -1;
  info.internetEnabled = false;
  info.cols = screen.cols();
  info.rows = screen.rows();
  info.depth = screen.depth();
  machine.setHostInfo(info);

  LuaContext ctx;
  ctx.screen = &screen;
  ctx.allocator = &allocator;
  ctx.internet = nullptr;
  ctx.alerts = &alerts;
  ctx.logLines = &logs;
  ctx.rebootRequested = &rebootReq;
  ctx.quitRequested = &quitReq;
  machine.attach(ctx);

  std::string err;
  const std::string bootRom = writeTempRom("ocemu_test_elsa_boot.lua", "-- bring the state up\n");

  // elsa can only be built once a live state exists, which is exactly why
  // LuaMachine::runChunk() exists: boot, inject the table, then run the guest.
  CHECK(machine.reboot(bootRom, &err));
  CHECK(machine.lastError().empty());

  host.install(machine.L(), "/nonexistent/ocsrc", root.string());
  const bool ran = machine.runChunk(rom.str(), "=elsa_test", &err);
  if (!ran) std::printf("  [diag] %s\n", err.c_str());
  CHECK(ran);
  CHECK(machine.lastError().empty());

  int failures = 0;
  for (const auto& l : logs) {
    if (l.rfind("FAIL", 0) == 0) {
      ++failures;
      std::printf("  elsa: %s\n", l.c_str());
    }
  }
  CHECK_EQ(failures, 0);
  // Every check above must actually have run, or the assertions are vacuous.
  CHECK(logs.size() >= 25);

  fs::remove_all(root);
}

// ---------------------------------------------------------------------------
//  Integration: boot the real OpenComputers OS through OCEmu's Lua core.
//
//  This is the end-to-end milestone: OCEmu's apis/*.lua and component/*.lua run
//  UNMODIFIED inside our embedded Lua 5.2, driven by our C++ `elsa` shim. The
//  only substitutions are the screen and keyboard, which we provide natively
//  through the elsa.filesystem.load interception point.
//
//  Skipped (reported, not failed) when OCEmu's sources are not present.
// ---------------------------------------------------------------------------
void testOpenOsBoot() {
  group("integration: boot OpenOS (OCEmu Lua core)");
  namespace fs = std::filesystem;

  const std::string ocSrc = findOcEmuSrc();
  const std::string ocData = findOcEmuData();

  std::error_code ec;
  if (!fs::exists(fs::path(ocSrc) / "main.lua", ec) ||
      !fs::exists(fs::path(ocSrc) / "apis", ec)) {
    std::printf("  SKIP: OCEmu sources not found at %s\n", ocSrc.c_str());
    std::printf("        set OCEMU_OC_SRC to override\n");
    return;
  }
  if (!fs::exists(fs::path(ocData), ec)) {
    std::printf("  SKIP: OCEmu machine data not found at %s\n", ocData.c_str());
    return;
  }

  OcEmuHost host;
  ScreenBuffer screen;
  MemoryAllocator allocator;
  LuaMachine machine;
  std::vector<std::string> alerts, logs;
  bool rebootReq = false, quitReq = false;

  // OpenOS plus OCEmu's own Lua state need real room; our allocator budget is a
  // host-side safety limit, not a machine spec.
  allocator.setInfinite(true);
  screen.applyLimits(3, 3);

  HostInfo info;
  info.gpuTier = 3;
  info.screenTier = 3;
  info.ramLimitKb = -1;
  info.internetEnabled = false;
  info.cols = 160;
  info.rows = 50;
  info.depth = 8;
  machine.setHostInfo(info);

  LuaContext ctx;
  ctx.screen = &screen;
  ctx.allocator = &allocator;
  ctx.internet = nullptr;
  ctx.alerts = &alerts;
  ctx.logLines = &logs;
  ctx.rebootRequested = &rebootReq;
  ctx.quitRequested = &quitReq;
  machine.attach(ctx);

  // Our tiers drive OCEmu's component list, which has to live in ocemu.cfg
  // because apis/component.lua reads it at load time.
  std::string cfgErr;
  if (!host.syncOcEmuConfig(ocData, 3, false, &cfgErr)) {
    std::printf("  FAIL: syncOcEmuConfig: %s\n", cfgErr.c_str());
    ++g_failures;
    return;
  }

  // The elsa table can only be built once a live lua_State exists, so the VM
  // must be booted first.
  std::string err;
  const std::string bootRom = writeTempRom("ocemu_test_openos_boot.lua", "-- openos harness\n");
  if (!machine.reboot(bootRom, &err)) {
    std::printf("  FAIL: VM boot: %s\n", err.c_str());
    ++g_failures;
    return;
  }

  host.install(machine.L(), ocSrc, ocData);
  host.setMachine(&machine);

  // main.lua unconditionally does `require("ffi")`, but every use of it sits
  // behind `if not machine.beep and elsa.SDL`, and we deliberately leave
  // elsa.SDL false. A stub satisfies the require without pulling in luaffifb.
  // The compat path is appended with a Lua long-bracket so no quoting in the
  // path can terminate the literal or the chunk.
  const std::string compatPath =
      (fs::path(OCEMU_ASSET_DIR) / "ocemu" / "component_compat.lua").string();
  const std::string ffiStub =
      std::string(R"LUA(
package.preload["ffi"] = function()
  return { os = "Linux", NULL = 0, new = function() return {} end,
           cast = function() return {} end, string = function(s) return tostring(s) end,
           typeof = function() return 0 end, C = function() end, copy = function(t) return t end }
end
_G.__ocemuCompatPath = [[)LUA") +
      compatPath + "]]\n";

  const std::string bootstrap =
      (fs::path(OCEMU_ASSET_DIR) / "ocemu" / "boot_openos.lua").string();
  const std::string bootSrc = "assert(loadfile(\"" + bootstrap + "\"))()";

  if (!machine.runChunk(ffiStub, "=ffi_stub", &err)) {
    std::printf("  FAIL: ffi stub: %s\n", err.c_str());
    ++g_failures;
    return;
  }
  if (!machine.runChunk(bootSrc, "=boot_openos", &err)) {
    for (const auto& l : logs) std::printf("    log: %s\n", l.c_str());
    std::printf("  FAIL: %s\n", err.c_str());
    ++g_failures;
    return;
  }

  // Validate the native screen against real OpenComputers semantics, reached the
  // way the guest reaches it.
  // Non-destructive integration assertions, run inside the guest sandbox where
  // `component` exists. These only READ: writing to the screen would race the
  // shell's own redraw and corrupt the boot screen we assert on.
  const std::string probe = R"LUA(
local RESULTS = {}
local function ck(name, ok, extra)
  RESULTS[#RESULTS+1] = (ok and "ok " or "FAIL ") .. tostring(name) ..
                        (extra ~= nil and (" [" .. tostring(extra) .. "]") or "")
end
-- Safe wrappers: a component method that raises must not abort the whole probe.
local function try(fn, ...)
  local res = table.pack(pcall(fn, ...))
  return res[1], table.unpack(res, 2, res.n)
end

local s = component.list("screen")()
ck("screen-found", s ~= nil, tostring(s))
if s then
  local okW, w, h = try(component.cecinvoke, s, "getResolution")
  ck("resolution-160x50", okW and w == 160 and h == 50, tostring(w) .. "x" .. tostring(h))
  local okM, mw, mh = try(component.cecinvoke, s, "maxResolution")
  ck("max-resolution-160x50", okM and mw == 160 and mh == 50, tostring(mw) .. "x" .. tostring(mh))

  -- get() must return the character as a single-character STRING as its first
  -- result. OpenOS's core/cursor.lua does
  --   table.pack(select(2, pcall(gpu.get, x, y)))
  -- and then feeds char[1] straight back into gpu.set, which type-checks for a
  -- string. Returning a number there broke every keystroke.
  local okG, c = try(component.cecinvoke, s, "get", 2, 2)
  ck("get-returns-string", okG and type(c) == "string", okG and type(c) or "error")
  -- Out-of-range reads must still yield a character rather than nil.
  local okO, oob = try(component.cecinvoke, s, "get", 9999, 9999)
  ck("get-out-of-range-safe", okO and type(oob) == "string", okO and type(oob) or "error")

  local okF, fg = try(component.cecinvoke, s, "getForeground")
  ck("getForeground-returns-number", okF and type(fg) == "number", okF and type(fg) or "error")
  local okB, _, isPal = try(component.cecinvoke, s, "getBackground")
  ck("getBackground-palette-flag", okB and (isPal == true or isPal == false),
     okB and tostring(isPal) or "error")
end

local kbd = component.list("keyboard")()
ck("keyboard-found", kbd ~= nil, tostring(kbd))
if kbd then
  local okT, kt = try(component.type, kbd)
  ck("keyboard-type", okT and kt == "keyboard", okT and tostring(kt) or "error")
end

-- A component proxy must resolve its methods: OpenOS's boot.lua calls
-- gpu.set(...) through one, and a nil method stops OpenOS dead.
do
  local okL, gaddr = pcall(function() return component.list("gpu", true)() end)
  ck("gpu-list-exact", okL and type(gaddr) == "string", okL and tostring(gaddr) or "error")
  if okL and type(gaddr) == "string" then
    local okP, g = pcall(component.proxy, gaddr)
    ck("gpu-proxy-built", okP and type(g) == "table", okP and type(g) or "error")
    if okP and type(g) == "table" then
      ck("gpu-proxy-set", type(g.set) == "function", type(g.set))
      ck("gpu-proxy-fill", type(g.fill) == "function", type(g.fill))
      ck("component-get-present", type(component.get) == "function", type(component.get))
    end
  end
end

-- component.<type> must resolve to a usable proxy. OpenOS's shell prints its
-- prompt only when io.stdin.tty and io.stdout.tty are both set, and those are
-- set from a bound gpu -- so a broken component.gpu means a silent shell.
do
  local g = component.gpu
  ck("component.gpu-non-nil", g ~= nil)
  if g ~= nil then
    ck("component.gpu-has-set", type(g.set) == "function", type(g.set))
    ck("component.gpu-has-get", type(g.get) == "function", type(g.get))
    local okS, screen = pcall(g.getScreen)
    ck("component.gpu-getScreen", okS and type(screen) == "string", okS and tostring(screen) or "error")
    local okR, w, h = pcall(g.getResolution)
    ck("component.gpu-getResolution", okR and type(w) == "number", okR and tostring(w) or "error")
  end
  local kbd = component.keyboard
  ck("component.keyboard-non-nil", kbd ~= nil)
end

-- Isolate the component-level file access the guest relies on.
do
  local ba = computer.getBootAddress()
  ck("bootAddress-string", type(ba) == "string", tostring(ba))
  if type(ba) == "string" then
    local okP, pxy = pcall(component.proxy, ba)
    ck("fs-proxy-built", okP and type(pxy) == "table", okP and type(pxy) or "error")
    if okP and type(pxy) == "table" then
      local okE, e = pcall(pxy.exists, "/lib/tty.lua")
      ck("proxy.exists-tty.lua", okE and e == true, okE and tostring(e) or tostring(e))
      local okC, h = pcall(pxy.open, "/lib/tty.lua", "r")
      ck("proxy.open-tty.lua", okC and type(h) == "number", okC and tostring(h) or tostring(h))
      local okI, hi = pcall(component.cecinvoke, ba, "open", "/lib/tty.lua", "r")
      ck("cecinvoke.open-tty.lua", okI and type(hi) == "number", okI and tostring(hi) or tostring(hi))
      local okR, d = pcall(pxy.read, h, 16)
      ck("proxy.read", okR and type(d) == "string", okR and type(d) or tostring(d))
    end
  end
end

-- require() must resolve OpenOS modules from /lib. boot.lua installs them with
-- `_G.package = package`, which only lands in the sandbox when _G is the sandbox.
do
end

for _, kind in ipairs({"gpu", "eeprom", "filesystem"}) do
  ck(kind .. "-found", component.list(kind)() ~= nil)
end

OCEMU_RESULTS = RESULTS
)LUA";

  // Drive the machine: elsa.update is the tick OCEmu's main.lua installs.
  // Tick generously: the screen must be captured AFTER the shell has had time
  // to react to the keystrokes.
  // OpenOS runs twelve boot scripts before the shell appears; give it room.
  for (int i = 0; i < 60000; ++i) {
    host.update(1.0 / 60.0);
  }

  std::printf("    machine state=%d lastError=%s\n", static_cast<int>(machine.state()),
              machine.lastError().c_str());

  // OpenOS booting means main.lua ran to completion and the machine thread is
  // alive; it does not mean a shell prompt has already been drawn.
  // The probe must run inside the guest sandbox, where `component` exists.
  const std::string probeResult =
      machine.runChunkIn(probe, "=openos_probe", "__ocemuGuestEnv", &err) ? "ok" : err;

  // Read the probe's results back out of the sandbox.
  if (probeResult == "ok") {
    machine.runChunk(R"LUA(
for _, line in ipairs(_G.__ocemuGuestEnv.OCEMU_RESULTS or {}) do host.log(line) end
)LUA", "=probe_results", &err);
  }

  if (probeResult != "ok") std::printf("    probe error: %s\n", probeResult.c_str());

  int probeFailures = 0;
  int probeChecks = 0;
  for (const auto& l : logs) {
    if (l.rfind("ok ", 0) == 0 || l.rfind("FAIL ", 0) == 0) {
      ++probeChecks;
      if (l.rfind("FAIL", 0) == 0) {
        ++probeFailures;
        std::printf("    !! %s\n", l.c_str());
      }
    }
  }
  std::printf("  %d probe checks, %d failed\n", probeChecks, probeFailures);

  CHECK(probeResult == "ok");
  CHECK(probeFailures == 0);
  // Guard against a vacuous pass: the probe must actually have run.
  CHECK(probeChecks >= 12);

  // The strongest end-to-end signal: read the screen back through the guest's
  // own component API and look for the strings OpenOS itself draws. These are
  // produced by the guest, so finding them proves the whole chain ran inside our
  // host (EEPROM -> BIOS -> init.lua -> package init -> filesystem -> boot
  // scripts -> shell) and that our native screen captured every glyph.
  // Type a command through the SAME entry point the SDL layer uses, using the
  // exact strings SDL_GetKeyName() reports. Going through OcEmuHost::pushKey
  // (not just the guest-facing pushChar API) is what caught the bug where
  // ordinary letters were queued with no character, so only space and enter --
  // the two keys whose character lives in the named-key table -- ever showed up.
  {
    // Printable characters arrive as their true character (SDL_TEXTINPUT),
    // named keys by name (SDL_KEYDOWN).
    for (const char* k : {"d", "i", "r"}) host.pushKey(k);
    host.pushKey("Return");
    for (int i = 0; i < 600; ++i) host.update(1.0 / 60.0);

    // Case must survive: this is what Shift and CapsLock produce.
    host.pushKey("A");
    for (int i = 0; i < 300; ++i) host.update(1.0 / 60.0);
  }

  // Read the screen back through the guest's OWN component API and look for the
  // strings OpenOS itself draws. Those are produced by the guest, so finding
  // them proves the whole chain ran inside our host (EEPROM -> BIOS ->
  // init.lua -> package init -> filesystem -> boot scripts -> shell) and that
  // our native screen captured every glyph correctly.
  //
  // gpu.get returns a one-character string per cell, and the screen is 1-based.
  {
    const std::string readScreen = R"LUA(
local s = component.list("screen")()
local w, h = component.cecinvoke(s, "getResolution")
local rows = {}
for y = 1, h do
  local line = {}
  for x = 1, w do
    local c = component.cecinvoke(s, "get", x, y)
    line[#line+1] = (type(c) == "string" and #c > 0) and c or " "
  end
  rows[y] = table.concat(line)
end
SCREEN_ROWS = rows
)LUA";
    if (machine.runChunkIn(readScreen, "=screen_read", "__ocemuGuestEnv", &err)) {
      machine.runChunk(R"LUA(
local ge = _G.__ocemuGuestEnv
if ge and ge.SCREEN_ROWS then
  for y, row in ipairs(ge.SCREEN_ROWS) do
    local t = row:gsub("%s+$", "")
    if #t > 0 then host.log(string.format("row%d %s", y, t)) end
  end
end
)LUA", "=screen_rows", &err);
    } else {
      std::printf("    screen read error: %s\n", err.c_str());
    }
  }

  bool sawBanner = false;
  bool sawPrompt = false;
  bool sawTyped = false;
  bool sawUpper = false;
  for (const auto& l : logs) {
    if (l.find("OpenOS 1.8.") != std::string::npos) sawBanner = true;
    if (l.find("/home #") != std::string::npos) sawPrompt = true;
    // The typed command must appear at the prompt.
    if (l.find("# dir") != std::string::npos) sawTyped = true;
    // The uppercase letter typed after Enter must be capitalised, not folded
    // to lowercase -- that regression is what made Shift/CapsLock look broken.
    if (l.find("# A") != std::string::npos) sawUpper = true;
  }
  // The end-to-end proof: OpenOS booted to an interactive shell and the shell
  // rendered our keystrokes onto the screen.
  //
  // Note the boot-progress lines ("Running boot scripts..." and so on) are NOT
  // asserted: once the shell starts, /etc/motd and the prompt scroll them off
  // the top of the screen, so their absence from the final frame says nothing.
  CHECK(sawBanner);
  CHECK(sawPrompt);
  CHECK(sawTyped);
  CHECK(sawUpper);

  bool noAlerts = true;
  for (const auto& a : alerts) {
    if (a.find("main.lua failed") != std::string::npos) noAlerts = false;
  }
  CHECK(noAlerts);
}

}  // namespace

int main() {
  std::printf("ocemu core tests\n================\n");

  testOcFont();
  testSoundCard();
  testScreenPointer();
  testWideGlyphLayout();
  testScreenWriteThroughput();
  testTiers();
  testDisplayLimits();
  testQuantisation();
  testScreenBuffer();
  testScreenDepthChange();
  testScreenText();
  testCellGeometry();
  testAllocator();
  testAllocatorInfinite();
  testConfig();
  testLuaBoot();
  testLuaTierClamping();
  testLuaOutOfMemory();
  testLuaInfiniteMemory();
  testLuaErrorHandling();
  testMissingRom();
  testRebootIsClean();
  testHostTick();
  testElsaHost();
  testOpenOsBoot();

  std::printf("\n================\n%d checks, %d failure(s)\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
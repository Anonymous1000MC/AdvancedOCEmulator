// ---------------------------------------------------------------------------
//  OcScreen -- a native OC `screen` component.
//
//  This is a port of the screen logic in OCEmu's component/screen_sdl2.lua. All
//  of that file's OpenComputers semantics -- the tier-2 16-colour palette, the
//  tier-3 grey ramp, `tier = math.min(depth, maxtier)`, palette-vs-literal
//  colour tracking, get/set/fill/copy -- are reproduced here. What is NOT ported
//  is its SDL window/texture/present path, which is replaced by a plain pixel
//  buffer the host uploads and draws with ImGui.
//
//  The buffer stores a character value plus foreground/background colour and
//  their palette indices per cell, exactly like the original, because
//  screen.get() has to report all five values.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <vector>

#include "ocemu/OcFont.hpp"
#include "ocemu/Tiers.hpp"

struct lua_State;

namespace ocemu {

class OcScreen {
 public:
  struct Cell {
    std::uint32_t value = ' ';  // character shown
    std::uint32_t fg = 0xFFFFFF;
    std::uint32_t bg = 0x000000;
    std::int32_t fgPalette = -1;  // -1 == literal colour, not a palette index
    std::int32_t bgPalette = -1;
    // True when this cell is the tail of a double-width glyph that started in the
    // cell to its left. OC's font has 1- and 2-cell glyphs (getCharWidth() is
    // #hexdigits/32), and screen.set advances by the glyph's width, so the
    // renderer must skip these instead of drawing an empty cell over half a
    // character.
    bool continuation = false;
  };

  OcScreen() = default;

  // `maxTier` is the card's tier (1..3). OCEmu's screen component speaks in
  // tiers, not bit depths; gpu.lua converts with depthTbl/rdepthTbl.
  // `requestedTier` is the panel's current setting (0 = follow the card).
  void configure(int maxWidth, int maxHeight, int maxTier, int requestedTier);

  [[nodiscard]] int width() const { return width_; }
  [[nodiscard]] int height() const { return height_; }
  [[nodiscard]] int maxWidth() const { return maxWidth_; }
  [[nodiscard]] int maxHeight() const { return maxHeight_; }
  [[nodiscard]] int tier() const { return tier_; }
  [[nodiscard]] int maxTier() const { return maxTier_; }

  // The renderer reads the cell array directly and resolves colours per frame.
  //
  // There used to be a second, palette-resolved array rebuilt after every single
  // write, which made writing N cells cost O(N * cells): a full 160x50 repaint
  // took 254 ms. Upstream screen_sdl2 keeps per-cell colour and resolves at
  // blit time, so a write is O(1) and resolution happens once per drawn frame --
  // the same total work, but paid by the renderer instead of the guest.
  [[nodiscard]] const std::vector<Cell>& cells() const { return cells_; }

  // Resolves a cell's stored colour (literal or palette index) to RGB.
  [[nodiscard]] Rgb resolveColor(std::uint32_t color, std::int32_t paletteIndex) const;
  [[nodiscard]] std::uint64_t revision() const { return revision_; }

  // The most recently constructed screen, for the host renderer to reach.
  [[nodiscard]] static OcScreen* active() { return s_active; }

  // Writes one cell exactly as a component `set` call would. Exposed for the
  // render benchmark in the tests; not part of the component API.
  void writeForTest(int x, int y, std::uint32_t value);
  // Exercises the same string path as the component's `set`, 1-based like it.
  void writeStringForTest(int x, int y, const char* text, bool vertical = false);
  // The single-cell write used by lSet, exposed for tests.
  static void put(OcScreen* s, int cx, int cy, std::uint32_t v);


  // Claims the trailing cell of every double-width glyph on screen. Public so the
  // renderer (and tests) can re-run it after a bulk edit.
  void markWideTails();

  // Pointer state, in screen cells. Real OpenComputers exposes
  // screen.getMousePosition()/getMouseButton(); OCEmu's own screen_sdl2 does
  // not implement them at all, so without these here every GUI program on the
  // machine is blind.
  void setMouseCell(int x, int y) { mouseX_ = x; mouseY_ = y; }
  void setMouseButton(int button, bool down);
  [[nodiscard]] int mouseX() const { return mouseX_; }
  [[nodiscard]] int mouseY() const { return mouseY_; }
  [[nodiscard]] bool mouseButtonDown(int button) const;

  // OC's bitmap font, used to decide glyph advances when writing. Optional: with
  // no font every glyph is treated as one cell wide, which is what we did before.
  void setFont(const OcFont* font) { font_ = font; }
  [[nodiscard]] const OcFont* font() const { return font_; }

  // Builds the OC component: returns (obj, cec, mai, di) on the Lua stack.
  // Register with lua_pushcfunction and pass the constructor arguments the same
  // way apis/component.lua would.
  static int luaComponent(lua_State* L);

 private:
  // Marks the screen dirty after a mutation.
  void touch();

  void clampPalette();
  [[nodiscard]] std::uint32_t resolve(std::uint32_t color, std::int32_t paletteIndex) const;

  static OcScreen* self(lua_State* L);

  // --- OC method implementations -----------------------------------------
  static int lGetForeground(lua_State* L);
  static int lSetForeground(lua_State* L);
  static int lGetBackground(lua_State* L);
  static int lSetBackground(lua_State* L);
  static int lGetDepth(lua_State* L);
  static int lSetDepth(lua_State* L);
  static int lMaxDepth(lua_State* L);
  static int lGetResolution(lua_State* L);
  static int lSetResolution(lua_State* L);
  static int lMaxResolution(lua_State* L);
  static int lGetPaletteColor(lua_State* L);
  static int lSetPaletteColor(lua_State* L);
  static int lGet(lua_State* L);
  static int lSet(lua_State* L);
  static int lFill(lua_State* L);
  static int lCopy(lua_State* L);
  static int lBitblt(lua_State* L);
  static int lClear(lua_State* L);
  static int lIsOn(lua_State* L);
  static int lTurnOn(lua_State* L);
  static int lTurnOff(lua_State* L);
  static int lGetAspectRatio(lua_State* L);
  static int lGetKeyboards(lua_State* L);
  static int lGetMousePosition(lua_State* L);
  static int lGetMouseButton(lua_State* L);
  static int lSetPrecise(lua_State* L);
  static int lIsPrecise(lua_State* L);
  static int lSetTouchModeInverted(lua_State* L);
  static int lIsTouchModeInverted(lua_State* L);

  int maxWidth_ = 80;
  int maxHeight_ = 25;
  int maxTier_ = 3;
  int width_ = 80;
  int height_ = 25;
  int tier_ = 3;
  bool on_ = true;
  bool precise_ = false;
  bool touchInverted_ = false;

  std::uint32_t fgColor_ = 0xFFFFFF;
  std::uint32_t bgColor_ = 0x000000;
  std::int32_t fgPalette_ = -1;
  std::int32_t bgPalette_ = -1;

  std::uint32_t palette_[16] = {0};
  std::vector<Cell> cells_;
  const OcFont* font_ = nullptr;
  // Pointer position in cells, or (-1, -1) when the cursor is outside the screen.
  int mouseX_ = -1;
  int mouseY_ = -1;
  bool mouseDown_[3] = {false, false, false};
  std::uint64_t revision_ = 1;

  static OcScreen* s_active;
};

}  // namespace ocemu
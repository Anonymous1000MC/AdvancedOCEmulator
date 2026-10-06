#include "ocemu/OcScreen.hpp"
#include "ocemu/ScreenBuffer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace ocemu {
namespace {

// OCEmu's screen component speaks in TIERS (1..3), not bit depths: gpu.lua owns
// the conversion (`depthTbl = {1,4,8}`, `rdepthTbl = {1,[4]=2,[8]=3}`) and calls
// screen.getDepth()/setDepth()/maxDepth() expecting a tier back.

// Palette entries exposed at a given tier.
int paletteSize(int tier) { return tier == 1 ? 1 : (tier == 2 ? 16 : 4096); }

}  // namespace

void OcScreen::configure(int maxWidth, int maxHeight, int maxTier, int requestedTier) {
  maxWidth_ = std::max(1, maxWidth);
  maxHeight_ = std::max(1, maxHeight);
  maxTier_ = std::clamp(maxTier, 1, 3);

  width_ = maxWidth_;
  height_ = maxHeight_;
  tier_ = (requestedTier > 0) ? std::clamp(requestedTier, 1, maxTier_) : maxTier_;

  // Tier 2 has a fixed 16-colour palette; tier 3 is a 16-entry grey ramp used as
  // the base of a 4096-colour space. Both are taken verbatim from
  // screen_sdl2.lua so colours match what the guest expects.
  static const std::uint32_t kTier2[16] = {
      0xFFFFFF, 0xFFCC33, 0xCC66CC, 0x6699FF, 0xFFFF33, 0x33CC33, 0xFF6699,
      0x333333, 0xCCCCCC, 0x336699, 0x9933CC, 0x333399, 0x663300, 0x336600,
      0xFF3333, 0x000000};
  for (int i = 0; i < 16; ++i) palette_[i] = (tier_ == 3) ? ((i + 1) * 0x0F0F0Fu) : kTier2[i];
  if (tier_ == 1) palette_[0] = 0xFFFFFF;

  clampPalette();
  cells_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), Cell{});
  touch();
}

void OcScreen::clampPalette() {
  // Tier 1 exposes a single entry, tier 2 sixteen, tier 3 the full 4096.
  const int limit = paletteSize(tier_);
  if (fgPalette_ >= limit) {
    fgPalette_ = (limit > 0) ? fgPalette_ % limit : -1;
    if (fgPalette_ < 0) fgPalette_ = -1;
  }
  if (bgPalette_ >= limit) {
    bgPalette_ = (limit > 0) ? bgPalette_ % limit : -1;
    if (bgPalette_ < 0) bgPalette_ = -1;
  }
}

std::uint32_t OcScreen::resolve(std::uint32_t color, std::int32_t paletteIndex) const {
  if (paletteIndex < 0) return color;
  const int limit = paletteSize(tier_);
  if (paletteIndex >= limit) return color;
  if (paletteIndex < 16) return palette_[paletteIndex];
  // Beyond the 16 base entries, synthesise from the grey ramp tier 3 uses:
  // entry n is (n % 16 + 1) * 0x0F0F0F.
  return static_cast<std::uint32_t>((paletteIndex % 16 + 1) * 0x0F0F0F);
}

void OcScreen::touch() { ++revision_; }

// OC's font is variable width, and screen.set advances x by the glyph's width,
// so a double-width glyph claims the cell to its right. Mark those cells so the
// renderer leaves them alone instead of painting over half the character.
void OcScreen::markWideTails() {
  if (font_ == nullptr || !font_->loaded()) return;
  for (int y = 0; y < height_; ++y) {
    for (int x = 0; x < width_; ++x) {
      Cell& c = cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                         static_cast<std::size_t>(x)];
      if (c.continuation) continue;  // already claimed by the glyph to its left
      if (font_->width(c.value) != 2) continue;
      if (x + 1 >= width_) continue;
      Cell& next = cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) +
                           static_cast<std::size_t>(x + 1)];
      next.continuation = true;
      next.value = 0;
    }
  }
}

Rgb OcScreen::resolveColor(std::uint32_t color, std::int32_t paletteIndex) const {
  return Rgb{static_cast<std::uint8_t>((resolve(color, paletteIndex) >> 16) & 0xFF),
             static_cast<std::uint8_t>((resolve(color, paletteIndex) >> 8) & 0xFF),
             static_cast<std::uint8_t>(resolve(color, paletteIndex) & 0xFF)};
}

// ---------------------------------------------------------------------------
//  Lua bindings
// ---------------------------------------------------------------------------

namespace {
// The single active screen is stored in the Lua registry as a light userdata
// pointer under this key, so the method thunks can find their state.
const char kRegistryTag = 0;
}  // namespace

OcScreen* OcScreen::s_active = nullptr;

namespace {
// Non-raising argument reader.
//
// component.cecinvoke calls these methods DIRECTLY -- there is no pcall in the
// way -- so a luaL_checkinteger failure would unwind through the host dispatch
// and take the whole machine down. OCEmu's own Lua methods were permissive (they
// just used the parameters), so be permissive here too: accept numbers and
// numeric strings, and fall back to a default rather than raising.
int argInt(lua_State* L, int idx, int def = 0) {
  const int t = lua_type(L, idx);
  if (t == LUA_TNUMBER) return static_cast<int>(lua_tointeger(L, idx));
  if (t == LUA_TSTRING) {
    std::size_t len = 0;
    const char* str = lua_tolstring(L, idx, &len);
    char* end = nullptr;
    const long v = std::strtol(str, &end, 10);
    if (end != str && end != nullptr) return static_cast<int>(v);
  }
  return def;
}
}  // namespace



OcScreen* OcScreen::self(lua_State* L) {
  lua_rawgetp(L, LUA_REGISTRYINDEX, &kRegistryTag);
  auto* screen = static_cast<OcScreen*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return screen;
}

int OcScreen::lGetForeground(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  if (s->fgPalette_ >= 0) {
    lua_pushinteger(L, s->fgPalette_);
    lua_pushboolean(L, 1);
  } else {
    lua_pushinteger(L, static_cast<lua_Integer>(s->fgColor_));
    lua_pushboolean(L, 0);
  }
  return 2;
}

int OcScreen::lSetForeground(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  const auto value = static_cast<std::uint32_t>(argInt(L, 1));
  const bool usePalette = lua_toboolean(L, 2) != 0;

  const std::uint32_t oldColor = s->fgColor_;
  const std::int32_t oldIndex = s->fgPalette_;

  if (usePalette) {
    s->fgPalette_ = static_cast<std::int32_t>(value);
    s->fgColor_ = s->fgPalette_ < 16
                     ? s->palette_[std::clamp<std::int32_t>(s->fgPalette_, 0, 15)]
                     : s->resolve(0, s->fgPalette_);
  } else {
    s->fgColor_ = value;
    s->fgPalette_ = -1;
  }
  s->clampPalette();

  lua_pushinteger(L, static_cast<lua_Integer>(oldColor));
  // The old palette index must be `false`, never -1, when there was no palette.
  //
  // OCEmu's reference is `scrrfp = palette and value`, so the "no palette" case
  // is literally false. OpenOS's vt100 does `fg(pal or rgb, not not pal)` on the
  // result: with -1, `pal or rgb` yields -1 and `not not pal` yields true, so
  // the guest calls gpu.setForeground(-1, true) and gpu.lua rejects it with
  // "invalid palette index".
  if (oldIndex < 0) {
    lua_pushboolean(L, 0);
  } else {
    lua_pushinteger(L, oldIndex);
  }
  return 2;
}

int OcScreen::lGetBackground(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  if (s->bgPalette_ >= 0) {
    lua_pushinteger(L, s->bgPalette_);
    lua_pushboolean(L, 1);
  } else {
    lua_pushinteger(L, static_cast<lua_Integer>(s->bgColor_));
    lua_pushboolean(L, 0);
  }
  return 2;
}

int OcScreen::lSetBackground(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  const auto value = static_cast<std::uint32_t>(argInt(L, 1));
  const bool usePalette = lua_toboolean(L, 2) != 0;

  const std::uint32_t oldColor = s->bgColor_;
  const std::int32_t oldIndex = s->bgPalette_;

  if (usePalette) {
    s->bgPalette_ = static_cast<std::int32_t>(value);
    s->bgColor_ = s->bgPalette_ < 16
                     ? s->palette_[std::clamp<std::int32_t>(s->bgPalette_, 0, 15)]
                     : s->resolve(0, s->bgPalette_);
  } else {
    s->bgColor_ = value;
    s->bgPalette_ = -1;
  }
  s->clampPalette();

  lua_pushinteger(L, static_cast<lua_Integer>(oldColor));
  // The old palette index must be `false`, never -1, when there was no palette.
  //
  // OCEmu's reference is `scrrfp = palette and value`, so the "no palette" case
  // is literally false. OpenOS's vt100 does `fg(pal or rgb, not not pal)` on the
  // result: with -1, `pal or rgb` yields -1 and `not not pal` yields true, so
  // the guest calls gpu.setForeground(-1, true) and gpu.lua rejects it with
  // "invalid palette index".
  if (oldIndex < 0) {
    lua_pushboolean(L, 0);
  } else {
    lua_pushinteger(L, oldIndex);
  }
  return 2;
}

int OcScreen::lGetDepth(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  lua_pushinteger(L, s->tier_);
  return 1;
}

int OcScreen::lSetDepth(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  const int tier = argInt(L, 1, s->maxTier_);
  const int old = s->tier_;
  s->tier_ = std::clamp(std::min(tier, s->maxTier_), 1, s->maxTier_);
  s->clampPalette();
  s->touch();
  lua_pushinteger(L, old);
  return 1;
}

int OcScreen::lMaxDepth(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  lua_pushinteger(L, s->maxTier_);
  return 1;
}

int OcScreen::lGetResolution(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  lua_pushinteger(L, s->width_);
  lua_pushinteger(L, s->height_);
  return 2;
}

int OcScreen::lSetResolution(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  const int nw = argInt(L, 1);
  const int nh = argInt(L, 2);

  const int cw = std::clamp(nw, 1, s->maxWidth_);
  const int ch = std::clamp(nh, 1, s->maxHeight_);
  const bool changed = (cw != s->width_ || ch != s->height_);
  if (changed) {
    s->width_ = cw;
    s->height_ = ch;
    s->cells_.assign(static_cast<std::size_t>(cw) * static_cast<std::size_t>(ch), Cell{});
    s->touch();
  }
  lua_pushboolean(L, changed);
  return 1;
}

int OcScreen::lMaxResolution(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  lua_pushinteger(L, s->maxWidth_);
  lua_pushinteger(L, s->maxHeight_);
  return 2;
}

int OcScreen::lGetPaletteColor(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  const auto index = argInt(L, 1);
  const int limit = paletteSize(s->tier_);
  if (index < 0 || index >= limit) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, static_cast<lua_Integer>(s->resolve(0, index)));
  return 1;
}

int OcScreen::lSetPaletteColor(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  const auto index = argInt(L, 1);
  const auto color = static_cast<std::uint32_t>(argInt(L, 2));
  if (index < 0 || index >= 16) {
    lua_pushnil(L);
    return 1;
  }
  const std::uint32_t old = s->palette_[index];
  s->palette_[index] = color;
  s->touch();
  lua_pushinteger(L, static_cast<lua_Integer>(old));
  return 1;
}

void OcScreen::writeForTest(int x, int y, std::uint32_t value) {
  // 1-based, exactly like the component's `set`: OC's screen coordinates start
  // at 1, and lSet subtracts one before indexing.
  const int cx = x - 1;
  const int cy = y - 1;
  if (cx < 0 || cy < 0 || cx >= width_ || cy >= height_) return;
  Cell& c = cells_[static_cast<std::size_t>(cy) * static_cast<std::size_t>(width_) +
                   static_cast<std::size_t>(cx)];
  c.value = value;
  c.fg = fgColor_;
  c.bg = bgColor_;
  c.fgPalette = fgPalette_;
  c.bgPalette = bgPalette_;
  c.continuation = false;

  // Maintain the wide-glyph tail in O(1) instead of rescanning the whole
  // screen. This used to call markWideTails() per cell written, which is O(cells)
  // per character: drawing a 160-column line scanned 8000 cells 160 times, and
  // anything animated redrew the screen every frame. That dominated frame time.
  //
  // The pair is self-consistent: a wide glyph claims the cell to its right, and
  // a narrow one releases it. That also clears a stale tail left behind when a
  // wide glyph is overwritten.
  if (cx + 1 < width_) {
    Cell& next = cells_[static_cast<std::size_t>(cy) *
                             static_cast<std::size_t>(width_) +
                         static_cast<std::size_t>(cx + 1)];
    if (font_ != nullptr && font_->loaded() && font_->isWide(value)) {
      next.value = 0;
      next.fg = fgColor_;
      next.bg = bgColor_;
      next.fgPalette = fgPalette_;
      next.bgPalette = bgPalette_;
      next.continuation = true;
    } else {
      next.continuation = false;
    }
  }
  touch();
}

int OcScreen::lGet(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  // OCEmu's screen is 1-based (screen_sdl2 checks `x >= 1` and indexes
  // screen.txt[y][x]), so shift down to our 0-based storage.
  const int x = argInt(L, 1) - 1;
  const int y = argInt(L, 2) - 1;
  if (x < 0 || y < 0 || x >= s->width_ || y >= s->height_) {
    // Upstream leaves untouched/out-of-range reads as nil; OpenOS's cursor
    // relies on getting a character back, so report a space.
    lua_pushstring(L, " ");
    lua_pushinteger(L, 0);
    lua_pushinteger(L, 0);
    return 3;
  }
  const Cell& c = s->cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(s->width_) +
                               static_cast<std::size_t>(x)];
  // The character must come back as a single-character STRING: OpenOS's
  // core/cursor.lua does `table.pack(select(2, pcall(gpu.get, x, y)))` and then
  // passes `char[1]` to gpu.set, which type-checks for a string. Returning the
  // raw byte value here made every keystroke fail with
  // "bad arguments #3 (string expected, got number)".
  const char ch = c.value == 0 ? ' ' : static_cast<char>(c.value);
  lua_pushlstring(L, &ch, 1);
  lua_pushinteger(L, static_cast<lua_Integer>(c.fg));
  lua_pushinteger(L, static_cast<lua_Integer>(c.bg));
  if (c.fgPalette >= 0) lua_pushinteger(L, c.fgPalette); else lua_pushnil(L);
  if (c.bgPalette >= 0) lua_pushinteger(L, c.bgPalette); else lua_pushnil(L);
  return 5;
}
int OcScreen::lSet(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  // 1-based, matching screen_sdl2.
  const int x = argInt(L, 1) - 1;
  const int y = argInt(L, 2) - 1;
  const bool vertical = lua_toboolean(L, 4) != 0;

  // gpu.lua's set() passes `value` as a *string* (compCheckArg(3, value,
  // "string")) and only the raw component API passes a number, so accept both.
  // Treating the string as an integer stored a 0 for every glyph, which is why
  // OpenOS drew nothing at all.

  if (lua_type(L, 3) == LUA_TSTRING) {
    // utf8.next semantics: walk codepoints, laying them out along one axis.
    size_t len = 0;
    const char* text = lua_tolstring(L, 3, &len);
    if (text != nullptr) {
      int cx = x;
      int cy = y;
      for (size_t i = 0; i < len;) {
        // Decode properly. The old loop computed each sequence's length but
        // stored only the lead byte, so every non-ASCII character rendered as
        // Latin-1 mojibake: U+2580 (E2 96 80) became 0xE2, i.e. "a-circumflex".
        // That is why OpenOS's box-drawing borders came out as rows of â.
        const std::uint16_t cp = ScreenBuffer::decodeUtf8(text, len, i);
        put(s, cx, cy, cp);
        // Real OpenComputers lays a double-width glyph over two cells and
        // continues after it, so advance by the glyph's width, not by one.
        const int step =
            (s->font_ != nullptr && s->font_->loaded()) ? s->font_->width(cp) : 1;
        if (vertical) {
          cy += (step > 0 ? step : 1);
          if (cy >= s->height_) break;
        } else {
          cx += (step > 0 ? step : 1);
          if (cx >= s->width_) break;
        }
      }
    }
  } else {
    auto value = static_cast<std::uint32_t>(argInt(L, 3));
    if (vertical) {
      for (int i = 0; value != 0 && i < 256; ++i) {
        put(s, x, y + i, value & 0xFF);
        value >>= 8;
      }
    } else {
      for (int i = 0; value != 0 && i < 256; ++i) {
        put(s, x + i, y, value & 0xFF);
        value >>= 8;
      }
    }
  }

  s->markWideTails();
  s->touch();
  return 0;
}

int OcScreen::lFill(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  // 1-based origin, like screen_sdl2 and every other OC screen call.
  const int x = argInt(L, 1) - 1;
  const int y = argInt(L, 2) - 1;
  const int w = argInt(L, 3);
  const int h = argInt(L, 4);
  // fill's last argument is a single-character string.
  std::uint32_t value = ' ';
  if (lua_type(L, 5) == LUA_TSTRING) {
    size_t len = 0;
    const char* text = lua_tolstring(L, 5, &len);
    if (text != nullptr && len > 0) value = static_cast<unsigned char>(text[0]);
  } else {
    value = static_cast<std::uint32_t>(argInt(L, 5));
  }

  for (int yy = std::max(0, y); yy < std::min(s->height_, y + h); ++yy) {
    for (int xx = std::max(0, x); xx < std::min(s->width_, x + w); ++xx) {
      Cell& c = s->cells_[static_cast<std::size_t>(yy) * static_cast<std::size_t>(s->width_) +
                             static_cast<std::size_t>(xx)];
      c.value = value;
      c.fg = s->fgColor_;
      c.bg = s->bgColor_;
      c.fgPalette = s->fgPalette_;
      c.bgPalette = s->bgPalette_;
    }
  }
  s->touch();
  lua_pushboolean(L, 1);
  return 1;
}

int OcScreen::lCopy(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  const int x = argInt(L, 1);
  const int y = argInt(L, 2);
  const int w = argInt(L, 3);
  const int h = argInt(L, 4);
  const int tx = argInt(L, 5);
  const int ty = argInt(L, 6);

  // Snapshot first: OC allows overlapping regions.
  std::vector<Cell> region;
  region.reserve(static_cast<std::size_t>(std::max(0, w)) * static_cast<std::size_t>(std::max(0, h)));
  for (int yy = 0; yy < h; ++yy) {
    for (int xx = 0; xx < w; ++xx) {
      const int sx = x + xx;
      const int sy = y + yy;
      if (sx < 0 || sy < 0 || sx >= s->width_ || sy >= s->height_) {
        region.push_back(Cell{});
      } else {
        region.push_back(s->cells_[static_cast<std::size_t>(sy) *
                                         static_cast<std::size_t>(s->width_) +
                                     static_cast<std::size_t>(sx)]);
      }
    }
  }

  for (int yy = 0; yy < h; ++yy) {
    for (int xx = 0; xx < w; ++xx) {
      const int dx = tx + xx;
      const int dy = ty + yy;
      if (dx < 0 || dy < 0 || dx >= s->width_ || dy >= s->height_) continue;
      s->cells_[static_cast<std::size_t>(dy) * static_cast<std::size_t>(s->width_) +
                 static_cast<std::size_t>(dx)] =
          region[static_cast<std::size_t>(yy) * static_cast<std::size_t>(w) +
                 static_cast<std::size_t>(xx)];
    }
  }
  s->touch();
  lua_pushboolean(L, 1);
  return 1;
}

// bitblt(buf, col, row, w, h, fromRow, fromCol) -- buf is a GPU buffer object
// with :bufferGet(x, y). Only what OpenOS's terminal needs is honoured.
int OcScreen::lBitblt(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  return 0;
}

int OcScreen::lClear(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  std::fill(s->cells_.begin(), s->cells_.end(), Cell{});
  s->touch();
  lua_pushboolean(L, 1);
  return 1;
}

int OcScreen::lIsOn(lua_State* L) {
  auto* s = self(L);
  lua_pushboolean(L, s != nullptr && s->on_);
  return 1;
}

int OcScreen::lTurnOn(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  const bool was = s->on_;
  s->on_ = true;
  lua_pushboolean(L, !was);
  return 1;
}

int OcScreen::lTurnOff(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  const bool was = s->on_;
  s->on_ = false;
  lua_pushboolean(L, was);
  return 1;
}

int OcScreen::lGetAspectRatio(lua_State* L) {
  lua_pushnumber(L, 1.0);
  lua_pushnumber(L, 1.0);
  return 2;
}

int OcScreen::lGetKeyboards(lua_State* L) {
  lua_newtable(L);
  return 1;
}

int OcScreen::lSetPrecise(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  s->precise_ = lua_toboolean(L, 1) != 0;
  return 0;
}

int OcScreen::lIsPrecise(lua_State* L) {
  auto* s = self(L);
  lua_pushboolean(L, s != nullptr && s->precise_);
  return 1;
}

void OcScreen::writeStringForTest(int x, int y, const char* text, bool vertical) {
  if (text == nullptr) return;
  const std::size_t len = std::strlen(text);
  int cx = x;
  int cy = y;
  for (std::size_t i = 0; i < len;) {
    const std::uint16_t cp = ScreenBuffer::decodeUtf8(text, len, i);
    put(this, cx, cy, cp);
    const int step = (font_ != nullptr && font_->loaded()) ? font_->width(cp) : 1;
    if (vertical) {
      cy += (step > 0 ? step : 1);
      if (cy >= height_) break;
    } else {
      cx += (step > 0 ? step : 1);
      if (cx >= width_) break;
    }
  }
  touch();
}

void OcScreen::put(OcScreen* s, int cx, int cy, std::uint32_t v) {
  if (s == nullptr || cx < 0 || cy < 0 || cx >= s->width_ || cy >= s->height_) return;
  Cell& c = s->cells_[static_cast<std::size_t>(cy) * static_cast<std::size_t>(s->width_) +
                       static_cast<std::size_t>(cx)];
  c.value = v;
  c.fg = s->fgColor_;
  c.bg = s->bgColor_;
  c.fgPalette = s->fgPalette_;
  c.bgPalette = s->bgPalette_;
  // Writing into a cell that is the tail of a wide glyph breaks the pair.
  c.continuation = false;
}

void OcScreen::setMouseButton(int button, bool down) {
  if (button >= 1 && button <= 3) mouseDown_[button - 1] = down;
}

bool OcScreen::mouseButtonDown(int button) const {
  if (button < 1 || button > 3) return false;
  return mouseDown_[button - 1];
}

int OcScreen::lGetMousePosition(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  // OC reports 0,0 when the pointer is not over the screen, and is otherwise
  // 1-based like every other screen coordinate.
  if (s->mouseX_ < 0 || s->mouseY_ < 0) {
    lua_pushinteger(L, 0);
    lua_pushinteger(L, 0);
  } else {
    lua_pushinteger(L, s->mouseX_ + 1);
    lua_pushinteger(L, s->mouseY_ + 1);
  }
  return 2;
}

int OcScreen::lGetMouseButton(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  const int button = argInt(L, 1, 1);
  lua_pushboolean(L, s->mouseButtonDown(button));
  return 1;
}

int OcScreen::lSetTouchModeInverted(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  s->touchInverted_ = lua_toboolean(L, 1) != 0;
  return 0;
}

int OcScreen::lIsTouchModeInverted(lua_State* L) {
  auto* s = self(L);
  lua_pushboolean(L, s != nullptr && s->touchInverted_);
  return 1;
}

// ---------------------------------------------------------------------------
//  Component constructor
// ---------------------------------------------------------------------------

int OcScreen::luaComponent(lua_State* L) {
  // Same argument shape as screen_sdl2.lua: address, slot, width, height, tier
  // (apis/component.lua passes table.unpack(info, 2, info.n)).
  const int maxWidth = argInt(L, 3, 80);
  const int maxHeight = argInt(L, 4, 25);
  const int maxTier = argInt(L, 5, 3);
  // Our component list appends the panel's configured depth as a 6th argument.
  const int depth = lua_gettop(L) >= 6 ? argInt(L, 6, 8) : 8;

  auto* screen = new OcScreen();
  screen->configure(maxWidth, maxHeight, maxTier, depth);
  s_active = screen;

  // One screen per machine, so a single registry slot is enough for the host to
  // reach the pixel buffer.
  lua_pushlightuserdata(L, screen);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &kRegistryTag);

  // Every screen method lives on the proxy table.
  //
  // Upstream OCEmu splits these: cost-accounted pixel methods go in `cec` and
  // are reached with component.cecinvoke, the rest on the proxy via
  // component.invoke. But OCEmu never exports cecinvoke into the guest
  // environment, so anything placed there is unreachable from Lua. Exposing one
  // complete table on both keeps component.invoke working (which is what the
  // guest's component.screen.* actually uses) while still satisfying anything
  // that expects the cost-accounted shape.
  lua_newtable(L);
  // The component must advertise the REAL OpenComputers type. apis/component.lua
  // does `proxy.type = proxy.type or info[1]`, so setting it here stops the
  // internal name ("screen_native") leaking into component.list/type, which is
  // what OpenOS matches against.
  lua_pushstring(L, "screen");
  lua_setfield(L, -2, "type");

  struct { const char* name; lua_CFunction fn; } objFns[] = {
      {"getForeground", &OcScreen::lGetForeground},
      {"setForeground", &OcScreen::lSetForeground},
      {"getBackground", &OcScreen::lGetBackground},
      {"setBackground", &OcScreen::lSetBackground},
      {"getDepth", &OcScreen::lGetDepth},
      {"setDepth", &OcScreen::lSetDepth},
      {"maxDepth", &OcScreen::lMaxDepth},
      {"getResolution", &OcScreen::lGetResolution},
      {"setResolution", &OcScreen::lSetResolution},
      {"maxResolution", &OcScreen::lMaxResolution},
      {"getPaletteColor", &OcScreen::lGetPaletteColor},
      {"setPaletteColor", &OcScreen::lSetPaletteColor},
      {"get", &OcScreen::lGet},
      {"set", &OcScreen::lSet},
      {"fill", &OcScreen::lFill},
      {"copy", &OcScreen::lCopy},
      {"bitblt", &OcScreen::lBitblt},
      {"clear", &OcScreen::lClear},
      {"isTouchModeInverted", &OcScreen::lIsTouchModeInverted},
      {"setTouchModeInverted", &OcScreen::lSetTouchModeInverted},
      {"isPrecise", &OcScreen::lIsPrecise},
      {"setPrecise", &OcScreen::lSetPrecise},
      {"turnOn", &OcScreen::lTurnOn},
      {"turnOff", &OcScreen::lTurnOff},
      {"isOn", &OcScreen::lIsOn},
      {"getAspectRatio", &OcScreen::lGetAspectRatio},
      {"getKeyboards", &OcScreen::lGetKeyboards},
      {"getMousePosition", &OcScreen::lGetMousePosition},
      {"getMouseButton", &OcScreen::lGetMouseButton},
  };
  // Registered raw: apis/component.lua already wraps every proxy call in
  // pcall(), so a mistyped argument surfaces as a normal catchable error rather
  // than unwinding through the host.
  for (const auto& f : objFns) {
    lua_pushcfunction(L, f.fn);
    lua_setfield(L, -2, f.name);
  }

  // Keep a reference so the same table can be returned as `cec` too.
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__self");

  // mai: method metadata; apis/component.lua fills in the defaults
  lua_newtable(L);

  // di: device info
  lua_newtable(L);
  lua_pushstring(L, "screen");
  lua_setfield(L, -2, "class");
  lua_pushstring(L, "Screen");
  lua_setfield(L, -2, "description");
  lua_pushstring(L, "MightyPirates GmbH & Co. KG");
  lua_setfield(L, -2, "vendor");
  lua_pushstring(L, ("MP" + std::to_string(maxTier * 1000) + " GTZ").c_str());
  lua_setfield(L, -2, "product");
  lua_pushinteger(L, screen->maxWidth() * screen->maxHeight());
  lua_setfield(L, -2, "capacity");

  // Return (proxy, cec, mai, di) with cec aliasing the proxy table.
  // Stack here is [obj][mai][di]; -3 is obj, not -4 (which does not exist).
  lua_pushvalue(L, -3);
  lua_insert(L, -3);  // -> [obj][obj][mai][di]
  return 4;
}

}  // namespace ocemu

// ---------------------------------------------------------------------------
//  Glyph sourcing for the emulated screen.
//
//  The character matrix needs a real monospace font. Rather than shipping a
//  binary asset we look for a system monospace TTF (DejaVu Sans Mono,
//  Liberation Mono, Noto Sans Mono, ... all present on CachyOS) and bake it
//  into ImGui's font atlas at high resolution with oversampling, so the cells
//  stay crisp at every zoom level. If no font is found we fall back to ImGui's
//  built-in ProggyClean, which is also monospaced.
//
//  Unicode block/shade elements (U+2580..U+259F) are drawn as rectangles
//  rather than glyphs: they are solid geometric shapes, and this keeps them
//  pixel-exact at 1-bit depth where a font glyph would be mush.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>

#include <imgui.h>

#include "ocemu/Tiers.hpp"

namespace ocemu {

class GlyphAtlas {
 public:
  // Adds the UI font and the large screen font to ImGui's atlas. Must be called
  // after ImGui_ImplOpenGL3_Init / before ImGui_ImplOpenGL3_CreateFontsTexture.
  // Returns false when neither a system font nor the built-in font could be
  // added (ImGui's built-in always succeeds, so this is effectively fatal-only).
  bool configure(float screenFontPx);

  [[nodiscard]] const std::string& fontPath() const { return fontPath_; }
  [[nodiscard]] bool usingSystemFont() const { return !fontPath_.empty(); }

  [[nodiscard]] ImFont* screenFont() const { return screenFont_; }
  [[nodiscard]] ImFont* uiFont() const { return uiFont_; }

  // Cell metrics derived from the screen font's monospace advance and pixel size.
  [[nodiscard]] float cellWidth() const;
  [[nodiscard]] float cellHeight() const;
  // Advance width of a single cell in the screen font. ImFont has no top-level
  // advance field, so this is read off a representative glyph.
  [[nodiscard]] float advanceX() const;

  // How a glyph is fitted into a cell of the given size.
  //
  // The atlas is baked once at a large fixed size and every frame the grid is
  // scaled down to fit the viewport, so the glyph MUST be scaled by the same
  // factor as the cell. Getting this wrong draws every character at the baked
  // size into a much smaller cell, which shows up as overlapping rows.
  struct CellMetrics {
    float fontSize = 0.0f;  // size to hand to ImDrawList::AddText
    float glyphW = 0.0f;   // scaled advance width of one glyph
    float offsetX = 0.0f;  // centring offset inside the cell
    float offsetY = 0.0f;
  };

  // Pure geometry, free of any font atlas state so it can be unit tested.
  //
  //   baseFontSize -- SizePixels the atlas was baked at
  //   baseAdvance  -- unscaled advance width of one glyph
  //
  // The returned fontSize always equals cellH, which is what guarantees that
  // adjacent rows cannot overlap no matter how far the grid is scaled down.
  [[nodiscard]] static CellMetrics fitToCell(float cellW, float cellH, float baseFontSize,
                                             float baseAdvance) {
    CellMetrics m;
    if (cellH <= 0.0f || baseFontSize <= 0.0f) return m;

    // AddText's size argument is relative to the baked size, so scaling by cellH
    // makes the glyph box exactly one cell tall: adjacent rows cannot overlap
    // however far the grid is scaled down.
    m.fontSize = cellH;
    m.glyphW = baseAdvance * (cellH / baseFontSize);

    // Centre the (monospaced) glyph horizontally; vertically it fills the cell.
    m.offsetX = (cellW - m.glyphW) * 0.5f;
    m.offsetY = (cellH - m.fontSize) * 0.5f;
    return m;
  }

  [[nodiscard]] CellMetrics cellMetrics(float cellW, float cellH) const;

  // Probes the usual CachyOS font locations for a monospace face.
  static std::string findMonospaceFont();

  // Codepoint range baked into the screen font: ASCII, Latin-1, Latin
  // Extended-A, box drawing and the block/shade elements.
  static const ImWchar* codepointRanges();

 private:
  ImFont* screenFont_ = nullptr;
  ImFont* uiFont_ = nullptr;
  std::string fontPath_;
};

// Draws one character cell (background quad + glyph) into `dl`.
void drawCharacterCell(ImDrawList* dl, const GlyphAtlas& atlas, const ImVec2& cellTopLeft,
                       float cellW, float cellH, std::uint16_t codepoint, Rgb fg, Rgb bg);

// Same, but with the glyph metrics supplied by the caller.
//
// The metrics depend only on the cell size, which is constant across a frame, so
// computing them per cell meant calling fitToCell()/advanceX() once for every
// cell on screen. Callers that draw a grid should compute them once and use this.
void drawCharacterCell(ImDrawList* dl, const GlyphAtlas& atlas, const ImVec2& cellTopLeft,
                       float cellW, float cellH, std::uint16_t codepoint, Rgb fg, Rgb bg,
                       const GlyphAtlas::CellMetrics& metrics);

}  // namespace ocemu
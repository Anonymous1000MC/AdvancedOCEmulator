#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ocemu {

// OpenComputers' built-in bitmap font, loaded from OCEmu's font.hex.
//
// Encoding: one line per code point, "<hex code>:<hex bitmap>". The bitmap is
// column-major bits, most significant bit first, 16 pixels tall:
//   32 hex chars = 128 bits = 8 wide  -> half-width (getCharWidth() == 1)
//   64 hex chars = 256 bits = 16 wide -> full-width (getCharWidth() == 2)
// Anything outside the file is treated as a blank half-width glyph.
class OcFont {
 public:
  static constexpr int kGlyphHeight = 16;
  static constexpr int kCellWidth = 8;

  // Loads font.hex. Returns false (and stays blank) if the file is missing.
  bool load(const std::string& path);

  // Width of `code` in cells: 1 or 2.
  [[nodiscard]] int width(std::uint32_t code) const;

  // True when the glyph should be drawn spanning two cells.
  [[nodiscard]] bool isWide(std::uint32_t code) const;

  // Copies the glyph's rows into `out` (kGlyphHeight entries of kCellWidth*2
  // bytes, 1 = set). Returns the width actually written (1 or 2).
  int bitmap(std::uint32_t code, std::vector<std::uint8_t>* out) const;

  [[nodiscard]] bool loaded() const { return loaded_; }

 private:
  // Glyphs are stored as decoded bitsets: kGlyphHeight rows of up to 16 px.
  struct Glyph {
    std::uint16_t rows[kGlyphHeight] = {};
    int width = 1;
  };

  std::vector<Glyph> glyphs_;
  bool loaded_ = false;
};

}  // namespace ocemu

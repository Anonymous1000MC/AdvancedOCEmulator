// ---------------------------------------------------------------------------
//  The emulated character-matrix display.
//
//  Holds one Cell (glyph + fg + bg) per character position. The buffer is sized
//  by the active TierSpec; when the Lua side asks for a resolution the request
//  is clamped to those limits and the clamping is reported back to Lua so the
//  guest can adapt (that is exactly how the real machine behaves when you
//  downgrade a graphics card).
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ocemu/Tiers.hpp"

namespace ocemu {

struct Cell {
  std::uint16_t glyph = ' ';  // Unicode codepoint
  Rgb fg{255, 255, 255};
  Rgb bg{0, 0, 0};
};

class ScreenBuffer {
 public:
  // (Re)allocate the buffer to exactly the tier limits.
  void applyLimits(const TierSpec& limits);
  void applyLimits(int gpuTier, int screenTier);

  [[nodiscard]] const TierSpec& limits() const { return limits_; }

  // Guest-requested resolution. Returns true when honoured verbatim, false
  // when the request had to be clamped down to the tier limits.
  bool requestResolution(int cols, int rows, int depth);

  [[nodiscard]] int requestedCols() const { return req_.cols; }
  [[nodiscard]] int requestedRows() const { return req_.rows; }
  [[nodiscard]] int requestedDepth() const { return req_.depth; }
  [[nodiscard]] bool requestWasClamped() const { return reqClamped_; }

  [[nodiscard]] int cols() const { return active_.cols; }
  [[nodiscard]] int rows() const { return active_.rows; }
  [[nodiscard]] int depth() const { return active_.depth; }
  [[nodiscard]] std::size_t cellCount() const { return cells_.size(); }
  [[nodiscard]] bool valid() const { return !cells_.empty(); }

  void clear();
  void setCell(int x, int y, std::uint16_t glyph, Rgb fg, Rgb bg);
  [[nodiscard]] const Cell& cell(int x, int y) const;

  void setPalette(Rgb fg, Rgb bg) { palFg_ = fg; palBg_ = bg; }
  [[nodiscard]] Rgb paletteFg() const { return palFg_; }
  [[nodiscard]] Rgb paletteBg() const { return palBg_; }

  [[nodiscard]] int cursorX() const { return curX_; }
  [[nodiscard]] int cursorY() const { return curY_; }
  void setCursor(int x, int y);

  // Write one codepoint at the cursor, wrapping and scrolling as needed.
  void putChar(std::uint16_t codepoint);
  // UTF-8 aware; handles \n, \r and \t control codes.
  void writeUtf8(const char* data, std::size_t len);
  void writeUtf8(const std::string& s) { writeUtf8(s.data(), s.size()); }

  void scrollUp(int lines);

  // Bumped on every mutation so the renderer can cheaply detect changes.
  [[nodiscard]] std::uint64_t revision() const { return revision_; }
  [[nodiscard]] std::uint64_t lastDrawnRevision() const { return drawnRevision_; }
  void markDrawn() { drawnRevision_ = revision_; }
  [[nodiscard]] bool needsRedraw() const { return revision_ != drawnRevision_; }

  // Force the renderer to repaint without touching the contents.
  void invalidate() { ++revision_; }

  [[nodiscard]] Rgb quantize(Rgb c) const { return quantizeColor(c, active_.depth); }

  // Decode one UTF-8 sequence; advances `i`. Invalid bytes yield U+FFFD.
  static std::uint16_t decodeUtf8(const char* data, std::size_t len, std::size_t& i);

 private:
  void resizeActive(int cols, int rows);

  TierSpec limits_{80, 25, 4};
  TierSpec active_{80, 25, 4};
  TierSpec req_{80, 25, 4};
  bool reqClamped_ = false;

  std::vector<Cell> cells_;
  Rgb palFg_{255, 255, 255};
  Rgb palBg_{0, 0, 0};
  int curX_ = 0;
  int curY_ = 0;
  std::uint64_t revision_ = 1;
  std::uint64_t drawnRevision_ = 0;
};

}  // namespace ocemu
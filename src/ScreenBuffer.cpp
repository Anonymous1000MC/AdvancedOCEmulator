#include "ocemu/ScreenBuffer.hpp"

#include <algorithm>
#include <cstring>

namespace ocemu {

void ScreenBuffer::applyLimits(const TierSpec& limits) {
  limits_ = limits;
  active_ = limits;
  req_ = limits;
  reqClamped_ = false;
  resizeActive(limits.cols, limits.rows);
  clear();
}

void ScreenBuffer::applyLimits(int gpuTier, int screenTier) {
  applyLimits(resolveDisplayLimits(gpuTier, screenTier));
}

void ScreenBuffer::resizeActive(int cols, int rows) {
  cols = std::max(1, cols);
  rows = std::max(1, rows);
  cells_.assign(static_cast<std::size_t>(cols) * static_cast<std::size_t>(rows), Cell{});
  active_.cols = cols;
  active_.rows = rows;
  curX_ = 0;
  curY_ = 0;
  ++revision_;
}

bool ScreenBuffer::requestResolution(int cols, int rows, int depth) {
  // Clamp the guest's wish to what the installed GPU + screen actually grant.
  const int wantCols = std::clamp(cols, 1, limits_.cols);
  const int wantRows = std::clamp(rows, 1, limits_.rows);
  const int wantDepth = std::clamp(depth, 1, limits_.depth);

  req_ = TierSpec{wantCols, wantRows, wantDepth};
  reqClamped_ = (wantCols != cols) || (wantRows != rows) || (wantDepth != depth);

  const bool geomChanged = (active_.cols != wantCols) || (active_.rows != wantRows);
  const bool depthChanged = (active_.depth != wantDepth);

  if (geomChanged) {
    resizeActive(wantCols, wantRows);
    clear();
  }
  if (depthChanged) {
    active_.depth = wantDepth;
    // Re-snap every stored colour onto the new palette rather than throwing the
    // contents away: downgrading a card should dim the picture, not blank it.
    for (Cell& c : cells_) {
      c.fg = quantizeColor(c.fg, active_.depth);
      c.bg = quantizeColor(c.bg, active_.depth);
    }
    ++revision_;
  }
  return !reqClamped_;
}

void ScreenBuffer::clear() {
  const Cell blank{' ', palFg_, palBg_};
  std::fill(cells_.begin(), cells_.end(), blank);
  curX_ = 0;
  curY_ = 0;
  ++revision_;
}

void ScreenBuffer::setCell(int x, int y, std::uint16_t glyph, Rgb fg, Rgb bg) {
  if (cells_.empty() || x < 0 || y < 0 || x >= cols() || y >= rows()) return;
  Cell& c = cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(cols()) +
                    static_cast<std::size_t>(x)];
  c.glyph = glyph;
  c.fg = quantize(fg);
  c.bg = quantize(bg);
  ++revision_;
}

const Cell& ScreenBuffer::cell(int x, int y) const {
  static const Cell kFallback{};
  if (cells_.empty() || x < 0 || y < 0 || x >= cols() || y >= rows()) return kFallback;
  return cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(cols()) +
                static_cast<std::size_t>(x)];
}

void ScreenBuffer::setCursor(int x, int y) {
  if (cells_.empty()) return;
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= cols()) x = cols() - 1;
  if (y >= rows()) y = rows() - 1;
  curX_ = x;
  curY_ = y;
}

void ScreenBuffer::putChar(std::uint16_t codepoint) {
  if (cells_.empty()) return;

  if (curX_ < 0) curX_ = 0;
  if (curY_ < 0) curY_ = 0;
  if (curX_ >= cols()) curX_ = 0;
  if (curY_ >= rows()) curY_ = rows() - 1;

  setCell(curX_, curY_, codepoint, palFg_, palBg_);

  if (++curX_ >= cols()) {
    curX_ = 0;
    if (++curY_ >= rows()) {
      scrollUp(1);
      curY_ = rows() - 1;
    }
  }
}

void ScreenBuffer::scrollUp(int lines) {
  if (lines <= 0 || cells_.empty()) return;
  const std::size_t w = static_cast<std::size_t>(cols());
  const std::size_t total = w * static_cast<std::size_t>(rows());

  if (static_cast<std::size_t>(lines) >= static_cast<std::size_t>(rows())) {
    clear();
    return;
  }

  const std::size_t n = w * static_cast<std::size_t>(lines);
  std::memmove(cells_.data(), cells_.data() + n, (total - n) * sizeof(Cell));

  const Cell blank{' ', palFg_, palBg_};
  for (std::size_t i = total - n; i < total; ++i) cells_[i] = blank;
  ++revision_;
}

void ScreenBuffer::writeUtf8(const char* data, std::size_t len) {
  if (!data || len == 0 || cells_.empty()) return;

  std::size_t i = 0;
  while (i < len) {
    const char c = data[i];

    switch (c) {
      case '\n':
        setCursor(0, curY_ + 1);
        if (curY_ >= rows()) {
          scrollUp(1);
          setCursor(0, rows() - 1);
        }
        ++i;
        continue;
      case '\r':
        setCursor(0, curY_);
        ++i;
        continue;
      case '\t': {
        const int stop = ((curX_ / 8) + 1) * 8;
        while (curX_ < stop && curX_ < cols()) putChar(' ');
        ++i;
        continue;
      }
      case '\b':
        if (curX_ > 0) --curX_;
        ++i;
        continue;
      default:
        break;
    }

    // Drop any other C0 control character: printing them as glyphs would
    // produce garbage boxes in a terminal-oriented display.
    if (static_cast<unsigned char>(c) < 0x20) {
      ++i;
      continue;
    }

    const std::size_t before = i;
    const std::uint16_t cp = decodeUtf8(data, len, i);
    if (i == before) ++i;  // belt and braces: never spin on a malformed byte
    putChar(cp);
  }
}

std::uint16_t ScreenBuffer::decodeUtf8(const char* data, std::size_t len, std::size_t& i) {
  constexpr std::uint16_t kReplacement = 0xFFFD;
  const auto b0 = static_cast<unsigned char>(data[i]);

  if (b0 < 0x80) {
    ++i;
    return static_cast<std::uint16_t>(b0);
  }

  std::size_t extra = 0;
  std::uint32_t cp = 0;
  if ((b0 & 0xE0) == 0xC0) {
    extra = 1;
    cp = b0 & 0x1Fu;
  } else if ((b0 & 0xF0) == 0xE0) {
    extra = 2;
    cp = b0 & 0x0Fu;
  } else if ((b0 & 0xF8) == 0xF0) {
    extra = 3;
    cp = b0 & 0x07u;
  } else {
    ++i;  // continuation byte or 5/6-byte form: not valid UTF-8
    return kReplacement;
  }

  if (i + extra >= len) {
    ++i;
    return kReplacement;
  }

  for (std::size_t k = 1; k <= extra; ++k) {
    const auto bk = static_cast<unsigned char>(data[i + k]);
    if ((bk & 0xC0) != 0x80) {
      ++i;
      return kReplacement;
    }
    cp = (cp << 6) | (bk & 0x3Fu);
  }

  i += extra + 1;
  if (cp > 0xFFFF) return kReplacement;
  return static_cast<std::uint16_t>(cp);
}

}  // namespace ocemu
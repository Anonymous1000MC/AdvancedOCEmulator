#include "ocemu/OcFont.hpp"

#include <cstdio>
#include <cstdlib>
#include <utility>
#include <fstream>

namespace ocemu {
namespace {

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Returns bit `index` of a hex-encoded bitmap, counting from the most
// significant bit of the first digit.
//
// This deliberately does NOT accumulate the whole run into an integer: a glyph is
// up to 64 hex digits = 256 bits, which does not fit the 64 bits a uint64_t
// holds, and the high rows were being silently truncated.
int hexBit(const char* p, std::size_t digits, std::size_t index) {
  const std::size_t digit = index / 4;
  if (digit >= digits) return 0;
  const int d = hexDigit(p[digit]);
  if (d < 0) return 0;
  return (d >> (3 - (index % 4))) & 1;
}

}  // namespace

bool OcFont::load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;

  // Code points run past 0xFFFF (the file holds 75843 glyphs), so index by the
  // largest code seen rather than by character.
  std::uint32_t maxCode = 0;
  std::vector<std::pair<std::uint32_t, std::string>> raw;
  std::string line;
  while (std::getline(in, line)) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos || colon == 0) continue;
    const std::uint32_t code =
        static_cast<std::uint32_t>(std::strtoul(line.substr(0, colon).c_str(), nullptr, 16));
    std::string bits = line.substr(colon + 1);
    while (!bits.empty() && (bits.back() == '\r' || bits.back() == '\n')) bits.pop_back();
    if (bits.empty()) continue;
    if (code > maxCode) maxCode = code;
    raw.emplace_back(code, std::move(bits));
  }
  if (raw.empty()) return false;

  glyphs_.assign(static_cast<std::size_t>(maxCode) + 1, Glyph{});
  for (const auto& [code, bits] : raw) {
    Glyph& g = glyphs_[code];
    g.width = static_cast<int>(bits.size() / 32);  // 32 hex chars == one cell
    if (g.width < 1) g.width = 1;
    if (g.width > 2) g.width = 2;
    const std::size_t px = static_cast<std::size_t>(g.width) * kCellWidth;
    const std::size_t total = bits.size() * 4;
    for (int y = 0; y < kGlyphHeight; ++y) {
      std::uint16_t row = 0;
      for (std::size_t x = 0; x < px && x < 16; ++x) {
        const std::size_t idx = static_cast<std::size_t>(y) * px + x;
        if (idx >= total) break;
        if (hexBit(bits.data(), bits.size(), idx)) row |= static_cast<std::uint16_t>(1u << x);
      }
      g.rows[y] = row;
    }
  }
  loaded_ = true;
  return true;
}

int OcFont::width(std::uint32_t code) const {
  if (!loaded_ || code >= glyphs_.size()) return 1;
  return glyphs_[code].width;
}

bool OcFont::isWide(std::uint32_t code) const { return width(code) == 2; }

int OcFont::bitmap(std::uint32_t code, std::vector<std::uint8_t>* out) const {
  out->assign(static_cast<std::size_t>(kGlyphHeight) * kCellWidth * 2, 0);
  const int w = width(code);
  if (!loaded_ || code >= glyphs_.size()) return w;
  const Glyph& g = glyphs_[code];
  for (int y = 0; y < kGlyphHeight; ++y) {
    for (int x = 0; x < w * kCellWidth; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * kCellWidth * 2 +
                            static_cast<std::size_t>(x);
      (*out)[i] = static_cast<std::uint8_t>((g.rows[y] >> x) & 1u);
    }
  }
  return w;
}

}  // namespace ocemu

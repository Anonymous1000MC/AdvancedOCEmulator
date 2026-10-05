// ---------------------------------------------------------------------------
//  Tier tables and colour-depth handling.
//
//  The emulator models OpenComputers' tier ladder for both the graphics
//  adapter and the display panel. A tier grants three things:
//
//      * a character-matrix geometry (columns x rows)
//      * a colour palette depth, expressed in bits per colour channel
//
//  Tier 1 -> 50x16,  1-bit  (black/white, monochrome)
//  Tier 2 -> 80x25,  4-bit  (4096 colours)
//  Tier 3 -> 160x50, 8-bit  (16.7M colours)
//
//  The GPU and the screen are independent purchases, so the *effective* limit
//  is the intersection of the two: a Tier 1 GPU behind a Tier 3 monitor still
//  renders a 50x16 monochrome screen.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cstdint>

namespace ocemu {

struct TierSpec {
  int cols = 80;
  int rows = 25;
  int depth = 4;  // bits per colour channel
};

// Packed 8-bit-per-channel colour as stored in the framebuffer cells.
struct Rgb {
  std::uint8_t r = 255;
  std::uint8_t g = 255;
  std::uint8_t b = 255;
};

// Total by construction: an out-of-range tier clamps instead of falling
// through to the default case, so callers cannot get an arbitrary geometry.
[[nodiscard]] inline constexpr TierSpec tierSpec(int tier) {
  return tier <= 1   ? TierSpec{50, 16, 1}
         : tier == 2 ? TierSpec{80, 25, 4}
                     : TierSpec{160, 50, 8};
}

[[nodiscard]] inline const char* tierLabel(int tier) {
  switch (tier) {
    case 1:  return "Tier 1";
    case 2:  return "Tier 2";
    default: return "Tier 3";
  }
}

// Intersection of the GPU tier and the screen tier.
[[nodiscard]] inline constexpr TierSpec resolveDisplayLimits(int gpuTier, int screenTier) {
  const TierSpec gpu = tierSpec(gpuTier);
  const TierSpec scr = tierSpec(screenTier);
  return TierSpec{
      std::min(gpu.cols, scr.cols),
      std::min(gpu.rows, scr.rows),
      std::min(gpu.depth, scr.depth),
  };
}

[[nodiscard]] inline constexpr int clampTier(int tier) {
  return tier < 1 ? 1 : (tier > 3 ? 3 : tier);
}

// Snap a channel value onto the palette grid defined by `depth`.
[[nodiscard]] inline int quantizeChannel(int v, int depth) {
  v = std::clamp(v, 0, 255);
  if (depth >= 8) return v;
  const int levels = (1 << depth) - 1;  // 1-bit -> 1, 4-bit -> 15
  if (levels <= 0) return 0;
  const int q = (v * levels + 127) / 255;
  return (q * 255) / levels;
}

[[nodiscard]] inline Rgb quantizeColor(Rgb c, int depth) {
  return Rgb{
      static_cast<std::uint8_t>(quantizeChannel(c.r, depth)),
      static_cast<std::uint8_t>(quantizeChannel(c.g, depth)),
      static_cast<std::uint8_t>(quantizeChannel(c.b, depth)),
  };
}

[[nodiscard]] inline constexpr Rgb rgbFromU32(std::uint32_t v) {
  return Rgb{
      static_cast<std::uint8_t>((v >> 16) & 0xFFu),
      static_cast<std::uint8_t>((v >> 8) & 0xFFu),
      static_cast<std::uint8_t>(v & 0xFFu),
  };
}

[[nodiscard]] inline constexpr std::uint32_t packRgb(Rgb c) {
  return (static_cast<std::uint32_t>(c.r) << 16) |
         (static_cast<std::uint32_t>(c.g) << 8) |
         static_cast<std::uint32_t>(c.b);
}

[[nodiscard]] inline constexpr bool rgbEq(Rgb a, Rgb b) {
  return a.r == b.r && a.g == b.g && a.b == b.b;
}

}  // namespace ocemu
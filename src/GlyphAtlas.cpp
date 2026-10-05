#include "ocemu/GlyphAtlas.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <vector>

namespace ocemu {
namespace {

// Monospace faces that ship with CachyOS, best first.
const char* kFontCandidates[] = {
    "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    "/usr/share/fonts/liberation/LiberationMono-Regular.ttf",
    "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
    "/usr/share/fonts/Adwaita/AdwaitaMono-Regular.ttf",
    "/usr/share/fonts/TTF/LiberationMono-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
    "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
};

bool fileExists(const std::filesystem::path& p) {
  std::error_code ec;
  return std::filesystem::is_regular_file(p, ec);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

// Rasterise a dithered rectangle at `level` coverage (1..4 quarters) using one
// quad per horizontal run instead of one per pixel.
void fillCheckerRegion(ImDrawList* dl, float x0, float y0, float x1, float y1, int level,
                       ImU32 fg, ImU32 bg) {
  if (level <= 0) return;
  if (level >= 4) {
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), fg);
    return;
  }

  const int width = static_cast<int>(std::lround(x1 - x0));
  const int height = static_cast<int>(std::lround(y1 - y0));
  if (width <= 0 || height <= 0) return;

  for (int py = 0; py < height; ++py) {
    // Per-pixel coverage as a fraction of 4, summed over the row. Even rows
    // start offset, odd rows shifted by one, which yields the classic 50%
    // checkerboard at level 2.
    int filled = 0;
    for (int px = 0; px < width; ++px) {
      const int phase = (px + py) & 3;  // 0..3 repeating
      if (phase < level) ++filled;
    }
    if (filled <= 0) continue;

    // Compress into runs.
    int runStart = -1;
    for (int px = 0; px <= width; ++px) {
      const bool on = px < width && ((px + py) & 3) < level;
      if (on && runStart < 0) {
        runStart = px;
      } else if (!on && runStart >= 0) {
        dl->AddRectFilled(ImVec2(x0 + static_cast<float>(runStart), y0 + static_cast<float>(py)),
                          ImVec2(x0 + static_cast<float>(px), y0 + static_cast<float>(py) + 1.0f),
                          fg);
        runStart = -1;
      }
    }
    (void)filled;
  }
  (void)bg;
}

// Block and shade elements (U+2580..U+2593) are geometric, so drawing them as
// rectangles keeps them exact even at 1-bit depth where a hinted glyph would
// turn to mush.
bool drawBlockGlyph(ImDrawList* dl, const ImVec2& tl, float w, float h, std::uint16_t cp,
                    ImU32 fg, ImU32 bg) {
  const float midY = tl.y + h * 0.5f;
  const float midX = tl.x + w * 0.5f;

  switch (cp) {
    case 0x2588:  // full block
      dl->AddRectFilled(tl, ImVec2(tl.x + w, tl.y + h), fg);
      return true;
    case 0x2580:  // upper half
      dl->AddRectFilled(tl, ImVec2(tl.x + w, midY), fg);
      return true;
    case 0x2584:  // lower half
      dl->AddRectFilled(ImVec2(tl.x, midY), ImVec2(tl.x + w, tl.y + h), fg);
      return true;
    case 0x258C:  // left half
      dl->AddRectFilled(tl, ImVec2(midX, tl.y + h), fg);
      return true;
    case 0x2590:  // right half
      dl->AddRectFilled(ImVec2(midX, tl.y), ImVec2(tl.x + w, tl.y + h), fg);
      return true;
    case 0x2596:  // quadrant lower left
    case 0x2597:  // quadrant lower right
    case 0x2598:  // quadrant upper left
    case 0x2599:  // quadrant upper right
    case 0x259A:  // quadrant upper left and lower right
    case 0x259B: {  // quadrant upper right and lower left
      const float qw = w * 0.5f;
      const float qh = h * 0.5f;
      switch (cp) {
        case 0x2596: dl->AddRectFilled(ImVec2(tl.x, midY), ImVec2(midX, tl.y + h), fg); break;
        case 0x2597: dl->AddRectFilled(ImVec2(midX, midY), ImVec2(tl.x + w, tl.y + h), fg); break;
        case 0x2598: dl->AddRectFilled(tl, ImVec2(midX, midY), fg); break;
        case 0x2599: dl->AddRectFilled(ImVec2(midX, tl.y), ImVec2(tl.x + w, midY), fg); break;
        case 0x259A:
          dl->AddRectFilled(tl, ImVec2(midX, midY), fg);
          dl->AddRectFilled(ImVec2(midX, midY), ImVec2(tl.x + w, tl.y + h), fg);
          break;
        default:
          dl->AddRectFilled(ImVec2(midX, tl.y), ImVec2(tl.x + w, midY), fg);
          dl->AddRectFilled(ImVec2(tl.x, midY), ImVec2(midX, tl.y + h), fg);
          break;
      }
      (void)qw;
      (void)qh;
      return true;
    }
    case 0x2591:  // light shade
      fillCheckerRegion(dl, tl.x, tl.y, tl.x + w, tl.y + h, 1, fg, bg);
      return true;
    case 0x2592:  // medium shade
      fillCheckerRegion(dl, tl.x, tl.y, tl.x + w, tl.y + h, 2, fg, bg);
      return true;
    case 0x2593:  // dark shade
      fillCheckerRegion(dl, tl.x, tl.y, tl.x + w, tl.y + h, 3, fg, bg);
      return true;
    case 0x2594:  // upper one eighth block
      dl->AddRectFilled(tl, ImVec2(tl.x + w, tl.y + h * 0.125f), fg);
      return true;
    case 0x2595:  // right one eighth block
      dl->AddRectFilled(ImVec2(tl.x + w * 0.875f, tl.y), ImVec2(tl.x + w, tl.y + h), fg);
      return true;
    default:
      return false;
  }
}

int encodeUtf8(std::uint16_t cp, char* buf) {
  if (cp < 0x80) {
    buf[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    buf[0] = static_cast<char>(0xC0 | (cp >> 6));
    buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  buf[0] = static_cast<char>(0xE0 | (cp >> 12));
  buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
  return 3;
}

}  // namespace

std::string GlyphAtlas::findMonospaceFont() {
  // Explicit override wins, which is how a user points us at their own font.
  if (const char* env = std::getenv("OCEMU_FONT"); env != nullptr && *env != '\0') {
    if (fileExists(env)) return env;
  }

  for (const char* p : kFontCandidates) {
    if (fileExists(p)) return p;
  }

  // Last resort: scan the font tree for anything monospaced.
  std::error_code ec;
  for (const char* root : {"/usr/share/fonts", "/usr/local/share/fonts"}) {
    if (!std::filesystem::exists(root, ec)) continue;
    for (std::filesystem::recursive_directory_iterator it(root, ec), end; it != end;
         it.increment(ec)) {
      if (ec) break;
      if (!it->is_regular_file(ec)) continue;
      const std::string ext = lower(it->path().extension().string());
      if (ext != ".ttf" && ext != ".otf") continue;
      const std::string name = lower(it->path().filename().string());
      if (name.find("mono") != std::string::npos) return it->path().string();
    }
  }
  return {};
}

const ImWchar* GlyphAtlas::codepointRanges() {
  // Sorted, paired, NUL-terminated as ImFontAtlas expects.
  static const ImWchar kRanges[] = {
      0x0020, 0x00FF,  // Basic Latin + Latin-1 Supplement
      0x0100, 0x017F,  // Latin Extended-A
      0x2500, 0x259F,  // Box drawing, block elements, shades
      0,
  };
  return kRanges;
}

bool GlyphAtlas::configure(float screenFontPx) {
  ImGuiIO& io = ImGui::GetIO();

  // The UI font must be atlas index 0: that is what ImGui treats as default.
  uiFont_ = io.Fonts->AddFontDefault();
  if (uiFont_ == nullptr) return false;

  // Bake the screen face large and oversampled, then render it down to the
  // cell size at draw time. Downscaling a high-res atlas beats upscaling a
  // 13px bitmap when the cells are 16-40px on screen.
  ImFontConfig cfg;
  cfg.SizePixels = screenFontPx;
  cfg.OversampleH = 2;
  cfg.OversampleV = 1;
  cfg.PixelSnapH = true;

  fontPath_ = findMonospaceFont();
  if (!fontPath_.empty()) {
    screenFont_ = io.Fonts->AddFontFromFileTTF(fontPath_.c_str(), screenFontPx, &cfg,
                                               codepointRanges());
  }

  if (screenFont_ == nullptr) {
    // ProggyClean is also monospaced, so the grid stays aligned.
    ImFontConfig fallback;
    fallback.SizePixels = screenFontPx;
    screenFont_ = io.Fonts->AddFontDefault(&fallback);
  }

  return screenFont_ != nullptr;
}

float GlyphAtlas::advanceX() const {
  if (screenFont_ == nullptr) return 8.0f;
  // A monospaced face reports the same advance for every glyph, so any
  // representative character will do.
  const ImFontGlyph* g = screenFont_->FindGlyph(static_cast<ImWchar>('M'));
  if (g != nullptr && g->AdvanceX > 0.0f) return g->AdvanceX;
  return screenFont_->FontSize * 0.6f;
}

float GlyphAtlas::cellWidth() const {
  return std::max(1.0f, std::round(advanceX()));
}

GlyphAtlas::CellMetrics GlyphAtlas::cellMetrics(float cellW, float cellH) const {
  if (screenFont_ == nullptr) return CellMetrics{};
  return fitToCell(cellW, cellH, screenFont_->FontSize, advanceX());
}

float GlyphAtlas::cellHeight() const {
  if (screenFont_ == nullptr) return 16.0f;
  return std::max(1.0f, std::round(screenFont_->FontSize));
}

void drawCharacterCell(ImDrawList* dl, const GlyphAtlas& atlas, const ImVec2& cellTopLeft,
                       float cellW, float cellH, std::uint16_t codepoint, Rgb fg, Rgb bg) {
  drawCharacterCell(dl, atlas, cellTopLeft, cellW, cellH, codepoint, fg, bg,
                    atlas.cellMetrics(cellW, cellH));
}

void drawCharacterCell(ImDrawList* dl, const GlyphAtlas& atlas, const ImVec2& cellTopLeft,
                       float cellW, float cellH, std::uint16_t codepoint, Rgb fg, Rgb bg,
                       const GlyphAtlas::CellMetrics& m) {
  const ImVec2 br(cellTopLeft.x + cellW, cellTopLeft.y + cellH);
  const ImU32 bgCol = IM_COL32(bg.r, bg.g, bg.b, 255);
  const ImU32 fgCol = IM_COL32(fg.r, fg.g, fg.b, 255);

  // Always paint the cell: the guest owns the palette, including the idea that
  // a "space" can be any colour it likes.
  dl->AddRectFilled(cellTopLeft, br, bgCol);

  if (codepoint < 0x20) return;  // nothing meaningful to render

  if (drawBlockGlyph(dl, cellTopLeft, cellW, cellH, codepoint, fgCol, bgCol)) return;

  ImFont* font = atlas.screenFont();
  if (font == nullptr) return;

  if (m.fontSize <= 0.0f) return;

  char buf[4];
  const int n = encodeUtf8(codepoint, buf);

  dl->AddText(font, m.fontSize, ImVec2(cellTopLeft.x + m.offsetX, cellTopLeft.y + m.offsetY),
              fgCol, buf, buf + n);
}

}  // namespace ocemu
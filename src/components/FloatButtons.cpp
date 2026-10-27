#include "FloatButtons.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <cstdlib>

namespace float_ui {

namespace {
constexpr int kSlop = 8;
constexpr int kSlopBelow = 16;
constexpr int kHalo = 2;  // white outline around ink on a dither, in pixels

// Only ink pixels are set, so the page shows through the shadow. Denser near
// the shape, sparser further out, shifted down.
void drawShadow(const GfxRenderer& renderer, const int x, const int y, const int w, const int h) {
  constexpr int kSpread = 8;
  constexpr int kDrop = 3;
  const int r = std::min(w, h) / 2;
  const int halfW = w / 2 - r;
  const int halfH = h / 2 - r;
  const int cx = x + w / 2;
  const int cy = y + h / 2 + kDrop;
  for (int py = y - kSpread + kDrop; py < y + h + kSpread + kDrop; py++) {
    for (int px = x - kSpread; px < x + w + kSpread; px++) {
      const int qx = std::max(std::abs(px - cx) - halfW, 0);
      const int qy = std::max(std::abs(py - cy) - halfH, 0);
      const int d2 = qx * qx + qy * qy;
      if (d2 <= r * r) continue;  // under the shape itself
      bool ink = false;
      if (d2 <= (r + 3) * (r + 3)) {
        ink = (px + py) % 2 == 0;  // 50%
      } else if (d2 <= (r + 6) * (r + 6)) {
        ink = px % 2 == 0 && py % 2 == 0;  // 25%
      } else if (d2 <= (r + kSpread) * (r + kSpread)) {
        ink = px % 4 == 0 && py % 4 == 0;  // 6%
      }
      if (ink) renderer.drawPixel(px, py, true);
    }
  }
}
}  // namespace

bool Box::contains(const int px, const int py) const {
  return px >= x - kSlop && px < x + w + kSlop && py >= y - kSlop && py < y + h + kSlopBelow;
}

void drawShape(const GfxRenderer& renderer, const int x, const int y, const int w, const int h, const Fill fill) {
  const int r = std::min(w, h) / 2;
  if (fill == Fill::Outline) drawShadow(renderer, x, y, w, h);
  const bool dithered = fill == Fill::Dither || fill == Fill::Ring;
  const Color color = fill == Fill::Black ? Color::Black : dithered ? Color::LightGray : Color::White;
  if (dithered) renderer.fillRoundedRect(x, y, w, h, r, Color::White);  // dither sets ink pixels only
  renderer.fillRoundedRect(x, y, w, h, r, color);
  if (fill == Fill::Ring) {
    renderer.fillRoundedRect(x + kRingWidth, y + kRingWidth, w - 2 * kRingWidth, h - 2 * kRingWidth,
                             std::max(0, r - kRingWidth), Color::White);
  }
  if (fill == Fill::Outline) renderer.drawRoundedRect(x, y, w, h, 1, r, true);
}

// freeink::Icon rows are logical, not pre-rotated (GfxRenderer::drawIcon
// expects the themes' rotated legacy format).
void drawIcon(const GfxRenderer& renderer, const freeink::Icon& icon, const int x, const int y, const bool black,
              const bool halo) {
  const int rowBytes = (icon.w + 7) / 8;
  const auto ink = [&icon, rowBytes](const int row, const int col) {
    return ((icon.bits[row * rowBytes + (col >> 3)] >> (7 - (col & 7))) & 1) == 0;
  };
  if (halo) {
    for (int row = 0; row < icon.h; row++) {
      for (int col = 0; col < icon.w; col++) {
        if (!ink(row, col)) continue;
        for (int dy = -kHalo; dy <= kHalo; dy++) {
          for (int dx = -kHalo; dx <= kHalo; dx++) {
            if (std::abs(dx) + std::abs(dy) <= kHalo + 1) renderer.drawPixel(x + col + dx, y + row + dy, !black);
          }
        }
      }
    }
  }
  for (int row = 0; row < icon.h; row++) {
    for (int col = 0; col < icon.w; col++) {
      if (ink(row, col)) renderer.drawPixel(x + col, y + row, black);
    }
  }
}

void drawLabel(const GfxRenderer& renderer, const int fontId, const int x, const int y, const char* text,
               const Fill fill) {
  const bool black = inkOn(fill);
  if (fill == Fill::Dither) {
    for (int dy = -kHalo; dy <= kHalo; dy++) {
      for (int dx = -kHalo; dx <= kHalo; dx++) {
        if ((dx != 0 || dy != 0) && std::abs(dx) + std::abs(dy) <= kHalo + 1) {
          renderer.drawText(fontId, x + dx, y + dy, text, !black);
        }
      }
    }
  }
  renderer.drawText(fontId, x, y, text, black);
}

void drawRound(const GfxRenderer& renderer, const Box& box, const freeink::Icon& icon, const Fill fill) {
  drawShape(renderer, box.x, box.y, box.w, box.h, fill);
  drawIcon(renderer, icon, box.x + (box.w - icon.w) / 2, box.y + (box.h - icon.h) / 2, inkOn(fill),
           fill == Fill::Dither);
}

}  // namespace float_ui
